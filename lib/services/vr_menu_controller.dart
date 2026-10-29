import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter/foundation.dart' show visibleForTesting;
import 'package:flutter/widgets.dart' show BuildContext;

import 'package:crypto/crypto.dart';
import 'package:http/http.dart' as http;
import 'package:provider/provider.dart';

import '../main.dart' show rootNavigatorKey;
import '../navigation/profile_navigation_scope.dart';
import '../media/library_query.dart';
import '../media/media_item.dart';
import '../media/media_kind.dart';
import '../media/media_library.dart';
import '../media/media_version.dart';
import '../models/transcode_quality_preset.dart';
import '../media/media_server_client.dart';
import '../media/media_stream.dart';
import '../providers/multi_server_provider.dart';
import '../utils/app_logger.dart';
import '../utils/video_player_navigation.dart';
import 'settings_service.dart';
import 'video_decode_capabilities.dart';
import 'xr_cinema_service.dart';

/// How a source is played on the headset.
class VrPlaybackPlan {
  /// The quality to ask the server for, or null to play the original file.
  final TranscodeQualityPreset? preset;

  /// Whether the stream the headset receives (after any conversion) suits its hardware decoder.
  final bool hardwareDecoding;

  const VrPlaybackPlan({required this.preset, required this.hardwareDecoding});
}

/// One poster tile and what picking it does.
class _VrTile {
  final String id;
  final String title;
  final String subtitle;
  final String imageUrl;
  final Future<void> Function() onSelect;

  const _VrTile({
    required this.id,
    required this.title,
    required this.onSelect,
    this.subtitle = '',
    this.imageUrl = '',
  });
}

/// A menu screen: a title and the tiles it shows once [load] has run.
class _VrScreen {
  final String title;
  final Future<void> Function() load;
  final List<_VrTile> Function() tiles;

  const _VrScreen({required this.title, required this.load, required this.tiles});
}

/// Makes Plezy a VR program: the headset menu (library, shows, episodes) is the app, and this runs invisibly
/// behind it as the engine that talks to the media server and resolves streams.
///
/// It starts the `plezy-xr` helper in menu mode, builds screens from the user's servers, sends them to the
/// headset, and acts on what the viewer picks. Playing a title opens the ordinary player route (so server
/// negotiation, resume points and progress reporting all work as usual); that screen then hands the stream
/// to this session instead of showing anything.
class VrMenuController {
  VrMenuController._();
  static final VrMenuController instance = VrMenuController._();

  static const int _pageSize = 60;

  XrCinemaSession? _session;
  final List<_VrScreen> _stack = [];
  Map<String, _VrTile> _tiles = {};
  int _generation = 0;
  bool _busy = false;
  bool _exiting = false;
  String? _notice;
  Timer? _watchdog;

  /// Testing aid: PLEZY_VR_AUTOPICK="3d movies;#first;#stop:20" drives the menu without a controller. Each entry is
  /// applied to the next screen shown: text picks the first tile whose title contains it, `#first` the first
  /// title, and `#stop:N` stops playback N seconds after it starts.
  final List<String> _autoPicks = [
    for (final step in (Platform.environment['PLEZY_VR_AUTOPICK'] ?? '').split(';'))
      if (step.trim().isNotEmpty) step.trim(),
  ];

  /// Whether the title being started may use the hardware decoder; read by the player screen when it hands the
  /// stream over.
  bool hardwareDecodingForNext = true;

  /// Set by the player screen once it has handed a stream to the headset, so the start-up watchdog stands down.
  bool playbackStarted = false;

  /// The running menu session, or null when the app is not in VR mode.
  XrCinemaSession? get session => _session;

