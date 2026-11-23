#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""The identity of one JasperEngine Android build: its release name, number and commit.

    python3 tools/android/release_identity.py            # the identity of this checkout
    python3 tools/android/release_identity.py --json     # the same, one JSON object

The release manager (agent 3) is the only one who declares a release name, and the name lives in
``tools/android/releases.json`` with the facts of every APK published so far. A build is named from
that file -- never from the branch, never from the date -- so two builds of the same commit get the
same name and a build of another commit cannot borrow one.

The Gradle modules read this through ``tools/android/release_identity.py --gradle`` (see
``engine/workspaces/androidstudio/editor/build.gradle``), which prints the ``-P`` assignments
Gradle takes. Nothing here writes anything: a build that cannot be named fails instead of
inventing a name, because an unnamed APK is exactly what this file exists to prevent.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
MANIFEST = Path(__file__).resolve().parent / "releases.json"

# Bumped by the release manager only, with a new entry in releases.json: Android's versionCode is
# what the system compares, and a lower one cannot be installed over a higher one.
VERSION_CODE = 1


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True,
                          check=False).stdout.strip()


def load_manifest() -> dict:
    with MANIFEST.open(encoding="utf-8") as handle:
        return json.load(handle)


def match_release(manifest: dict, commit: str) -> dict | None:
    """The release entry whose commit this checkout is, or None.

    A published APK names the exact commit it was built from. A checkout of any other commit has
    no name yet: building it would produce an unidentified APK, so the caller must refuse.
    """
    for release in manifest.get("releases", []):
        if release.get("commit") == commit:
            return release
    return None


def identity(manifest: dict | None = None, commit: str | None = None) -> dict:
    manifest = manifest if manifest is not None else load_manifest()
    commit = commit if commit is not None else git("rev-parse", "HEAD")
    release = match_release(manifest, commit)
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    highest_code = max((r.get("versionCode") or r.get("number") or 0) for r in manifest.get("releases", []))
    if release:
        build_number = release.get("number", manifest.get("buildNumber", 0))
        version_code = release.get("versionCode") or build_number
    else:
        # An unpublished commit is the next build after everything published, and both of its
        # numbers are ABOVE every release: Android refuses to install a lower versionCode over a
        # higher one, so a candidate that reused a released number could not be tried on a phone
        # that already had that release. (The build number can be ahead of every published
        # versionCode -- V8 was built before the manifest existed -- so the code follows both.)
        build_number = manifest.get("buildNumber", 0) + 1
        version_code = max(highest_code, build_number) + 1
    result = {
        "commit": commit,
        "shortCommit": commit[:9],
        # A published commit keeps its published name; anything else is a candidate that this
        # build is the first to make, and it says so.
        "releaseName": release["name"] if release else f"candidate-{commit[:7]}",
        "releaseNumber": release.get("number") if release else None,
        "buildNumber": build_number,
        "dirty": dirty,
        "published": release is not None,
        "versionCode": version_code,
    }
    return result


def gradle_assignments(ident: dict) -> str:
    """The -P flags Gradle reads (editor/build.gradle, app/build.gradle), ONE PER LINE.

    Both halves of that are load-bearing, and both were wrong:

    - each flag needs its own -P. Gradle reads a bare `name=value` as a TASK NAME, so the build
      failed with "Task 'jasperReleaseName=candidate-86430b3 jasperBuildNumber=9 ...' not found".
    - one per line, because tools/build.sh reads this file into an array with `mapfile`, which
      splits on newlines: on one line the five assignments arrived as a single argument.
    """
    return "\n".join((
        f"-PjasperReleaseName={ident['releaseName']}",
        f"-PjasperBuildNumber={ident['buildNumber']}",
        f"-PjasperCommit={ident['commit']}",
        f"-PjasperVersionCode={ident['versionCode']}",
        f"-PjasperDirty={'true' if ident['dirty'] else 'false'}",
    ))


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--json", action="store_true", help="the identity as one JSON object")
    parser.add_argument("--gradle", action="store_true", help="the -P assignments for Gradle")
    parser.add_argument("--commit", help="identify this commit instead of HEAD (CI passes $GITHUB_SHA)")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when this commit is not a published release")
    args = parser.parse_args(argv)

    manifest = load_manifest()
    ident = identity(manifest, args.commit)

    if args.json:
        print(json.dumps(ident, indent=2, sort_keys=True))
    elif args.gradle:
        print(gradle_assignments(ident))
    else:
        print(f"release     {ident['releaseName']}")
        print(f"build       #{ident['buildNumber']}")
        print(f"commit      {ident['commit']}")
        print(f"versionCode {ident['versionCode']}")
        print(f"published   {'yes' if ident['published'] else 'no (a candidate, named after its commit)'}")
        if ident["dirty"]:
            print("working tree has uncommitted changes: the APK would not match the commit")

    if args.strict and not ident["published"]:
        print(f"error: {ident['shortCommit']} is not a release in tools/android/releases.json",
              file=sys.stderr)
        return 1
    if args.strict and ident["dirty"]:
        print("error: the working tree is dirty; commit the change before releasing", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
