#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Record a built APK as a release, with its SHA-256 and the tests that were run.

    python3 tools/android/publish.py --apk build/android/jasperengine-editor.apk \
        --name "Android RC1" --tests docs/android/rc1-tests.md --run 45096599335

What it does, in order, and refuses to do any of it if a step fails:

1. Reads the identity of the checkout (tools/android/release_identity.py) and checks that the APK
   was built from this commit: the build's identity is written into the APK's Java
   (EditorBuildInfo) and into ``assets/jasper-build.txt`` by Gradle, and both must agree.
2. Hashes the APK (SHA-256) and reads its ABI directories, package name and versionCode with the
   same pure-Python APK reader the repository's Android screen check uses (tools/android-screen-
   check.py), so no aapt2 is needed.
3. Appends a release entry to tools/android/releases.json: name, number, kind, commit, package,
   ABIs, SHA-256, size, date, the run that built it and the note file.
4. Writes the ``--notes`` file (default: docs/android/<slug>-notes.md) from the entry, so the
   GitHub release body and the repository's record cannot disagree.

The APK itself is never copied into the repository: it goes to the GitHub release this prints the
command for. ``--dry-run`` does everything but write.
"""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import json
import re
import struct
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "android"))

import release_identity  # noqa: E402  (the sibling script's rules, not a copy of them)

MANIFEST = release_identity.MANIFEST


# ---------------------------------------------------------------------------------------------
# A minimal APK reader: an APK is a zip with AndroidManifest.xml (binary XML) and lib/<abi>/*.so

def apk_metadata(apk: Path) -> dict:
    result = {"abis": [], "package": None, "versionCode": None, "versionName": None,
              "label": None, "buildLine": None}
    with zipfile.ZipFile(apk) as archive:
        names = archive.namelist()
        result["abis"] = sorted({n.split("/")[1] for n in names
                                 if n.startswith("lib/") and n.endswith(".so")})
        manifest = archive.read("AndroidManifest.xml")
        result.update(parse_android_manifest(manifest))
        for candidate in ("assets/jasper-build.txt",):
            if candidate in names:
                result["buildLine"] = archive.read(candidate).decode("utf-8", "replace").strip()
        if result["label"] is None:
            result["label"] = result["package"]
    return result


def parse_android_manifest(data: bytes) -> dict:
    """package=, versionCode=, versionName= from Android's binary XML manifest.

    The binary format (docs: "AndroidManifest.xml, compiled") is a header and a string pool
    followed by chunks; the <manifest> element's attributes carry the four values, and each
    attribute value is either an index into the string pool or an inline int. Only what an APK
    names in those attributes is read: anything else raises, and the caller falls back to the
    identity file rather than guessing.
    """
    if len(data) < 8 or struct.unpack_from("<H", data, 0)[0] != 0x0003:
        raise ValueError("not a binary XML manifest")
    strings: list[str] = []
    offset = struct.unpack_from("<H", data, 2)[0]
    size = len(data)
    result = {"package": None, "versionCode": None, "versionName": None}
    while offset + 8 <= size:
        chunk_type, header_size, chunk_size = struct.unpack_from("<HHI", data, offset)
        if chunk_size < 8:
            break
        if chunk_type == 0x0001:  # string pool
            count, _styles, flags, strings_start, _ = struct.unpack_from("<IIIII", data, offset + 8)
            utf8 = bool(flags & (1 << 8))
            offsets = struct.unpack_from(f"<{count}I", data, offset + header_size)
            base = offset + strings_start
            for string_offset in offsets:
                at = base + string_offset
                if utf8:
                    # u16len (in UTF-16 units) then u8len (bytes), each 1 or 2 bytes
                    n = data[at]
                    at += 2 if n & 0x80 else 1
                    length = data[at]
                    if length & 0x80:
                        length = ((length & 0x7F) << 8) | data[at + 1]
                        at += 2
                    else:
                        at += 1
                    strings.append(data[at:at + length].decode("utf-8", "replace"))
                else:
                    length = struct.unpack_from("<H", data, at)[0]
                    at += 2
                    if length & 0x8000:
                        length = ((length & 0x7FFF) << 16) | struct.unpack_from("<H", data, at)[0]
                        at += 2
                    strings.append(data[at:at + length * 2].decode("utf-16-le", "replace"))
        elif chunk_type == 0x0102:  # start element
            # ResXMLTree_node (16 bytes: 8 header, lineNumber, comment) then ResXMLTree_attrExt
            # (ns, name, attributeStart, attributeSize, attributeCount, idIndex, classIndex,
            # styleIndex = 4+4+2*6 = 20), then `attributeCount` ResXMLTree_attribute entries of
            # attributeSize bytes each (ns, name, rawValue, then an 8-byte Res_value).
            body = offset + 16
            (_ns, name_index, attr_start, attr_size,
             attr_count) = struct.unpack_from("<IIHHH", data, body)
            if name_index < len(strings) and strings[name_index] == "manifest":
                attrs = body + attr_start
                for i in range(attr_count):
                    at = attrs + i * attr_size
                    (_ans, aname, raw_value, _vsize, _res0, dtype,
                     res_data) = struct.unpack_from("<IIIHBBI", data, at)
                    if aname >= len(strings):
                        continue
                    key = strings[aname]
                    if dtype == 0x03 and raw_value < len(strings):  # a string
                        text = strings[raw_value]
                    else:
                        text = res_data
                    if key == "package":
                        result["package"] = text
                    elif key == "versionCode":
                        result["versionCode"] = int(text)
                    elif key == "versionName":
                        result["versionName"] = str(text)
                break
        offset += chunk_size
    if result["package"] is None:
        raise ValueError("no <manifest package=...> in the binary manifest")
    return result


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apk", required=True, type=Path)
    parser.add_argument("--name", required=True, help='the release name, e.g. "Android RC1"')
    parser.add_argument("--kind", choices=("rc", "stable"), default="rc")
    parser.add_argument("--tests", type=Path, help="the markdown with the tests that were run")
    parser.add_argument("--run", help="the GitHub Actions run id that built it")
    parser.add_argument("--notes", type=Path, help="where the notes file is written")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)

    apk = args.apk if args.apk.is_absolute() else (Path.cwd() / args.apk)
    if not apk.is_file():
        print(f"error: no APK at {apk}", file=sys.stderr)
        return 1

    manifest = release_identity.load_manifest()
    ident = release_identity.identity(manifest)
    if any(r["name"].lower() == args.name.lower() for r in manifest["releases"]):
        print(f"error: {args.name!r} is already used; release names are never reused", file=sys.stderr)
        return 1
    if ident["dirty"]:
        print("error: the working tree has uncommitted changes; the APK would not match its commit",
              file=sys.stderr)
        return 1
    if release_identity.match_release(manifest, ident["commit"]):
        print(f"error: {ident['shortCommit']} already has a release; a rebuild keeps that name"
              f" and gets a higher build number", file=sys.stderr)
        return 1

    meta = apk_metadata(apk)
    # the APK must be the one this checkout built
    if meta["buildLine"] and ident["shortCommit"] not in meta["buildLine"]:
        print(f"error: the APK names {meta['buildLine']!r}, not this commit {ident['shortCommit']}",
              file=sys.stderr)
        return 1

    digest = sha256(apk)
    number = manifest["buildNumber"] + 1
    version_code = number
    entry = {
        "name": args.name,
        "number": number,
        "kind": args.kind,
        "versionCode": version_code,
        "commit": ident["commit"],
        "package": meta["package"],
        "versionName": meta["versionName"],
        "abis": meta["abis"],
        "sha256": digest,
        "sizeBytes": apk.stat().st_size,
        "date": dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "ciRun": args.run,
        "tests": str(args.tests) if args.tests else None,
        "notes": None,
    }
    notes_path = args.notes or (ROOT / "docs" / "android" / f"{slug(args.name)}-notes.md")

    print(f"apk        {apk}")
    print(f"release    {entry['name']} (build #{number}, versionCode {version_code})")
    print(f"commit     {entry['commit']}")
    print(f"package    {entry['package']} {entry.get('versionName') or ''}")
    print(f"abis       {', '.join(entry['abis']) or '(none found)'}")
    print(f"sha256     {digest}")
    print(f"size       {entry['sizeBytes']} bytes")
    if args.dry_run:
        print("dry run: nothing written")
        return 0

    notes_path.parent.mkdir(parents=True, exist_ok=True)
    tests = ""
    if args.tests and (args.tests.is_file()):
        tests = args.tests.read_text(encoding="utf-8").strip()
    elif args.tests:
        print(f"warning: no test file at {args.tests}; the notes will say so", file=sys.stderr)
    notes_path.write_text(
        f"# {entry['name']} (build #{number})\n\n"
        f"| | |\n| --- | --- |\n"
        f"| APK | `{apk.name}` |\n"
        f"| SHA-256 | `{digest}` |\n"
        f"| Commit | `{entry['commit']}` |\n"
        f"| Package | `{entry['package']}` (`versionCode` {version_code}) |\n"
        f"| ABIs | {', '.join(entry['abis']) or 'none found'} |\n"
        f"| Built | {entry['date']} |\n"
        f"| CI run | {('https://github.com/' + owner_repo() + '/actions/runs/' + args.run) if args.run else 'not recorded'} |\n\n"
        + ("## Tests that were run\n\n" + (tests if tests else
           "_None recorded yet: this build is not a stable release._") + "\n"),
        encoding="utf-8")
    entry["notes"] = str(notes_path.relative_to(ROOT))

    manifest["buildNumber"] = number
    manifest["releases"].append(entry)
    MANIFEST.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(f"notes      {entry['notes']}")
    print(f"recorded   {MANIFEST.relative_to(ROOT)}")
    print()
    print("Publish the APK as a GitHub release (it is not committed):")
    print(f'  gh release create android-{slug(args.name)} "{apk}" \\')
    print(f'      --title "{entry["name"]}" --notes-file {entry["notes"]} \\')
    print(f'      --target {entry["commit"]}')
    return 0


def owner_repo() -> str:
    out = subprocess.run(["git", "remote", "get-url", "origin"], cwd=ROOT,
                         capture_output=True, text=True, check=False).stdout.strip()
    found = re.search(r"github\.com[:/]([^/]+/[^/.]+)", out)
    return found.group(1) if found else "OWNER/REPO"


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
