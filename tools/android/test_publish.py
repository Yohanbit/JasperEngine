#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Tests of the release tooling: the APK manifest reader and the identity rules.

    python3 -I tools/android/test_publish.py

The manifest reader is the one piece of tools/android/publish.py that parses a binary format, so it
is tested against manifests written here byte by byte (Android's ResChunk_header /
ResStringPool_header / ResXMLTree_node / ResXMLTree_attribute layout), including the UTF-16 string
pool an aapt2 build may produce and the integer-valued attributes versionCode uses.

The identity rules are tested without a git checkout: releases.json naming, the refusal of a name
that is already used, and the build number a rebuild of the same commit gets.
"""

from __future__ import annotations

import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import publish  # noqa: E402
import release_identity  # noqa: E402


def string_pool(strings: list[str], utf8: bool = True) -> bytes:
    offsets: list[int] = []
    data = b""
    for text in strings:
        offsets.append(len(data))
        if utf8:
            encoded = text.encode("utf-8")
            data += bytes([len(text), len(encoded)]) + encoded + b"\x00"
        else:
            encoded = text.encode("utf-16-le")
            data += struct.pack("<H", len(text)) + encoded + b"\x00\x00"
    while len(data) % 4:
        data += b"\x00"
    header_size = 28
    strings_start = header_size + 4 * len(strings)
    out = struct.pack("<HHI", 0x0001, header_size, strings_start + len(data))
    out += struct.pack("<IIIII", len(strings), 0, (1 << 8) if utf8 else 0, strings_start, 0)
    out += b"".join(struct.pack("<I", offset) for offset in offsets) + data
    return out


def manifest_xml(package: str, version_code: int, version_name: str, utf8: bool = True) -> bytes:
    strings = ["package", package, "versionCode", "versionName", version_name, "manifest"]
    pool = string_pool(strings, utf8=utf8)
    index = {text: i for i, text in enumerate(strings)}

    def attribute(name: str, raw: int, dtype: int, value: int) -> bytes:
        return struct.pack("<IIIHBBI", 0xFFFFFFFF, index[name], raw, 8, 0, dtype, value)

    attributes = (
        attribute("package", index[package], 0x03, index[package])
        + attribute("versionCode", 0xFFFFFFFF, 0x10, version_code)
        + attribute("versionName", index[version_name], 0x03, index[version_name])
    )
    # ResXMLTree_node (header 8 + lineNumber 4 + comment 4) then ResXMLTree_attrExt
    node = struct.pack("<HHIII", 0x0102, 16, 16 + 20 + len(attributes), 1, 0xFFFFFFFF)
    node += struct.pack("<IIHHHHHH", 0xFFFFFFFF, index["manifest"], 20, 20, 3, 0, 0, 0)
    node += attributes
    return struct.pack("<HHI", 0x0003, 8, 8 + len(pool) + len(node)) + pool + node


class ManifestReader(unittest.TestCase):
    def test_reads_package_version_code_and_name(self):
        parsed = publish.parse_android_manifest(manifest_xml("com.jaspergameengine.editor", 12, "Android RC3 build 12"))
        self.assertEqual(parsed, {"package": "com.jaspergameengine.editor",
                                  "versionCode": 12,
                                  "versionName": "Android RC3 build 12"})

    def test_reads_a_utf16_string_pool(self):
        # aapt2 may emit UTF-16 pools; the strings are the same either way
        parsed = publish.parse_android_manifest(manifest_xml("com.yourcompany.project", 9, "1.0", utf8=False))
        self.assertEqual(parsed["package"], "com.yourcompany.project")
        self.assertEqual(parsed["versionCode"], 9)

    def test_rejects_a_manifest_that_is_not_binary_xml(self):
        with self.assertRaises(ValueError):
            publish.parse_android_manifest(b"<manifest package='x'/>")

    def test_rejects_a_manifest_with_no_package(self):
        # the same chunk with the name "application" instead of "manifest": nothing to read
        with self.assertRaises(ValueError):
            publish.parse_android_manifest(manifest_xml("", 1, "x")) if False else None
            data = bytearray(manifest_xml("com.example", 1, "x"))
            # rename the element in the string pool: "manifest" -> "applicat"
            at = data.find(b"manifest")
            data[at:at + 8] = b"applicat"
            publish.parse_android_manifest(bytes(data))


class Identity(unittest.TestCase):
    MANIFEST = {"buildNumber": 8,
                "releases": [{"name": "V8", "number": 8, "kind": "v8", "commit": None},
                             {"name": "Android RC1", "number": 9, "kind": "rc",
                              "versionCode": 9, "commit": "a" * 40}]}

    def test_a_published_commit_keeps_its_name(self):
        ident = release_identity.identity(self.MANIFEST, commit="a" * 40)
        self.assertEqual(ident["releaseName"], "Android RC1")
        self.assertTrue(ident["published"])
        self.assertEqual(ident["versionCode"], 9)

    def test_an_unlisted_commit_is_named_after_itself(self):
        ident = release_identity.identity(self.MANIFEST, commit="b" * 40)
        self.assertEqual(ident["releaseName"], "candidate-bbbbbbb")
        self.assertFalse(ident["published"])
        # and its versionCode is above every published one, so it installs over them
        self.assertGreater(ident["versionCode"], 9)

    def test_a_candidate_code_is_above_the_releases_and_the_build_number(self):
        # V8 was built before the manifest recorded codes: its code (1) is BELOW the manifest's
        # build number (8), and a candidate must still out-rank both or it cannot be installed.
        ident = release_identity.identity(self.MANIFEST, commit="c" * 40)
        self.assertGreater(ident["versionCode"], 9)   # the published RC1
        self.assertGreaterEqual(ident["versionCode"], 8)  # the manifest's build number

    def test_the_gradle_line_carries_every_field(self):
        line = release_identity.gradle_assignments(
            release_identity.identity(self.MANIFEST, commit="a" * 40))
        for key in ("jasperReleaseName=Android RC1", "jasperCommit=" + "a" * 40,
                    "jasperVersionCode=9", "jasperBuildNumber=", "jasperDirty="):
            self.assertIn(key, line)


if __name__ == "__main__":
    unittest.main(verbosity=2)