  /// Starts the headset menu. Safe to call once, at app start.
  Future<void> start() async {
    if (_session != null) return;
    final environment = SettingsService.instance.read(SettingsService.vrEnvironment);
    final XrCinemaSession session;
    try {
      session = await XrCinemaService.launchMenu(environment: environment);
    } on Object catch (e, st) {
      appLogger.e('VR menu could not start', error: e, stackTrace: st);
      return;
    }
    _session = session;
    session.events.listen(_onEvent);
    session.showMessage('Connecting to your servers...');

    final provider = await _waitForServers();
    if (provider == null) {
      session.showMessage('No media server is connected. Start Plezy normally once to sign in.');
      return;
    }
    // The home screen has to load its libraries before it can be shown.
    final home = _homeScreen();
    try {
      await home.load();
    } on Object catch (e, st) {
      appLogger.e('VR menu: loading the home screen failed', error: e, stackTrace: st);
      session.showMessage('Could not load your library. Check the server connection.');
      return;
    }
    _stack.add(home);
    await _showCurrent();
  }

  /// A context inside the signed-in profile's scope, where the player screen finds the state it needs (the
  /// playback provider and friends). The app's root navigator is above that scope, so a route pushed there
  /// would not see it; falls back to the root only before a profile is mounted.
  BuildContext? get _profileContext => profileNavigationRegistry.navigator?.context ?? rootNavigatorKey.currentContext;

  /// The server provider, once the app's widgets are mounted; null before that.
  MultiServerProvider? _provider() {
    final context = _profileContext;
    if (context == null) return null;
    try {
      return context.read<MultiServerProvider>();
    } on Object {
      return null; // providers not mounted yet
    }
  }

  Future<MultiServerProvider?> _waitForServers() async {
    for (var i = 0; i < 60; i++) {
      final provider = _provider();
      // Wait for the profile scope too: playing a title needs its navigator.
      if (provider != null &&
          profileNavigationRegistry.navigator != null &&
          provider.serverManager.visibleOnlineClients.isNotEmpty) {
        return provider;
      }
      await Future<void>.delayed(const Duration(milliseconds: 500));
    }
    return null;
  }

  Map<String, MediaServerClient> _clients() => _provider()?.serverManager.visibleOnlineClients ?? const {};

  void _onEvent(XrCinemaEvent event) {
    switch (event) {
      case XrCinemaSelected(:final id):
        unawaited(_select(id));
      case XrCinemaBack():
        unawaited(_back());
      case XrCinemaError(:final message):
        _notice = message;
      case XrCinemaExited():
        // The headset session is gone (left from SteamVR, or the helper crashed): the app has no screen to
        // return to, so end it.
        if (!_exiting) {
          _exiting = true;
          exit(0);
        }
      default:
        break;
    }
  }

  // ---- navigation ----

  Future<void> _select(String id) async {
    if (_busy) return;
    final tile = _tiles[id];
    if (tile == null) return;
    _busy = true;
    try {
      await tile.onSelect();
    } on Object catch (e, st) {
      appLogger.e('VR menu: picking "$id" failed', error: e, stackTrace: st);
      _session?.showMessage('That did not work. Press back to return.');
    } finally {
      _busy = false;
    }
  }

  Future<void> _back() async {
    if (_busy || _stack.length <= 1) return;
    _stack.removeLast();
    await _showCurrent();
  }

  Future<void> _push(_VrScreen screen) async {
    _session?.showMessage('Loading...');
    await screen.load();
    _stack.add(screen);
    await _showCurrent();
  }

  /// Sends the top screen to the headset, with its posters downloaded first so it appears complete.
  Future<void> _showCurrent() async {
    final session = _session;
    if (session == null || _stack.isEmpty) return;
    final generation = ++_generation;
    final screen = _stack.last;
    final tiles = screen.tiles();
    final posters = await _downloadPosters(tiles);
    if (generation != _generation) return; // the viewer moved on while posters were downloading

    _tiles = {for (final tile in tiles) tile.id: tile};
    final notice = _notice;
    _notice = null;
    session.showScreen(
      title: notice == null ? screen.title : '${screen.title}  -  $notice',
      canGoBack: _stack.length > 1,
      tiles: [
        for (var i = 0; i < tiles.length; i++)
          XrMenuTile(id: tiles[i].id, title: tiles[i].title, subtitle: tiles[i].subtitle, imagePath: posters[i]),
      ],
    );
    _runAutoPick(tiles);
  }

