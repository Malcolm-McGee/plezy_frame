import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:plezy/media/stereo_3d.dart';
import 'package:plezy/services/settings_service.dart';
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
