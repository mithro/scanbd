#!/usr/bin/env python3
"""Derive the Debian version for the welland patched scanbd package.

scanbd upstream is a fixed released tarball (1.5.1); this repo carries Debian's
1.5.1-7 packaging, re-homed as a native package, plus a local C patch that
fixes physical button presses for the SANE pixma backend. The version is:

    <UPSTREAM_VERSION>+welland<WELLAND_REV>          e.g. 1.5.1+welland1

The +welland<N> local suffix sorts ABOVE Debian's 1.5.1-7 (so this build
installs over it) and is cleanly superseded by any future official upstream
release (1.5.2, 1.6.x, ...), which apt treats as newer. Bump WELLAND_REV when
the packaging or the patch changes but upstream does not.

The changelog timestamp comes from the packaging HEAD commit date, so a
re-build re-stamps deterministically.
"""
import argparse
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHANGELOG = REPO / "debian" / "changelog"
SOURCE = "scanbd"
# upstream release this repo packages (Debian's scanbd 1.5.1-7 sources)
UPSTREAM_VERSION = "1.5.1"
WELLAND_REV = "1"
MAINTAINER = "Tim 'mithro' Ansell <me@mith.ro>"


def _git(*args):
    return subprocess.run(["git", "-C", str(REPO), *args],
                          capture_output=True, text=True, check=True).stdout.strip()


def version():
    return f"{UPSTREAM_VERSION}+welland{WELLAND_REV}"


def write_changelog():
    try:
        date = _git("log", "-1", "--format=%cd", "--date=rfc2822")
    except subprocess.CalledProcessError:
        # no commits yet (fresh checkout in CI before fetch-depth resolves)
        from email.utils import formatdate
        date = formatdate(localtime=True)
    CHANGELOG.write_text(
        f"{SOURCE} ({version()}) unstable; urgency=medium\n\n"
        f"  * Welland patched build of scanbd {UPSTREAM_VERSION} (Debian 1.5.1-7\n"
        f"    sources, re-homed as a native package).\n"
        f"  * Fix physical scanner button presses never firing an action with the\n"
        f"    SANE pixma backend (Canon PIXMA / CanoScan, e.g. LiDE 400): the poll\n"
        f"    loop now SET_VALUEs the backend's \"button-update\" option to refresh\n"
        f"    the cached button/event state before reading the monitored options.\n\n"
        f" -- {MAINTAINER}  {date}\n"
    )


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--write-changelog", action="store_true")
    a = ap.parse_args()
    (write_changelog if a.write_changelog else lambda: print(version()))()