  void _runAutoPick(List<_VrTile> tiles) {
    if (_autoPicks.isEmpty || _autoPicks.first.startsWith('#stop:')) return;
    final step = _autoPicks.removeAt(0);
    final candidates = tiles.where((t) => t.id != 'exit' && t.id != 'more');
    final tile = step == '#first'
        ? candidates.firstOrNull
        : tiles.where((t) => t.title.toLowerCase().contains(step.toLowerCase())).firstOrNull;
    if (tile == null) return;
    appLogger.i('VR menu autopick: ${tile.title}');
    Future<void>.delayed(const Duration(seconds: 2), () => _select(tile.id));
  }

  // ---- screens ----

  _VrScreen _homeScreen() {
    final tiles = <_VrTile>[];
    return _VrScreen(
      title: 'Plezy',
      load: () async {
        tiles.clear();
        final clients = _clients();
        final continueWatching = <_VrTile>[];
        final libraryTiles = <_VrTile>[];
        for (final client in clients.values) {
          try {
            final items = await client.fetchContinueWatching(count: 20);
            continueWatching.addAll(items.map((item) => _itemTile(client, item)));
          } on Object catch (e) {
            appLogger.w('VR menu: continue watching failed for ${client.serverName}', error: e);
          }
          try {
            for (final library in await client.fetchLibraries()) {
              if (library.hidden || library.kind.isMusic || library.kind == MediaKind.photo) continue;
              libraryTiles.add(
                _VrTile(
                  id: 'lib:${client.serverId}:${library.id}',
                  title: library.title,
                  subtitle: clients.length > 1 ? (client.serverName ?? '') : '',
                  onSelect: () => _push(_libraryScreen(client, library)),
                ),
              );
            }
          } on Object catch (e) {
            appLogger.w('VR menu: libraries failed for ${client.serverName}', error: e);
          }
        }
        if (continueWatching.isNotEmpty) {
          tiles.add(
            _VrTile(
              id: 'continue',
              title: 'Continue Watching',
              subtitle: '${continueWatching.length} titles',
              onSelect: () =>
                  _push(_VrScreen(title: 'Continue Watching', load: () async {}, tiles: () => continueWatching)),
            ),
          );
        }
        tiles.addAll(libraryTiles);
        tiles.add(_VrTile(id: 'exit', title: 'Exit Plezy VR', onSelect: _exitApp));
      },
      tiles: () => tiles,
    );
  }

  _VrScreen _libraryScreen(MediaServerClient client, MediaLibrary library) {
    final loaded = <_VrTile>[];
    var total = 0;

    Future<void> nextPage() async {
      final page = await client.fetchLibraryPagedContent(
        library.id,
        query: LibraryQuery(
          offset: loaded.length,
          limit: _pageSize,
          sort: const LibrarySort(field: 'title', direction: LibrarySortDirection.ascending),
        ),
        libraryKind: library.kind,
      );
      total = page.totalCount;
      loaded.addAll(page.items.map((item) => _itemTile(client, item)));
    }

    return _VrScreen(
      title: library.title,
      load: nextPage,
      tiles: () => [
        ...loaded,
        if (loaded.length < total && loaded.isNotEmpty)
          _VrTile(
            id: 'more',
            title: 'Load more',
            subtitle: '${total - loaded.length} more',
            onSelect: () async {
              _session?.showMessage('Loading...');
              await nextPage();
              await _showCurrent();
            },
          ),
      ],
    );
  }

