import 'package:flutter_test/flutter_test.dart';
import 'package:plezy/media/stereo_3d.dart';

void main() {
  group('detectStereo3dFromName', () {
    test('reads delimited packing tokens', () {
      expect(detectStereo3dFromName('Avatar.2009.3D.HSBS.1080p.mkv'), Stereo3dLayout.sideBySideHalf);
      expect(detectStereo3dFromName('Avatar (2009) [Half-SBS]'), Stereo3dLayout.sideBySideHalf);
      expect(detectStereo3dFromName('Avatar.2009.3D.FSBS.mkv'), Stereo3dLayout.sideBySideFull);
      expect(detectStereo3dFromName('Avatar 3D Full SBS'), Stereo3dLayout.sideBySideFull);
      expect(detectStereo3dFromName('Movie.3D.H-OU.mkv'), Stereo3dLayout.topBottomHalf);
      expect(detectStereo3dFromName('Movie.3D.HTAB.mkv'), Stereo3dLayout.topBottomHalf);
      expect(detectStereo3dFromName('Movie.3D.FTAB.mkv'), Stereo3dLayout.topBottomFull);
      expect(detectStereo3dFromName('Movie Over Under'), Stereo3dLayout.topBottomHalf);
    });
    test('ignores look-alikes and bare 3D', () {
      expect(detectStereo3dFromName('Absbsb.mkv'), Stereo3dLayout.none);
      expect(detectStereo3dFromName('Movie.3D.mkv'), Stereo3dLayout.none);
      expect(detectStereo3dFromName('Brother.2000.mkv'), Stereo3dLayout.none);
      expect(detectStereo3dFromName(null), Stereo3dLayout.none);
      expect(detectStereo3dFromName(''), Stereo3dLayout.none);
    });
  });

  group('stereo3dFromContainer', () {
    test('maps Matroska values and picks full vs half from the frame shape', () {
      expect(stereo3dFromContainer(1, frameWidth: 1920, frameHeight: 1080).layout, Stereo3dLayout.sideBySideHalf);
      expect(stereo3dFromContainer(1, frameWidth: 3840, frameHeight: 1080).layout, Stereo3dLayout.sideBySideFull);
      expect(stereo3dFromContainer(3, frameWidth: 1920, frameHeight: 1080).layout, Stereo3dLayout.topBottomHalf);
      expect(stereo3dFromContainer(3, frameWidth: 1920, frameHeight: 2160).layout, Stereo3dLayout.topBottomFull);
    });
    test('flags right-eye-first packings', () {
      expect(stereo3dFromContainer(11).swapEyes, isTrue);
      expect(stereo3dFromContainer(2).swapEyes, isTrue);
      expect(stereo3dFromContainer(1).swapEyes, isFalse);
    });
    test('unknown values are 2D', () {
      expect(stereo3dFromContainer(null).isStereo, isFalse);
      expect(stereo3dFromContainer(0).isStereo, isFalse);
      expect(stereo3dFromContainer('nonsense').isStereo, isFalse);
    });
  });

  test('stereo3dFromMpvStereoIn maps mpv values', () {
    expect(stereo3dFromMpvStereoIn('sbs2l', frameWidth: 1920, frameHeight: 1080).layout, Stereo3dLayout.sideBySideHalf);
    expect(stereo3dFromMpvStereoIn('sbs2r', frameWidth: 3840, frameHeight: 1080).swapEyes, isTrue);
    expect(stereo3dFromMpvStereoIn('ab2l', frameWidth: 1920, frameHeight: 1080).layout, Stereo3dLayout.topBottomHalf);
    expect(stereo3dFromMpvStereoIn('mono').isStereo, isFalse);
    expect(stereo3dFromMpvStereoIn(null).isStereo, isFalse);
  });

  test('a bare 3D tag is only a guess, and only when asked for', () {
    expect(detectStereo3dFromName('Avatar (2009) 3D.mkv'), Stereo3dLayout.none);
    expect(detectStereo3dFromName('Avatar (2009) 3D.mkv', guessFromBare3dTag: true), Stereo3dLayout.sideBySideHalf);
    expect(detectStereo3dFromName('Avatar 3D FSBS', guessFromBare3dTag: true), Stereo3dLayout.sideBySideFull);
    expect(detectStereo3dFromName('Avatar 2009.mkv', guessFromBare3dTag: true), Stereo3dLayout.none);
    expect(resolveStereo3d(name: 'Movie 3D', guessFromBare3dTag: true).layout, Stereo3dLayout.sideBySideHalf);
  });

  test('resolveStereo3d prefers override, then container, then name', () {
    const sbs = Stereo3dInfo(Stereo3dLayout.sideBySideHalf);
    const tab = Stereo3dInfo(Stereo3dLayout.topBottomHalf);
    expect(resolveStereo3d(userOverride: tab, container: sbs, name: 'x.FSBS'), tab);
    expect(resolveStereo3d(container: sbs, name: 'x.H-OU'), sbs);
    expect(resolveStereo3d(name: 'x.H-OU'), tab);
    expect(resolveStereo3d(name: 'x'), const Stereo3dInfo(Stereo3dLayout.none));
  });

  test('wire names round-trip', () {
    for (final l in Stereo3dLayout.values) {
      expect(Stereo3dLayout.fromWireName(l.wireName), l);
    }
    expect(Stereo3dLayout.fromWireName('bogus'), Stereo3dLayout.none);
  });
}
