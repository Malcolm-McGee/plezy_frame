#!/usr/bin/env python3
"""Prepare the libmpv prefix for building Plezy on SteamOS (Steam Frame) without root.

SteamOS has a read-only root and ships neither libmpv nor the Lua 5.2 / MuJS
libraries the pinned libmpv build links against. This script:

  1. fetches the pinned libmpv prefix (scripts/fetch_linux_libmpv.py),
  2. downloads liblua5.2-0 and libmujs3 from Ubuntu 24.04 (arm64) - the distro the
     libmpv build was made on, so the sonames and symbol versions match - checks
     them against the SHA-256 in Ubuntu's package index, and unpacks the shared
     libraries next to libmpv,
  3. writes empty mujs/lua52 pkg-config stand-ins so `pkg-config mpv` resolves
     (they are only build-time requirements of mpv itself),
  4. downloads a Temurin 21 JDK (SteamOS has none, and the `jni` Flutter package
     needs jni.h to configure the Linux build), verified against Adoptium's
     published SHA-256.

Everything lands inside the project folder; nothing else is touched.
"""

import argparse
import gzip
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MIRROR = "http://ports.ubuntu.com/ubuntu-ports"
PACKAGES = ("liblua5.2-0", "libmujs3")
COMPONENTS = ("main", "universe")
# (pkg-config name, version) pairs mpv.pc lists in Requires.private but SteamOS lacks.
PC_STUBS = (("mujs", "1.3.0"), ("lua52", "5.2.4"))
JDK_API = (
    "https://api.adoptium.net/v3/assets/latest/21/hotspot"
    "?architecture={arch}&image_type=jdk&os=linux&vendor=eclipse"
)


def fetch(url: str) -> bytes:
    # Adoptium's API rejects Python's default user-agent.
    request = urllib.request.Request(url, headers={"User-Agent": "plezy-steamos-setup"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def package_index(component: str) -> dict[str, dict[str, str]]:
    data = gzip.decompress(fetch(f"{MIRROR}/dists/noble/{component}/binary-arm64/Packages.gz")).decode()
    packages = {}
    for block in data.split("\n\n"):
        fields = {}
        for line in block.splitlines():
            if ": " in line and not line.startswith(" "):
                key, _, value = line.partition(": ")
                fields[key] = value
        if "Package" in fields:
            packages[fields["Package"]] = fields
    return packages


def extract_deb_libs(deb: bytes, dest: Path) -> list[str]:
    """Copy the *.so* files of a .deb's data archive into dest; returns their names."""
    with tempfile.TemporaryDirectory() as tmp:
        deb_path = Path(tmp) / "pkg.deb"
        deb_path.write_bytes(deb)
        listing = subprocess.run(["ar", "t", str(deb_path)], check=True, capture_output=True, text=True).stdout.split()
        member = next(name for name in listing if name.startswith("data.tar"))
        data = subprocess.run(["ar", "p", str(deb_path), member], check=True, capture_output=True).stdout
    copied = []
    zstd = member.endswith(".zst")
    if zstd:
        data = subprocess.run(["zstd", "-dc"], input=data, check=True, capture_output=True).stdout
    contents: dict[str, bytes] = {}
    links: dict[str, str] = {}
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
        for entry in archive:
            name = Path(entry.name).name
            if ".so" not in name:
                continue
            if entry.isfile():
                contents[name] = archive.extractfile(entry).read()
            elif entry.issym():
                links[name] = Path(entry.linkname).name
    # The soname libmpv asks for (liblua5.2.so.0) is usually a symlink to the
    # versioned file; write it as a real copy so the prefix stays relocatable.
    for name, target in links.items():
        if target in contents:
            contents[name] = contents[target]
    for name, blob in contents.items():
        (dest / name).write_bytes(blob)
        copied.append(name)
    return copied


def ensure_jdk(dest: Path) -> bool:
    """Download and unpack a Temurin 21 JDK into dest unless one is already there."""
    if (dest / "include/jni.h").is_file():
        print(f"JDK already present at {dest}")
        return True
    import platform

    arch = {"aarch64": "aarch64", "arm64": "aarch64", "x86_64": "x64"}.get(platform.machine())
    if arch is None:
        print(f"error: no JDK mapping for {platform.machine()}", file=sys.stderr)
        return False
    assets = json.loads(fetch(JDK_API.format(arch=arch)))
    package = assets[0]["binary"]["package"]
    print(f"fetching {package['link']}")
    blob = fetch(package["link"])
    if hashlib.sha256(blob).hexdigest() != package["checksum"]:
        print("error: the JDK failed its SHA-256 check", file=sys.stderr)
        return False
    with tempfile.TemporaryDirectory() as tmp:
        with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as archive:
            archive.extractall(tmp, filter="data")
        top = next(Path(tmp).iterdir())
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(top), str(dest))
    return (dest / "include/jni.h").is_file()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dest", type=Path, default=ROOT / "libmpv-prefix", help="libmpv prefix directory")
    parser.add_argument("--jdk-dest", type=Path, default=ROOT / "jdk", help="where to unpack the JDK")
    args = parser.parse_args()
    dest: Path = args.dest.resolve()
    jdk: Path = args.jdk_dest.resolve()

    if subprocess.run([sys.executable, str(ROOT / "scripts/fetch_linux_libmpv.py"), "--dest", str(dest)]).returncode:
        return 1

    libdir = next((p for p in (dest / "lib/aarch64-linux-gnu", dest / "lib/x86_64-linux-gnu") if p.is_dir()), None)
    if libdir is None:
        print(f"error: no libmpv library directory under {dest}/lib", file=sys.stderr)
        return 1

    index: dict[str, dict[str, str]] = {}
    for component in COMPONENTS:
        index.update(package_index(component))
    for name in PACKAGES:
        fields = index.get(name)
        if fields is None:
            print(f"error: {name} is not in the Ubuntu index", file=sys.stderr)
            return 1
        deb = fetch(f"{MIRROR}/{fields['Filename']}")
        if hashlib.sha256(deb).hexdigest() != fields["SHA256"]:
            print(f"error: {name} failed its SHA-256 check", file=sys.stderr)
            return 1
        copied = extract_deb_libs(deb, libdir)
        print(f"{name} {fields['Version']}: {', '.join(sorted(copied))}")

    stubs = dest / "lib/pkgconfig"
    stubs.mkdir(parents=True, exist_ok=True)
    for name, version in PC_STUBS:
        (stubs / f"{name}.pc").write_text(
            f"Name: {name}\nDescription: stand-in so pkg-config can resolve libmpv.pc\nVersion: {version}\n"
        )

    if not ensure_jdk(jdk):
        return 1

    print()
    print("Done. Build with:")
    print(f'  export JAVA_HOME="{jdk}"')
    print(f'  export PKG_CONFIG_PATH="{stubs}:{libdir}/pkgconfig"')
    print(f'  export LD_LIBRARY_PATH="{libdir}"   # only needed to run from the build tree')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