  _VrScreen _childrenScreen(MediaServerClient client, MediaItem parent) {
    final children = <_VrTile>[];
    return _VrScreen(
      title: parent.displayTitle,
      load: () async {
        children
          ..clear()
          ..addAll((await client.fetchChildren(parent.id)).map((item) => _itemTile(client, item)));
      },
      tiles: () => children,
    );
  }

  _VrTile _itemTile(MediaServerClient client, MediaItem item) {
    final subtitle = switch (item.kind) {
      MediaKind.movie => item.year?.toString() ?? '',
      _ => item.displaySubtitle ?? (item.year?.toString() ?? ''),
    };
    return _VrTile(
      id: 'item:${client.serverId}:${item.id}',
      title: item.displayTitle,
      subtitle: subtitle,
      imageUrl: client.thumbnailUrl(item.thumbPath, width: 300, height: 450),
      onSelect: () => item.kind.isVideo ? _play(client, item) : _push(_childrenScreen(client, item)),
    );
  }

  // ---- playback ----

  /// How to play a source on the headset: which quality to ask the server for (null: the original file),
  /// and whether the hardware decoder may be used.
  ///
  /// The decoder handles H.264 and 8-bit HEVC in hardware at any size. Its 10-bit HEVC path crashes the
  /// chip's firmware, so 10-bit sources are decoded in software, which keeps up to about 1080p. A source
  /// that is software-only and larger than that is converted by the server into 1080p H.264 (side-by-side 3D
  /// survives it), which the hardware decoder then handles.
  @visibleForTesting
  static VrPlaybackPlan planForSource({String? codec, int? width, int? height, int? bitDepth}) {
    final name = (codec ?? '').toLowerCase();
    if (name.isEmpty) return const VrPlaybackPlan(preset: null, hardwareDecoding: false); // unknown: be safe
    final isH264 = name == 'h264' || name == 'avc' || name == 'avc1';
    final isHevc = name == 'hevc' || name == 'h265';
    final hardware = (isH264 && (bitDepth ?? 8) <= 8) || (isHevc && bitDepth == 8);
    final heavy = (width ?? 0) > 2000 || (height ?? 0) > 1100;
    final convert = !hardware && heavy;
    return VrPlaybackPlan(
      preset: convert ? TranscodeQualityPreset.p1080_20mbps : null,
      hardwareDecoding: hardware || convert, // a converted stream is H.264
    );
  }

  /// Reads the title's video format from the server (codec, size, bit depth) and decides how to play it.
  Future<VrPlaybackPlan> _planFor(MediaServerClient client, MediaItem item) async {
    String? codec;
    int? width, height, bitDepth;
    try {
      final info = await client.getFileInfo(item);
      final streams = info?.versions.firstOrNull?.parts.firstOrNull?.streams ?? const [];
      final video = streams.where((s) => s.kind == MediaStreamKind.video).firstOrNull;
      codec = video?.codec;
      width = video?.width;
      height = video?.height;
      bitDepth = video?.bitDepth;
    } on Object catch (e) {
      appLogger.w('VR menu: could not read ${item.displayTitle} to plan playback', error: e);
    }
    if (codec == null) {
      // The list entry may still know the codec and size, though never the bit depth.
      final MediaVersion? version = item.mediaVersions?.firstOrNull;
      codec = version?.videoCodec;
      width ??= version?.width;
      height ??= version?.height;
    }
    final plan = planForSource(codec: codec, width: width, height: height, bitDepth: bitDepth);
    appLogger.i(
      'VR menu: ${item.displayTitle} is $codec ${width}x$height ${bitDepth ?? '?'}-bit: '
      '${plan.hardwareDecoding ? 'hardware' : 'software'} decoding'
      '${plan.preset == null ? '' : ', server converts to ${plan.preset!.name}'}',
    );
    return plan;
  }

