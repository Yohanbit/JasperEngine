#!/usr/bin/env python3
# (c) Eduardo Doria and contributors
# SPDX-License-Identifier: MIT
"""Reproduces the two engine/core/io/Data.cpp defects against the file's own source.

    tools/android/data-overflow-real.py [--no-run]

Data.cpp belongs to the main agent, so this does not edit it and does not reimplement it: it reads
the file, lifts the bodies of `Data::seek` and `Data::read` out of it verbatim, puts them in a class
with the same fields and the same unsigned types, and runs the two sequences a Lua script can reach.
If Data.cpp changes, this changes with it -- a retyped copy would silently stop proving anything.

Exit codes: 0 the defect is gone (both sequences are now safe -- the file was patched); 1 the defect
is still there, with the AddressSanitizer stack trace; 2 the file could not be read or lifted.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA_CPP = os.path.join(ROOT, "engine", "core", "io", "Data.cpp")
INCLUDES = ["engine/core", "engine/libs/json", "engine/libs/sokol"]

HARNESS = r"""
// Generated: the bodies below are lifted from engine/core/io/Data.cpp, not retyped.
#include <cstdio>
#include <cstring>

namespace jasperengine {
    // the same fields, in the same order and of the same unsigned types as Data; the two bodies are
    // defined inside the class so they see those fields, and they are the file's own text
    struct DataLike {
        unsigned char* dataPtr = nullptr;
        unsigned int dataLength = 0;
        unsigned int offset = 0;

#include "lifted.h"

        unsigned int length() { return dataLength; }
        unsigned int pos() { return offset; }
    };
}

static int fails = 0;

static void expect(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "STILL BROKEN");
    if (!ok) ++fails;
}

int main() {
    std::printf("A. an empty Data, seek(-4), read(8)  (FileData.newFile -> seek -> read8)\n");
    {
        jasperengine::DataLike empty;
        empty.seek(-4);
        std::printf("   pos() after seek(-4) = %u\n", empty.pos());
        if (empty.pos() > 0) {
            // the offset wrapped: a read now memcpy's from dataPtr + that. Under ASan this aborts.
            std::printf("   reading, expecting AddressSanitizer below\n");
            std::fflush(stdout);
            unsigned char out[8] = {0};
            (void)empty.read(out, 8);
            std::printf("   read returned without a crash -- odd for a wrapped offset\n");
            ++fails;
        } else {
            expect(true, "seek(-4) on an empty buffer leaves the offset at 0");
        }
    }

    std::printf("B. read(4) then read(1) on a 4-byte buffer\n");
    {
        unsigned char buf[4] = {'A', 'B', 'C', 'D'};
        jasperengine::DataLike d;
        d.dataPtr = buf;
        d.dataLength = 4;
        unsigned char out[8] = {0};
        const unsigned int first = d.read(out, 4);
        const unsigned int second = d.read(out, 1);
        std::printf("   read(4) = %u, read(1) at the end = %u, byte = 0x%02x\n", first, second, out[0]);
        expect(second == 0, "a read at the end returns 0 bytes");
    }

    if (fails == 0) {
        std::printf("\nthe two defects are fixed in Data.cpp\n");
        return 0;
    }
    std::printf("\n%d defect(s) still in Data.cpp; see ANDROID_HANDOFF.md section 4 for the patch\n", fails);
    return 1;
}
"""


def lift(source, signature):
    """The body of `signature`, braces included, exactly as the file writes it."""
    start = source.find(signature)
    if start < 0:
        raise SystemExit(f"data-overflow-real: {signature!r} is not in Data.cpp any more")
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                body = source[start:index + 1]
                # inside the class the qualifier is not allowed, and it is the only rewrite: the
                # arithmetic and the braces are the file's
                # "unsigned int Data::read(" -> "unsigned int read(" : drop the qualifier only
                return re.sub(r"^(unsigned\ int|void)\ Data::", lambda m: m.group(1) + " ", body)
    raise SystemExit("data-overflow-real: unbalanced braces while lifting a body")


def main(argv):
    parser = argparse.ArgumentParser(description="Reproduce the Data.cpp seek/read defects.")
    parser.add_argument("--no-run", action="store_true", help="build only")
    arguments = parser.parse_args(argv)

    try:
        source = open(DATA_CPP, encoding="utf-8").read()
    except OSError as error:
        return 2 if print(f"data-overflow-real: {error}", file=sys.stderr) else 2

    read = lift(source, "unsigned int Data::read(")
    seek = lift(source, "void Data::seek(")

    compiler = os.environ.get("CXX") or shutil.which("g++") or shutil.which("clang++")
    if not compiler:
        print("data-overflow-real: no C++ compiler (set CXX)", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="data-overflow-") as workdir:
        with open(os.path.join(workdir, "harness.cpp"), "w", encoding="utf-8") as handle:
            handle.write(HARNESS)
        with open(os.path.join(workdir, "lifted.h"), "w", encoding="utf-8") as handle:
            handle.write("#pragma once\n#include <cstring>\n\n" + read + "\n\n" + seek + "\n")
        binary = os.path.join(workdir, "repro")
        command = [compiler, "-std=c++20", "-O0", "-g", "-fsanitize=address,undefined",
                   "-fno-sanitize-recover=all", "-o", binary,
                   os.path.join(workdir, "harness.cpp")]
        for include in INCLUDES:
            command += ["-I", os.path.join(ROOT, include)]
        build = subprocess.run(command, capture_output=True, text=True)
        if build.returncode != 0:
            print("data-overflow-real: the harness did not compile:\n" + (build.stderr or build.stdout),
                  file=sys.stderr)
            return 2
        if arguments.no_run:
            print(f"data-overflow-real: built {binary}")
            return 0
        print("lifted from engine/core/io/Data.cpp verbatim:\n")
        for body in (read, seek):
            for line in body.splitlines():
                print("   " + line)
            print()
        run = subprocess.run([binary], capture_output=True, text=True)
        output = run.stdout + run.stderr
        print(output.rstrip())
        # ASan kills the process at defect A, so "the run finished" is itself the result: a clean
        # exit means the wrapped offset is gone, an abort means it is still there
        if "AddressSanitizer" in output or "SEGV" in output:
            print("\ndata-overflow-real: the defect is present -- Data.cpp aborts under ASan")
            return 1
        return run.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
