#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Says which assets of a project will not work on an Android phone, and why.

The runtime's answer lives in engine/platform/android/{AndroidAssetImport,AndroidImageLimits,
AndroidImageOrientation,AndroidModelSidecars,AndroidImportPreflight}.h and is tested on the host by
AndroidSubsystemsTest.cpp. This script is the part a person runs: it walks a project folder, feeds
each file's first bytes to a small builder compiled against those same headers, and prints the
report the editor's Project panel would show before packaging.

    tools/android-asset-report.py <project folder> [--bitmap-factory] [--media-codec]
                                  [--max-texture N] [--decoded-mb N] [--json]

Exit codes: 0 nothing is broken, 1 at least one asset will not draw or play, 2 the report could not
be built (no C++ compiler, or the headers did not compile) -- a broken asset is not a broken tool,
so the two are never the same code.

It composes, it does not reimplement: the verdicts come from the headers, so a change there changes
this without a second copy of the rules.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HEADERS = ("AndroidAssetImport.h", "AndroidImageLimits.h", "AndroidImageOrientation.h",
           "AndroidModelSidecars.h", "AndroidImportPreflight.h")

# a model's sidecars are in its own JSON, and a .gltf is text: read all of it
PREFIX_BYTES = {
    ".gltf": 4 << 20,
}
DEFAULT_PREFIX = 64 << 10

# what an import of each type is: the extension decides which header answers, the bytes refine it
SCANNED_EXTENSIONS = {
    ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tga", ".psd", ".hdr", ".exr", ".ktx2", ".ktx",
    ".webp", ".heic", ".heif", ".avif", ".svg",
    ".wav", ".ogg", ".mp3", ".flac", ".m4a", ".aac", ".opus",
    ".gltf", ".glb", ".fbx", ".obj", ".dae", ".3ds",
}

# the builder prints one line per file and parses nothing else: a finding that needs a new field
# gets it added here and in the header, so the tool cannot drift from the tested code
BUILDER = r"""
#include "AndroidImportPreflight.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace jasperengine::android;

// one byte per field cannot hold a path, so the builder reads a manifest of "<size> <name>" lines
// and the files themselves from stdin-adjacent paths the caller writes
int main(int argc, char** argv) {
    if (argc < 2) return 3;
    PreflightOptions options;
    options.androidBitmapFactory = argc > 2 && std::string(argv[2]) == "1";
    options.androidMediaCodec = argc > 3 && std::string(argv[3]) == "1";
    if (argc > 4) options.budget.maxTextureSize = std::atoi(argv[4]);
    if (argc > 5) options.budget.decodedBytesLimit = (uint64_t)std::atoll(argv[5]) << 20;
    if (argc > 6) options.residentLimitBytes = (uint64_t)std::atoll(argv[6]) << 20;

    std::ifstream list(argv[1]);
    std::string line;
    std::vector<PreflightInput> inputs;
    std::vector<std::string> names;
    while (std::getline(list, line)) {
        // "<display name>\t<path>"
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        PreflightInput input;
        input.fileName = line.substr(0, tab);
        std::ifstream file(line.substr(tab + 1), std::ios::binary);
        std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        input.prefix.assign(bytes.begin(), bytes.end());
        input.fileBytes = input.prefix.size();
        names.push_back(input.fileName);
        inputs.push_back(input);
    }

    const PreflightReport report = preflightImports(inputs, options);
    for (const PreflightFinding& finding : report.findings) {
        // tab-separated: the caller formats, this only carries what the headers answered
        printf("%s\t%s\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%.4f\t",
               finding.fileName.c_str(), finding.assetName.c_str(), (int)finding.severity,
               (int)finding.type, finding.message.c_str(), finding.width, finding.height,
               finding.importedWidth, finding.importedHeight, finding.scale);
        for (size_t i = 0; i < finding.missingSidecars.size(); i++) {
            printf("%s%s", i ? "," : "", finding.missingSidecars[i].c_str());
        }
        printf("\t%u\t", finding.orientation);
    for (size_t i = 0; i < finding.sidecarAssetNames.size(); i++) {
        printf("%s%s", i ? "," : "", finding.sidecarAssetNames[i].c_str());
    }
    printf("\n");
    }
    printf("#\t%d\t%d\t%llu\t%llu\t%d\t", report.broken, report.notes,
           (unsigned long long)report.largestDecodedBytes,
           (unsigned long long)report.residentTextureBytes, report.overResidentBudget ? 1 : 0);
    for (size_t i = 0; i < report.suggestDownscale.size(); i++) {
        printf("%s%s", i ? "," : "", report.suggestDownscale[i].c_str());
    }
    printf("\t%s\n", report.summary.c_str());
    return report.broken > 0 ? 1 : 0;
}
"""

SEVERITY = {0: "fine", 1: "note", 2: "BROKEN"}
TYPE = {0: "unknown", 4: "texture", 5: "model", 6: "audio"}


def fail(message):
    print(f"android-asset-report: {message}", file=sys.stderr)
    return 2


def repo_root():
    return os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def build(root, workdir):
    compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        return None, "no C++ compiler found (set CXX)"
    source = os.path.join(workdir, "report.cpp")
    with open(source, "w", encoding="utf-8") as handle:
        handle.write(BUILDER)
    binary = os.path.join(workdir, "report")
    command = [compiler, "-std=c++20", "-O1", "-o", binary, source,
               "-I", os.path.join(root, "engine", "platform", "android"),
               "-I", os.path.join(root, "engine", "core"),
               "-I", os.path.join(root, "engine", "libs", "json")]
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode != 0:
        return None, "the headers did not compile:\n" + (result.stderr or result.stdout).strip()
    return binary, None


