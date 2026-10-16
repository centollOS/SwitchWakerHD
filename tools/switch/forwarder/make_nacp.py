#!/usr/bin/env python3
"""Write control.nacp (the title data of the HOME menu and hbmenu) with the standard library only.

    make_nacp.py OUT.nacp [--name NAME] [--name-suffix S] [--author AUTHOR] [--version DISPLAY] [--title-id HEX]

The defaults are the project's: name SwitchWakerHD, author centollOS/SwitchWakerHD, the version from
git describe (in the current folder; 0.1.0 without git) and the forwarder's title ID.

Layout: switchbrew "NACP Format" (0x4000 bytes). Every language slot gets the same name and
author (build_forwarder.sh writes an icon for each of them too), so the HOME menu
shows the title and icon whatever the console's language. Choices (as in centollOS):
  - StartupUserAccount = None: no user picker (the game keeps its saves on the SD card).
  - Screenshot = Allow, VideoCapture = Manual (1): the system may capture video (SysDVR streams
    the screen through it, and the capture button records); automatic recording (2) would make
    hbloader hold back 96 MiB of the application's memory (nx-hbloader calculateMaxHeapSize:
    only video_capture == 2 sets g_isAutomaticGameplayRecording).
  - No save data, no rating ages (-1), no parental-control flags, LogoHandling = Auto.
  - PresenceGroupId, SaveDataOwnerId, LocalCommunicationId[0] and SeedForPseudoDeviceId = the
    title ID, as the official tools and hacBrewPack's --titleid fill them; AddOnContentBaseId = title ID + 0x1000.
"""
import argparse
import os
import struct
import subprocess

LANGUAGES = [
    "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish",
    "Spanish", "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean",
    "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese",
]


def put(buf, off, data, size):
    data = data[: size - 1] if isinstance(data, bytes) else data
    buf[off : off + len(data)] = data


def generate_nacp(name: str, author: str, version: str, title_id: str) -> bytes:
    tid = int(title_id, 16)
    name_b, author_b, version_b = (x.encode("utf-8") for x in (name, author, version))
    if len(name_b) >= 0x200 or len(author_b) >= 0x100 or len(version_b) >= 0x10:
        raise SystemExit("make_nacp: name/author/version too long")

    n = bytearray(0x4000)
    for i in range(len(LANGUAGES)):
        put(n, i * 0x300, name_b, 0x200)
        put(n, i * 0x300 + 0x200, author_b, 0x100)

    n[0x3025] = 0  # StartupUserAccount: None
    n[0x3026] = 0  # UserAccountSwitchLock: Disable
    struct.pack_into("<I", n, 0x3028, 0)  # AttributeFlag
    struct.pack_into("<I", n, 0x302C, (1 << len(LANGUAGES)) - 1)  # SupportedLanguageFlag
    struct.pack_into("<I", n, 0x3030, 0)  # ParentalControlFlag
    n[0x3034] = 0  # Screenshot: Allow
    n[0x3035] = 1  # VideoCapture: Manual (not 2: no 96 MiB recording reserve)
    n[0x3036] = 0  # DataLossConfirmation: None
    n[0x3037] = 0  # PlayLogPolicy: All
    struct.pack_into("<Q", n, 0x3038, tid)  # PresenceGroupId
    n[0x3040:0x3060] = b"\xff" * 0x20  # RatingAge: none
    put(n, 0x3060, version_b, 0x10)  # DisplayVersion
    struct.pack_into("<Q", n, 0x3070, tid + 0x1000)  # AddOnContentBaseId
    struct.pack_into("<Q", n, 0x3078, tid)  # SaveDataOwnerId
    struct.pack_into("<Q", n, 0x30B0, tid)  # LocalCommunicationId[0]
    n[0x30F0] = 0  # LogoType (unused)
    n[0x30F1] = 0  # LogoHandling: Auto
    struct.pack_into("<Q", n, 0x30F8, tid)  # SeedForPseudoDeviceId
    return bytes(n)


def get_git_version() -> str | None:
    """Get version from git describe, like the forwarder does."""
    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            capture_output=True, text=True, cwd=os.getcwd()
        )
        if result.returncode == 0 and result.stdout.strip():
            version = result.stdout.strip()
            # Strip leading 'v' like forwarder
            if version.startswith('v'):
                version = version[1:]
            # Truncate to 15 chars if needed (NACP limit)
            if len(version) > 15:
                # Drop -g<hash> suffix if present
                if '-g' in version:
                    version = version.rsplit('-g', 1)[0]
                version = version[:15]
            return version
    except Exception:
        pass
    return None


def main():
    ap = argparse.ArgumentParser(description="Generate control.nacp for Switch homebrew")
    ap.add_argument("out", help="Output NACP file path")
    ap.add_argument("--name", default="SwitchWakerHD", help="Application name (max 511 chars, default: SwitchWakerHD)")
    ap.add_argument("--name-suffix", default="", help="appended to the name (e.g. a debug build's marker)")
    ap.add_argument("--author", default="centollOS/SwitchWakerHD", help="Author/publisher (max 255 chars, default: centollOS/SwitchWakerHD)")
    ap.add_argument("--version", default=None, help="Display version (max 15 chars, default: from git describe or 0.1.0)")
    ap.add_argument("--title-id", default="01ff575748440000", help="Title ID in hex (default: WWHD forwarder)")
    a = ap.parse_args()

    # Default version from git describe
    version = a.version
    if version is None:
        version = get_git_version() or "0.1.0"

    name = a.name + a.name_suffix
    nacp = generate_nacp(name, a.author, version, a.title_id)
    # an unchanged NACP keeps its timestamp: the build packs the NRO again only when the NACP changed
    try:
        with open(a.out, "rb") as f:
            same = f.read() == nacp
    except OSError:
        same = False
    if not same:
        with open(a.out, "wb") as f:
            f.write(nacp)
    print(f"make_nacp: {a.out}: {name!r} / {a.author!r} {version!r} 0x{int(a.title_id, 16):016x}")


if __name__ == "__main__":
    main()
