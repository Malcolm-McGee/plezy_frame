import 'dart:async';
import 'dart:convert';
import 'dart:io';

import '../media/stereo_3d.dart';
import 'settings_service.dart' show VrEnvironment, VrEnvironmentWire;

/// What the VR cinema is asked to play.
class XrCinemaRequest {
  final String url;
  final Map<String, String> headers;
  final Duration start;
  final String title;
  final VrEnvironment environment;
  final Stereo3dInfo stereo;
  final int? volume;

  const XrCinemaRequest({
    required this.url,
    this.headers = const {},
    this.start = Duration.zero,
    this.title = '',
    this.environment = VrEnvironment.cinema,
    this.stereo = const Stereo3dInfo(Stereo3dLayout.none),
    this.volume,
  });
}

/// Events the `plezy-xr` helper prints on stdout (see `linux/xr/xr_protocol.h`).
sealed class XrCinemaEvent {
  const XrCinemaEvent();
}

class XrCinemaReady extends XrCinemaEvent {
  const XrCinemaReady();
}

class XrCinemaPosition extends XrCinemaEvent {
  final Duration position;
  final Duration duration;
  final bool paused;
  final String environment;
  final Stereo3dLayout layout;

  /// mpv's hardware decoder in use (`no` means software decoding).
  final String hwdec;

  /// Frames mpv has dropped so far; a rising count means the headset cannot keep up.
  final int droppedFrames;

  const XrCinemaPosition({
    required this.position,
    required this.duration,
    required this.paused,
    required this.environment,
    required this.layout,
    this.hwdec = 'no',
    this.droppedFrames = 0,
  });
}

class XrCinemaEnded extends XrCinemaEvent {
  const XrCinemaEnded();
}

class XrCinemaError extends XrCinemaEvent {
  final String message;
  const XrCinemaError(this.message);
}

class XrCinemaExited extends XrCinemaEvent {
  const XrCinemaExited();
}

/// Parses one helper stdout line; null for anything unrecognised so a newer
/// helper's extra events never break an older app.
XrCinemaEvent? parseXrCinemaEvent(String line) {
  final trimmed = line.trim();
  if (trimmed.isEmpty) return null;
  final space = trimmed.indexOf(' ');
  final verb = space < 0 ? trimmed : trimmed.substring(0, space);
  final rest = space < 0 ? '' : trimmed.substring(space + 1);
  switch (verb) {
    case 'ready':
      return const XrCinemaReady();
    case 'ended':
      return const XrCinemaEnded();
    case 'exit':
      return const XrCinemaExited();
    case 'error':
      return XrCinemaError(rest.isEmpty ? 'unknown error' : rest);
    case 'position':
      final parts = rest.split(' ');
      if (parts.length < 3) return null;
      final position = double.tryParse(parts[0]);
      final duration = double.tryParse(parts[1]);
      if (position == null || duration == null || !position.isFinite || !duration.isFinite) return null;
      return XrCinemaPosition(
        position: _seconds(position),
        duration: _seconds(duration),
        paused: parts[2] == '1',
        environment: parts.length > 3 ? parts[3] : '',
        layout: Stereo3dLayout.fromWireName(parts.length > 4 ? parts[4] : null),
        hwdec: parts.length > 5 ? parts[5] : 'no',
        droppedFrames: parts.length > 6 ? int.tryParse(parts[6]) ?? 0 : 0,
      );
  }
  return null;
}

Duration _seconds(double s) => Duration(milliseconds: (s < 0 ? 0 : s * 1000).round());

/// Command-line arguments for the helper.
List<String> buildXrCinemaArguments(XrCinemaRequest request) => [
  '--url',
  request.url,
  for (final e in request.headers.entries) ...['--header', '${e.key}: ${e.value}'],
  if (request.start > Duration.zero) ...['--start', (request.start.inMilliseconds / 1000).toStringAsFixed(3)],
  '--env',
  request.environment.wireName,
  '--layout',
  request.stereo.layout.wireName,
  if (request.stereo.swapEyes) '--swap-eyes',
  if (request.volume != null) ...['--volume', '${request.volume!.clamp(0, 100)}'],
  if (request.title.isNotEmpty) ...['--title', request.title],
];

