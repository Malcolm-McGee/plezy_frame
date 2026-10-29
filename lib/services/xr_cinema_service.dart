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

  /// Whether the headset may use its hardware video decoder. False for sources that decoder cannot handle
  /// (10-bit HEVC crashes its firmware), which are decoded in software instead.
  final bool hardwareDecoding;

  const XrCinemaRequest({
    required this.url,
    this.headers = const {},
    this.start = Duration.zero,
    this.title = '',
    this.environment = VrEnvironment.cinema,
    this.stereo = const Stereo3dInfo(Stereo3dLayout.none),
    this.volume,
    this.hardwareDecoding = true,
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

/// A tile in the VR menu was picked (menu mode).
class XrCinemaSelected extends XrCinemaEvent {
  final String id;
  const XrCinemaSelected(this.id);
}

/// The back button, or the controller's menu button on a screen that can go back (menu mode).
class XrCinemaBack extends XrCinemaEvent {
  const XrCinemaBack();
}

/// The viewer stopped playback from inside the headset and is back at the menu (menu mode).
class XrCinemaStopped extends XrCinemaEvent {
  const XrCinemaStopped();
}

/// One poster tile of a VR menu screen.
class XrMenuTile {
  final String id;
  final String title;
  final String subtitle;

  /// Path of a downloaded JPEG/PNG poster, or empty for a placeholder.
  final String imagePath;

  const XrMenuTile({required this.id, required this.title, this.subtitle = '', this.imagePath = ''});
}

/// Protocol fields are tab-separated and line-based, so neither may appear inside one.
String xrProtocolField(String value) => value.replaceAll(RegExp(r'[\t\r\n]+'), ' ').trim();

/// Parses one helper stdout line; null for anything unrecognised so a newer
/// helper's extra events never break an older app.
XrCinemaEvent? parseXrCinemaEvent(String line) {
  if (line.startsWith('select\t')) {
    final id = line.substring('select\t'.length).trimRight();
    return id.isEmpty ? null : XrCinemaSelected(id);
  }
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
    case 'back':
      return const XrCinemaBack();
    case 'stopped':
      return const XrCinemaStopped();
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

/// Command-line arguments for the helper's menu mode (no stream yet).
List<String> buildXrMenuArguments({required VrEnvironment environment}) => ['--menu', '--env', environment.wireName];

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
  final IOSink? _log;
  final StreamController<XrCinemaEvent> _events = StreamController<XrCinemaEvent>.broadcast();
  XrCinemaPosition? _lastPosition;
  bool _closed = false;

  XrCinemaSession._(this._process, this._log) {
    // ignore: prefer_initializing_formals
    _process.stdout.transform(utf8.decoder).transform(const LineSplitter()).listen((line) {
      // Position lines arrive twice a second; keep the log to everything else.
      if (!line.startsWith('position ')) _log?.writeln('[event] $line');
      final event = parseXrCinemaEvent(line);
      if (event == null) return;
      if (event is XrCinemaPosition) _lastPosition = event;
      if (!_events.isClosed) _events.add(event);
    }, onDone: _finish);
    // The helper logs diagnostics to stderr; keep them in the log file, which also stops the pipe filling up.
    _process.stderr.transform(utf8.decoder).listen((text) => _log?.write(text));
    _process.exitCode.then((code) {
      _log?.writeln('[exit] helper exited with code $code');
      return _log?.flush().whenComplete(() => _log.close());
    });
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

  /// Menu mode: replace the menu with a screen of poster tiles.
  void showScreen({required String title, required bool canGoBack, required List<XrMenuTile> tiles}) {
    send('ui.begin\t${xrProtocolField(title)}\t${canGoBack ? 1 : 0}');
    for (final tile in tiles) {
      send(
        'ui.tile\t${xrProtocolField(tile.id)}\t${xrProtocolField(tile.title)}\t${xrProtocolField(tile.subtitle)}'
        '\t${xrProtocolField(tile.imagePath)}',
      );
    }
    send('ui.end');
    // Testing aid: PLEZY_XR_DUMP_UI=/path/menu.png writes each screen to a PNG for inspection without a headset.
    final dump = Platform.environment['PLEZY_XR_DUMP_UI'];
    if (dump != null && dump.isNotEmpty) send('ui.dump\t${xrProtocolField(dump)}');
  }

  /// Menu mode: show a message ("Loading...") in place of the tiles.
  void showMessage(String message) => send('ui.status\t${xrProtocolField(message)}');

  /// Menu mode: play [request] on the cinema screen, replacing whatever is playing.
  void play(XrCinemaRequest request) {
    send('layout ${request.stereo.layout.wireName}');
    send('hwdec ${request.hardwareDecoding ? 'auto' : 'software'}');
    if (request.stereo.swapEyes) send('swap');
    if (request.volume != null) send('volume ${request.volume!.clamp(0, 100)}');
    for (final entry in request.headers.entries) {
      send('header ${xrProtocolField('${entry.key}: ${entry.value}')}');
    }
    if (request.start > Duration.zero) send('start ${(request.start.inMilliseconds / 1000).toStringAsFixed(3)}');
    send('load ${request.url}');
  }

  /// Menu mode: stop playback and return to the menu.
  void stopPlayback() => send('stop');

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

  /// Where the helper's output for the latest session is kept.
  static String get logPath {
    final state = Platform.environment['XDG_STATE_HOME'];
    final base = (state != null && state.isNotEmpty) ? state : '${Platform.environment['HOME'] ?? '.'}/.local/state';
    return '$base/plezy-xr.log';
  }

  static IOSink? _openLog() {
    try {
      final file = File(logPath)..parent.createSync(recursive: true);
      final sink = file.openWrite();
      sink.writeln('[start] ${DateTime.now().toIso8601String()}');
      return sink;
    } on Object {
      return null; // a log that cannot be written must not stop the cinema
    }
  }

  /// Starts the helper in menu mode: it opens the headset session straight away and waits for screens
  /// and streams. Throws [StateError] when it is not installed.
  static Future<XrCinemaSession> launchMenu({required VrEnvironment environment, XrProcessStarter? starter}) async {
    final path = helperPath;
    if (path == null && starter == null) throw StateError('The VR cinema helper (plezy-xr) is not installed');
    final start = starter ?? (exe, args) => Process.start(exe, args);
    final process = await start(path ?? 'plezy-xr', buildXrMenuArguments(environment: environment));
    return XrCinemaSession._(process, starter == null ? _openLog() : null);
  }

  /// Starts the helper. Throws [StateError] when it is not installed.
  static Future<XrCinemaSession> launch(XrCinemaRequest request, {XrProcessStarter? starter}) async {
    final path = helperPath;
    if (path == null && starter == null) throw StateError('The VR cinema helper (plezy-xr) is not installed');
    final start = starter ?? (exe, args) => Process.start(exe, args);
    final process = await start(path ?? 'plezy-xr', buildXrCinemaArguments(request));
    return XrCinemaSession._(process, starter == null ? _openLog() : null);
  }
}
