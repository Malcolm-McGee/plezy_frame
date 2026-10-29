part of '../../video_player_screen.dart';

extension _VideoPlayerVrCinemaMethods on VideoPlayerScreenState {
  /// Whether titles should open straight in the VR cinema: the setting, or the app
  /// having been started as a VR program by the launcher.
  bool get _vrAutoEnter =>
      XrCinemaService.isSupported &&
      (XrCinemaService.launchedForVr || SettingsService.instance.read(SettingsService.vrAutoEnter));

  /// Called when the first frame is up. Opens the cinema once per screen, never for live TV.
  void _maybeAutoEnterVrCinema() {
    if (!_vrAutoEnter || _vrAutoEntered || widget.isLive || _vrSession != null) return;
    _vrAutoEntered = true;
    unawaited(_enterVrCinema(leaveWhenDone: true));
  }

  /// Hands the current stream to the VR cinema helper (Steam Frame and other
  /// OpenXR headsets), pauses flat playback while it runs and resumes it where
  /// the headset left off.
  Future<void> _enterVrCinema({bool leaveWhenDone = false}) async {
    final currentPlayer = player;
    final media = _lastOpenedMedia;
    if (_vrSession != null || currentPlayer == null || media == null || !mounted) return;

    final settings = SettingsService.instance;
    // The player reports film time; the helper plays the stream, which a transcode may start mid-film.
    final timelineOffset = currentPlayer.timelineOffset;
    final position = currentPlayer.state.position;
    final streamStart = position > timelineOffset ? position - timelineOffset : Duration.zero;
    final wasPlaying = currentPlayer.state.playing;
    await currentPlayer.pause();

    final stereo = settings.read(SettingsService.vrAutoDetect3d)
        ? await _detectStereo3d(currentPlayer)
        : const Stereo3dInfo(Stereo3dLayout.none);

    final request = XrCinemaRequest(
      url: media.uri,
      headers: media.headers ?? const {},
      start: streamStart,
      title: widget.metadata.displayTitle,
      environment: settings.read(SettingsService.vrEnvironment),
      stereo: stereo,
      volume: currentPlayer.state.volume.round(),
      hardwareDecoding: VrMenuController.instance.hardwareDecodingForNext,
    );

    // VR is the app: the headset menu already has a session, so play on that one. Nothing is shown in the
    // (invisible) window; when the viewer stops or the film ends, hand the position back and leave this screen.
    final menu = VrMenuController.instance;
    if (menu.session != null) {
      final last = await menu.playAndWait(request);
      if (!mounted) return;
      final livePlayer = player;
      if (last != null && livePlayer != null) await livePlayer.seek(last.position + timelineOffset);
      if (!mounted) return;
      await _handleBackButton();
      return;
    }

    final XrCinemaSession session;
    try {
      session = await XrCinemaService.launch(request);
    } on Object catch (e) {
      appLogger.e('VR cinema failed to start', error: e);
      if (mounted) showErrorSnackBar(context, t.videoControls.vrCinemaFailed(message: e.toString()));
      if (wasPlaying) await currentPlayer.play();
      return;
    }
    if (!mounted) {
      await session.stop();
      return;
    }
    _vrSession = session;

    String? failure;
    final exited = Completer<void>();
    final subscription = session.events.listen((event) {
      if (event is XrCinemaError) failure = event.message;
      if (event is XrCinemaExited && !exited.isCompleted) exited.complete();
    });

    // A modal stands in for the player on the desktop window while the headset
    // is on; closing it ends the session. It dismisses itself when VR exits.
    final dialogContext = Completer<BuildContext>(); // completes on the dialog's first build
    unawaited(
      showDialog<void>(
        context: context,
        barrierDismissible: false,
        builder: (ctx) {
          if (!dialogContext.isCompleted) dialogContext.complete(ctx);
          return AlertDialog(
            title: Text(t.videoControls.vrCinemaActiveTitle),
            content: Text(t.videoControls.vrCinemaActiveMessage),
            actions: [TextButton(onPressed: session.stop, child: Text(t.videoControls.vrCinemaExit))],
          );
        },
      ),
    );

    await exited.future;
    await subscription.cancel();
    _vrSession = null;
    // The helper can exit before the dialog's first frame; wait for the build
    // so it is never left stranded on screen.
    final ctx = await dialogContext.future
        .then<BuildContext?>((c) => c)
        .timeout(const Duration(seconds: 2), onTimeout: () => null);
    if (ctx != null && ctx.mounted) Navigator.of(ctx).pop();

    final last = session.lastPosition;
    if (!mounted) return;
    final livePlayer = player;
    if (livePlayer == null) return;
    if (last != null) await livePlayer.seek(last.position + timelineOffset);
    if (!mounted) return;
    if (leaveWhenDone && failure == null) {
      // Started straight into VR: leaving the headset means done watching, so go back to the library.
      await _handleBackButton();
      return;
    }
    if (!mounted) return;
    if (failure != null) {
      showErrorSnackBar(context, t.videoControls.vrCinemaFailed(message: failure!));
    }
    // Resume only if the viewer was watching, not paused, when they took the headset off.
    if (wasPlaying && (last == null || !last.paused)) await livePlayer.play();
  }

  /// Container metadata (via mpv), then the file name, decide how the eyes are packed.
  Future<Stereo3dInfo> _detectStereo3d(Player currentPlayer) async {
    Stereo3dInfo container = const Stereo3dInfo(Stereo3dLayout.none);
    int? frameWidth;
    int? frameHeight;
    try {
      final stereoIn = await currentPlayer.getProperty('video-params/stereo-in');
      frameWidth = int.tryParse(await currentPlayer.getProperty('video-params/w') ?? '');
      frameHeight = int.tryParse(await currentPlayer.getProperty('video-params/h') ?? '');
      container = stereo3dFromMpvStereoIn(stereoIn, frameWidth: frameWidth, frameHeight: frameHeight);
    } on Object catch (e) {
      appLogger.d('Could not read container stereo mode: $e');
    }
    final versions = _availableVersions;
    final version = (_effectiveSelectedMediaIndex >= 0 && _effectiveSelectedMediaIndex < versions.length)
        ? versions[_effectiveSelectedMediaIndex]
        : null;
    final fileName = version?.parts.map((p) => p.file).whereType<String>().firstOrNull;
    // File name first: it carries the frame packing, a bare title rarely does.
    return resolveStereo3d(
      container: container,
      // The library's name counts too: a library called "3D Movies" is the viewer saying what is in it.
      name: [fileName, widget.metadata.displayTitle, widget.metadata.libraryTitle].whereType<String>().join(' '),
      guessFromBare3dTag: true,
      frameWidth: frameWidth,
      frameHeight: frameHeight,
    );
  }

  Future<void> _stopVrCinemaOnDispose() async {
    final session = _vrSession;
    _vrSession = null;
    if (session != null) await session.stop();
  }
}