  Future<void> _play(MediaServerClient client, MediaItem item) async {
    final session = _session;
    if (session == null) return;
    session.showMessage('Starting ${item.displayTitle}...');
    playbackStarted = false;
    final plan = await _planFor(client, item);
    hardwareDecodingForNext = plan.hardwareDecoding;
    // A conversion must come back as H.264: refuse the codecs the headset cannot decode, for this title only.
    VideoDecodeCapabilities.temporaryRefusals = plan.preset == null
        ? const {}
        : const {RankedVideoCodec.hevc, RankedVideoCodec.av1};
    final context = _profileContext; // read after the await: the tree may have changed meanwhile
    if (context == null) return;
    // If the player screen never reaches the headset (a failed open shows an error nobody can see), give up
    // and return to the menu instead of leaving the viewer on "Starting...".
    _watchdog?.cancel();
    _watchdog = Timer(const Duration(seconds: 60), () {
      if (playbackStarted) return;
      _notice = 'Could not start playback';
      profileNavigationRegistry.navigator?.maybePop();
    });
    try {
      // The context was read after the last await above and is used straight away.
      // ignore: use_build_context_synchronously
      await navigateToVideoPlayer(context, metadata: item, selectedQualityPreset: plan.preset);
    } finally {
      _watchdog?.cancel();
      VideoDecodeCapabilities.temporaryRefusals = const {}; // other titles are left alone
    }
    await _showCurrent(); // back at the menu, on the screen the title was picked from
  }

  /// Plays [request] on the headset's cinema screen and completes with the last known position once the
  /// viewer stops, the film ends, or the headset session goes away.
  Future<XrCinemaPosition?> playAndWait(XrCinemaRequest request) async {
    final session = _session;
    if (session == null) return null;
    playbackStarted = true;
    final done = Completer<void>();
    final subscription = session.events.listen((event) {
      if ((event is XrCinemaStopped || event is XrCinemaEnded || event is XrCinemaExited) && !done.isCompleted) {
        done.complete();
      }
    });
    session.play(request);
    if (_autoPicks.isNotEmpty && _autoPicks.first.startsWith('#stop:')) {
      final seconds = int.tryParse(_autoPicks.removeAt(0).substring('#stop:'.length)) ?? 20;
      Timer(Duration(seconds: seconds), session.stopPlayback);
    }
    await done.future;
    await subscription.cancel();
    return session.lastPosition;
  }

  Future<void> _exitApp() async {
    _exiting = true;
    await _session?.stop();
    exit(0);
  }

  // ---- posters ----

  Directory get _posterDirectory {
    final cache = Platform.environment['XDG_CACHE_HOME'];
    final base = (cache != null && cache.isNotEmpty) ? cache : '${Platform.environment['HOME'] ?? '.'}/.cache';
    return Directory('$base/plezy-xr/posters');
  }

  /// Downloads each tile's poster (a few at a time) and returns local paths, empty where there is none.
  Future<List<String>> _downloadPosters(List<_VrTile> tiles) async {
    final paths = List<String>.filled(tiles.length, '');
    try {
      _posterDirectory.createSync(recursive: true);
    } on Object {
      return paths;
    }
    const batch = 8;
    for (var start = 0; start < tiles.length; start += batch) {
      final end = (start + batch).clamp(0, tiles.length);
      await Future.wait([
        for (var i = start; i < end; i++) _fetchPoster(tiles[i].imageUrl).then((path) => paths[i] = path),
      ]);
    }
    return paths;
  }

  Future<String> _fetchPoster(String url) async {
    if (url.isEmpty) return '';
    final file = File('${_posterDirectory.path}/${sha1.convert(utf8.encode(url))}.img');
    try {
      if (file.existsSync() && file.lengthSync() > 0) return file.path;
      final response = await http.get(Uri.parse(url)).timeout(const Duration(seconds: 6));
      if (response.statusCode != 200 || response.bodyBytes.isEmpty) return '';
      await file.writeAsBytes(response.bodyBytes);
      return file.path;
    } on Object {
      return ''; // a missing poster shows a placeholder, never an error
    }
  }
}