def collect(folder):
    """Every importable file under `folder`, with the bytes an import would read first."""
    entries = []
    for directory, subdirectories, files in os.walk(folder):
        subdirectories[:] = sorted(d for d in subdirectories if not d.startswith("."))
        for name in sorted(files):
            if name.startswith("."):
                continue
            extension = os.path.splitext(name)[1].lower()
            if extension not in SCANNED_EXTENSIONS:
                continue
            path = os.path.join(directory, name)
            # the name an import sees is the leaf, never the machine's path
            entries.append((name, path, PREFIX_BYTES.get(extension, DEFAULT_PREFIX)))
    return entries


def truncated_copy(path, limit, workdir, index):
    """A prefix file, because a 4 GB video's first 64 KiB is all the verdict needs."""
    with open(path, "rb") as handle:
        head = handle.read(limit)
    copy = os.path.join(workdir, f"prefix-{index:05d}")
    with open(copy, "wb") as handle:
        handle.write(head)
    return copy


def main(argv):
    parser = argparse.ArgumentParser(description="Which assets will not work on an Android phone.")
    parser.add_argument("folder", help="the project folder to scan")
    parser.add_argument("--bitmap-factory", action="store_true",
                        help="the device decodes WebP/HEIC through BitmapFactory")
    parser.add_argument("--media-codec", action="store_true",
                        help="the device decodes M4A/AAC through MediaCodec")
    parser.add_argument("--max-texture", type=int, default=4096, help="the GPU's longest side")
    parser.add_argument("--decoded-mb", type=int, default=0,
                        help="the bytes one decoded image may take (0: no limit known)")
    parser.add_argument("--resident-mb", type=int, default=0,
                        help="the MiB this device has for all textures together (0: not known)")
    parser.add_argument("--json", action="store_true", help="the report as JSON")
    arguments = parser.parse_args(argv)

    if not os.path.isdir(arguments.folder):
        return fail(f"{arguments.folder} is not a folder")
    entries = collect(arguments.folder)
    if not entries:
        print(f"android-asset-report: no importable asset under {arguments.folder}")
        return 0

    root = repo_root()
    with tempfile.TemporaryDirectory(prefix="android-report-") as workdir:
        binary, error = build(root, workdir)
        if not binary:
            return fail(error)
        listing = os.path.join(workdir, "list.txt")
        with open(listing, "w", encoding="utf-8") as handle:
            for index, (name, path, limit) in enumerate(entries):
                handle.write(f"{name}\t{truncated_copy(path, limit, workdir, index)}\n")
        command = [binary, listing, "1" if arguments.bitmap_factory else "0",
                   "1" if arguments.media_codec else "0", str(arguments.max_texture),
                   str(arguments.decoded_mb), str(arguments.resident_mb)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode not in (0, 1):
            return fail("the report tool failed:\n" + (result.stderr or result.stdout).strip())

        findings = []
        summary = None
        for line in result.stdout.splitlines():
            fields = line.split("\t")
            if fields[0] == "#":
                summary = {"broken": int(fields[1]), "notes": int(fields[2]),
                           "largestDecodedBytes": int(fields[3]),
                           "residentTextureBytes": int(fields[4]),
                           "overResidentBudget": fields[5] == "1",
                           "suggestDownscale": [s for s in fields[6].split(",") if s],
                           "summary": fields[7]}
                continue
            if len(fields) < 11:
                continue
            findings.append({
                "file": fields[0], "assetName": fields[1],
                "severity": SEVERITY.get(int(fields[2]), "?"),
                "type": TYPE.get(int(fields[3]), fields[3]),
                "message": fields[4],
                "width": int(fields[5]), "height": int(fields[6]),
                "importedWidth": int(fields[7]), "importedHeight": int(fields[8]),
                "scale": float(fields[9]),
                "sidecars": [s for s in fields[10].split(",") if s],
                "orientation": int(fields[11]) if len(fields) > 11 else 0,
                "sidecarAssetNames": [s for s in (fields[12] if len(fields) > 12 else "").split(",") if s],
            })

    if arguments.json:
        print(json.dumps({"summary": summary, "findings": findings}, indent=2))
    else:
        for finding in findings:
            if finding["severity"] == "fine":
                continue
            mark = "!" if finding["severity"] == "BROKEN" else "-"
            print(f"{mark} {finding['file']} -> {finding['assetName']} ({finding['type']})")
            print(f"    {finding['message']}")
            if finding["sidecarAssetNames"]:
                pairs = [f"{uri} -> {name}" for uri, name in
                         zip(finding["sidecars"], finding["sidecarAssetNames"])]
                print("    they land as: " + ", ".join(pairs))
        if summary:
            total = len(findings)
            print()
            print(f"{total} asset{'s' if total != 1 else ''}: {summary['broken']} broken, "
                  f"{summary['notes']} to note")
            if summary["largestDecodedBytes"]:
                print(f"largest single decode: {summary['largestDecodedBytes'] / (1 << 20):.1f} MB")
            if summary["residentTextureBytes"]:
                print(f"all textures resident: {summary['residentTextureBytes'] / (1 << 20):.1f} MB")
            if summary["overResidentBudget"]:
                print("shrink first: " + ", ".join(summary["suggestDownscale"]))
            if summary["summary"]:
                print(f"first to fix: {summary['summary']}")
    return 1 if (summary and summary["broken"]) else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