/// Locates the helper: an explicit override, else beside the app executable
/// (where the bundle installs it). Pure so it can be tested.
String? resolveXrHelperPath({
  required String resolvedExecutable,
  required Map<String, String> environment,
  bool Function(String path)? exists,
}) {
  final fileExists = exists ?? (p) => File(p).existsSync();
  final override = environment['PLEZY_XR_BIN'];
  if (override != null && override.isNotEmpty && fileExists(override)) return override;
  final sep = Platform.pathSeparator;
  final slash = resolvedExecutable.lastIndexOf(sep);
  if (slash < 0) return null;
  final beside = '${resolvedExecutable.substring(0, slash)}${sep}plezy-xr';
  return fileExists(beside) ? beside : null;
}

/// A running VR cinema. Stop it with [stop]; it also ends itself when the user
/// leaves the headset or the stream finishes.
class XrCinemaSession {
  final Process _process;
  final StreamController<XrCinemaEvent> _events = StreamController<XrCinemaEvent>.broadcast();
  XrCinemaPosition? _lastPosition;
  bool _closed = false;

  XrCinemaSession._(this._process) {
    _process.stdout.transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
      final event = parseXrCinemaEvent(line);
      if (event == null) return;
      if (event is XrCinemaPosition) _lastPosition = event;
      if (!_events.isClosed) _events.add(event);
    }, onDone: _finish);
    // The helper logs diagnostics to stderr; drain it so it can never block.
    _process.stderr.drain<void>();
  }

  Stream<XrCinemaEvent> get events => _events.stream;

  /// Last position the helper reported, for resuming flat playback.
  XrCinemaPosition? get lastPosition => _lastPosition;

  /// Completes with the exit code once the helper has gone.
  Future<int> get exitCode => _process.exitCode;

  void _finish() {
    if (_closed) return;
    _closed = true;
    if (!_events.isClosed) {
      _events.add(const XrCinemaExited());
      _events.close();
    }
  }

  void send(String command) {
    if (_closed) return;
    try {
      _process.stdin.writeln(command);
    } on Object {
      // The helper is already gone; exit handling will report it.
    }
  }

  void setEnvironment(VrEnvironment environment) => send('env ${environment.wireName}');
  void setLayout(Stereo3dLayout layout) => send('layout ${layout.wireName}');
  void togglePlayback() => send('toggle');
  void seekTo(Duration position) => send('seek ${(position.inMilliseconds / 1000).toStringAsFixed(3)}');

  /// Asks the helper to leave VR, then kills it if it has not within [grace].
  Future<void> stop({Duration grace = const Duration(seconds: 3)}) async {
    send('quit');
    try {
      await _process.exitCode.timeout(grace);
    } on TimeoutException {
      _process.kill(ProcessSignal.sigterm);
    }
  }
}

typedef XrProcessStarter = Future<Process> Function(String executable, List<String> arguments);

class XrCinemaService {
  const XrCinemaService._();

  /// Whether this install can open the VR cinema: Linux with the helper present.
  /// The helper itself still reports an error when no headset or OpenXR runtime
  /// is active, so the button is offered wherever the helper exists.
  static bool get isSupported => Platform.isLinux && helperPath != null;

  /// True when the launcher started the app as a VR program (`PLEZY_VR=1`, set by
  /// `plezy-steamos.sh`): fullscreen, and every title opens straight in the cinema.
  static bool get launchedForVr => Platform.isLinux && Platform.environment['PLEZY_VR'] == '1';

  static String? get helperPath => Platform.isLinux
      ? resolveXrHelperPath(resolvedExecutable: Platform.resolvedExecutable, environment: Platform.environment)
      : null;

  /// Starts the helper. Throws [StateError] when it is not installed.
  static Future<XrCinemaSession> launch(XrCinemaRequest request, {XrProcessStarter? starter}) async {
    final path = helperPath;
    if (path == null && starter == null) throw StateError('The VR cinema helper (plezy-xr) is not installed');
    final start = starter ?? (exe, args) => Process.start(exe, args);
    final process = await start(path ?? 'plezy-xr', buildXrCinemaArguments(request));
    return XrCinemaSession._(process);
  }
}
