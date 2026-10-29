import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:plezy/media/stereo_3d.dart';
import 'package:plezy/models/transcode_quality_preset.dart';
import 'package:plezy/services/settings_service.dart';
import 'package:plezy/services/video_decode_capabilities.dart';
import 'package:plezy/services/vr_menu_controller.dart';
import 'package:plezy/services/xr_cinema_service.dart';

void main() {
  group('parseXrCinemaEvent', () {
    test('parses position lines', () {
      final e = parseXrCinemaEvent('position 12.500 3600.000 1 lounge sideBySideHalf');
      expect(e, isA<XrCinemaPosition>());
      final p = e! as XrCinemaPosition;
      expect(p.position, const Duration(milliseconds: 12500));
      expect(p.duration, const Duration(hours: 1));
      expect(p.paused, isTrue);
      expect(p.environment, 'lounge');
      expect(p.layout, Stereo3dLayout.sideBySideHalf);
    });

    test('reads the decoder and dropped frames, and tolerates their absence', () {
      final full = parseXrCinemaEvent('position 1.000 9.000 0 cinema none v4l2m2m-copy 12')! as XrCinemaPosition;
      expect(full.hwdec, 'v4l2m2m-copy');
      expect(full.droppedFrames, 12);
      final old = parseXrCinemaEvent('position 1.000 9.000 0 cinema none')! as XrCinemaPosition;
      expect(old.hwdec, 'no');
      expect(old.droppedFrames, 0);
    });

    test('parses simple events and keeps error text', () {
      expect(parseXrCinemaEvent('ready'), isA<XrCinemaReady>());
      expect(parseXrCinemaEvent('ended\n'), isA<XrCinemaEnded>());
      expect(parseXrCinemaEvent('exit'), isA<XrCinemaExited>());
      expect((parseXrCinemaEvent('error EGL: no display') as XrCinemaError).message, 'EGL: no display');
    });

    test('ignores unknown or malformed lines', () {
      expect(parseXrCinemaEvent(''), isNull);
      expect(parseXrCinemaEvent('telemetry 1 2'), isNull);
      expect(parseXrCinemaEvent('position abc def 0'), isNull);
      expect(parseXrCinemaEvent('position 1 2'), isNull);
      expect(parseXrCinemaEvent('position nan 2 0'), isNull);
    });

    test('negative positions clamp to zero', () {
      final p = parseXrCinemaEvent('position -3 10 0 void none')! as XrCinemaPosition;
      expect(p.position, Duration.zero);
    });
  });

  test('buildXrCinemaArguments passes every field and escapes nothing', () {
    final args = buildXrCinemaArguments(
      const XrCinemaRequest(
        url: 'https://s/v.mkv?X-Plex-Token=a b',
        headers: {'Authorization': 'Bearer t'},
        start: Duration(seconds: 90),
        title: 'Movie',
        environment: VrEnvironment.void_,
        stereo: Stereo3dInfo(Stereo3dLayout.topBottomFull, swapEyes: true),
        volume: 250,
      ),
    );
    expect(args, [
      '--url',
      'https://s/v.mkv?X-Plex-Token=a b',
      '--header',
      'Authorization: Bearer t',
      '--start',
      '90.000',
      '--env',
      'void',
      '--layout',
      'topBottomFull',
      '--swap-eyes',
      '--volume',
      '100',
      '--title',
      'Movie',
    ]);
  });

  test('buildXrCinemaArguments omits optional flags by default', () {
    final args = buildXrCinemaArguments(const XrCinemaRequest(url: 'u'));
    expect(args, ['--url', 'u', '--env', 'cinema', '--layout', 'none']);
  });

  group('resolveXrHelperPath', () {
    test('prefers a valid override', () {
      final path = resolveXrHelperPath(
        resolvedExecutable: '/opt/plezy/plezy',
        environment: {'PLEZY_XR_BIN': '/x/plezy-xr'},
        exists: (p) => true,
      );
      expect(path, '/x/plezy-xr');
    });
    test('falls back to the file beside the executable', () {
      final path = resolveXrHelperPath(
        resolvedExecutable: '/opt/plezy/plezy',
        environment: const {},
        exists: (p) => p == '/opt/plezy/plezy-xr',
      );
      expect(path, '/opt/plezy/plezy-xr');
    });
    test('is null when absent', () {
      expect(
        resolveXrHelperPath(resolvedExecutable: '/opt/plezy/plezy', environment: const {}, exists: (_) => false),
        isNull,
      );
    });
  });

  test('environment wire names match the native helper', () {
    expect(VrEnvironment.void_.wireName, 'void');
    expect(VrEnvironment.passthrough.wireName, 'passthrough');
    expect({for (final e in VrEnvironment.values) e.wireName}, {'void', 'cinema', 'lounge', 'space', 'passthrough'});
  });

  group('how a source is played on the headset', () {
    test('H.264 and 8-bit HEVC use the hardware decoder at any size, with no conversion', () {
      for (final (codec, width, height) in [('h264', 3840, 2160), ('AVC', 1920, 1080), ('hevc', 3840, 2072)]) {
        final plan = VrMenuController.planForSource(codec: codec, width: width, height: height, bitDepth: 8);
        expect(plan.hardwareDecoding, isTrue, reason: codec);
        expect(plan.preset, isNull, reason: codec);
      }
    });

    test('10-bit HEVC is decoded in software, and converted by the server above 1080p', () {
      final big = VrMenuController.planForSource(codec: 'hevc', width: 3840, height: 2072, bitDepth: 10);
      expect(big.preset, TranscodeQualityPreset.p1080_20mbps);
      expect(big.hardwareDecoding, isTrue, reason: 'the converted stream is H.264');
      final small = VrMenuController.planForSource(codec: 'HEVC', width: 1920, height: 1080, bitDepth: 10);
      expect(small.hardwareDecoding, isFalse);
      expect(small.preset, isNull);
    });

    test('HEVC with an unknown bit depth is treated as 10-bit', () {
      final plan = VrMenuController.planForSource(codec: 'hevc', width: 3840, height: 2160);
      expect(plan.preset, TranscodeQualityPreset.p1080_20mbps);
      final small = VrMenuController.planForSource(codec: 'hevc', width: 1920, height: 1080);
      expect(small.hardwareDecoding, isFalse);
      expect(small.preset, isNull);
    });

    test('other codecs are software-only: converted above 1080p, played as is below', () {
      for (final codec in ['av1', 'vp9', 'mpeg2video']) {
        expect(
          VrMenuController.planForSource(codec: codec, width: 3840, height: 2160, bitDepth: 8).preset,
          TranscodeQualityPreset.p1080_20mbps,
          reason: codec,
        );
        expect(VrMenuController.planForSource(codec: codec, width: 1280, height: 720, bitDepth: 8).preset, isNull);
      }
    });

    test('10-bit H.264 is software-decoded', () {
      expect(
        VrMenuController.planForSource(codec: 'h264', width: 1920, height: 1080, bitDepth: 10).hardwareDecoding,
        isFalse,
      );
    });

    test('an unknown source is played as is, in software', () {
      for (final codec in [null, '']) {
        final plan = VrMenuController.planForSource(codec: codec, width: 3840, height: 2160);
        expect(plan.hardwareDecoding, isFalse);
        expect(plan.preset, isNull);
      }
    });
  });

  group('temporary codec refusals (the VR cinema asking for H.264)', () {
    tearDown(() => VideoDecodeCapabilities.temporaryRefusals = const {});

    test('a refused codec is not offered as a transcode output, and H.264 always is', () {
      VideoDecodeCapabilities.temporaryRefusals = const {RankedVideoCodec.hevc, RankedVideoCodec.av1};
      expect(VideoDecodeCapabilities.accepts(RankedVideoCodec.hevc), isFalse);
      expect(VideoDecodeCapabilities.accepts(RankedVideoCodec.av1), isFalse);
      expect(VideoDecodeCapabilities.accepts(RankedVideoCodec.h264), isTrue);
      expect(VideoDecodeCapabilities.transcodeVideoCodecs, [RankedVideoCodec.h264]);
    });

    test('H.264 cannot be refused even if listed', () {
      VideoDecodeCapabilities.temporaryRefusals = const {RankedVideoCodec.h264};
      expect(VideoDecodeCapabilities.accepts(RankedVideoCodec.h264), isTrue);
    });

    test('clearing the set restores the normal offer', () {
      VideoDecodeCapabilities.temporaryRefusals = const {RankedVideoCodec.hevc};
      VideoDecodeCapabilities.temporaryRefusals = const {};
      expect(VideoDecodeCapabilities.accepts(RankedVideoCodec.hevc), isTrue);
    });
  });

  group('menu events and commands', () {
    test('parses selections, back and stopped', () {
      expect((parseXrCinemaEvent('select\tlib:1:2') as XrCinemaSelected).id, 'lib:1:2');
      expect((parseXrCinemaEvent('select\titem:srv:The Movie 3') as XrCinemaSelected).id, 'item:srv:The Movie 3');
      expect(parseXrCinemaEvent('select\t'), isNull);
      expect(parseXrCinemaEvent('back'), isA<XrCinemaBack>());
      expect(parseXrCinemaEvent('stopped'), isA<XrCinemaStopped>());
    });

    test('protocol fields cannot contain tabs or newlines', () {
      expect(xrProtocolField('a\tb\nc\r\nd'), 'a b c d');
      expect(xrProtocolField('  padded  '), 'padded');
    });

    test('menu arguments start the helper in menu mode', () {
      expect(buildXrMenuArguments(environment: VrEnvironment.lounge), ['--menu', '--env', 'lounge']);
    });
  });

  group('XrCinemaSession against a stand-in helper', () {
    late Directory dir;
    setUp(() => dir = Directory.systemTemp.createTempSync('xr_helper_test'));
    tearDown(() => dir.deleteSync(recursive: true));

    String script(String body) {
      final f = File('${dir.path}/helper.sh')..writeAsStringSync('#!/bin/sh\n$body\n');
      Process.runSync('chmod', ['+x', f.path]);
      return f.path;
    }

    test('relays events, tracks the last position and stops on quit', () async {
      final helper = script(r"""
echo ready
echo "position 5.000 100.000 0 cinema none"
while read line; do
  echo "position 42.000 100.000 1 cinema none"
  if [ "$line" = "quit" ]; then echo exit; exit 0; fi
done
""");
      final session = await XrCinemaService.launch(
        const XrCinemaRequest(url: 'u'),
        starter: (exe, args) => Process.start(helper, args),
      );
      final events = <XrCinemaEvent>[];
      session.events.listen(events.add);
      await session.stop();
      await session.exitCode;
      await Future<void>.delayed(const Duration(milliseconds: 100));
      expect(events.first, isA<XrCinemaReady>());
      expect(events.last, isA<XrCinemaExited>());
      expect(session.lastPosition!.position, const Duration(seconds: 42));
    });

    test('sends menu screens and playback requests in the helper protocol', () async {
      final captured = '${dir.path}/captured.txt';
      final helper = script('cat > "$captured"');
      final session = await XrCinemaService.launchMenu(
        environment: VrEnvironment.cinema,
        starter: (exe, args) => Process.start(helper, args),
      );
      session.showScreen(
        title: 'Movies\tand more',
        canGoBack: true,
        tiles: const [
          XrMenuTile(id: 'item:1', title: 'Avatar', subtitle: '2009', imagePath: '/tmp/a.img'),
          XrMenuTile(id: 'more', title: 'Load\nmore'),
        ],
      );
      session.showMessage('Loading...');
      session.play(
        const XrCinemaRequest(
          url: 'https://s/v.mkv?t=1',
          headers: {'X-Token': 'abc'},
          start: Duration(seconds: 90),
          stereo: Stereo3dInfo(Stereo3dLayout.sideBySideHalf, swapEyes: true),
          volume: 80,
          hardwareDecoding: false,
        ),
      );
      session.stopPlayback();
      await session.stop(grace: const Duration(seconds: 2)); // closes stdin via quit, then the script ends
      await session.exitCode;
      final lines = File(captured).readAsLinesSync();
      expect(lines, [
        'ui.begin\tMovies and more\t1',
        'ui.tile\titem:1\tAvatar\t2009\t/tmp/a.img',
        'ui.tile\tmore\tLoad more\t\t',
        'ui.end',
        'ui.status\tLoading...',
        'layout sideBySideHalf',
        'hwdec software',
        'swap',
        'volume 80',
        'header X-Token: abc',
        'start 90.000',
        'load https://s/v.mkv?t=1',
        'stop',
        'quit',
      ]);
    });

    test('reports an error line and exit when the helper fails to start VR', () async {
      final helper = script('echo "error no headset"; echo exit; exit 1');
      final session = await XrCinemaService.launch(
        const XrCinemaRequest(url: 'u'),
        starter: (exe, args) => Process.start(helper, args),
      );
      final events = await session.events.toList();
      expect(events.whereType<XrCinemaError>().single.message, 'no headset');
      expect(events.last, isA<XrCinemaExited>());
      expect(await session.exitCode, 1);
    });

    test('stop kills a helper that ignores quit', () async {
      final helper = script('while true; do sleep 1; done');
      final session = await XrCinemaService.launch(
        const XrCinemaRequest(url: 'u'),
        starter: (exe, args) => Process.start(helper, args),
      );
      await session.stop(grace: const Duration(milliseconds: 200));
      expect(await session.exitCode, isNot(0));
    });
  });
}
