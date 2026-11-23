// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT
//
// A standalone reproduction of two defects in engine/core/io/Data.cpp, and the patch that fixes
// them, side by side. Data.cpp is the main agent's core file, so this branch documents the bug and
// proves the fix instead of editing it.
//
//   g++ -std=c++20 -O0 -g -fsanitize=address,undefined -fno-sanitize-recover=all
//       tools/android/data-io-overflow-repro.cpp -o /tmp/datarepro && /tmp/datarepro B
//
// Run with no argument for all three cases (A aborts under ASan: that is defect A). Run with `A` for
// only the aborting case.
//
// Defect A -- a read past the end of a buffer, with an unsigned wrap:
//   Data::read:56   if (offset + aBytes >= dataLength) aBytes = dataLength - offset;
//   With dataLength == 0 (or offset > dataLength) `dataLength - offset` wraps to a huge value and
//   the memcpy below reads gigabytes from an empty buffer. Reachable from a script:
//   FileData.newFile(false) -> seek(-4) -> read8().
//
// Defect B -- a read at the end returns the wrong bytes instead of nothing:
//   `>=` turns "asked for exactly what is left" into "past the end", the subtraction then gives 0
//   ... except when offset has already moved past dataLength, where it wraps again. Either way the
//   read at EOF must return 0 bytes, and today it can return the buffer's first byte instead.

#include <cstdio>
#include <cstring>
#include <vector>

namespace bug {
    // verbatim arithmetic of engine/core/io/Data.cpp:81-88 and :56-64
    struct Data {
        unsigned char* dataPtr = nullptr;
        unsigned int dataLength = 0;
        unsigned int offset = 0;
        void seek(int aOffset) {
            if (aOffset >= 0)
                offset = (unsigned int)aOffset;
            else
                offset = dataLength + (unsigned int)aOffset;
            if (offset > dataLength - 1)
                offset = dataLength - 1;
        }
        unsigned int read(unsigned char* aDst, unsigned int aBytes) {
            if (offset + aBytes >= dataLength)
                aBytes = dataLength - offset;
            memcpy(aDst, dataPtr + offset, aBytes);
            offset += aBytes;
            return aBytes;
        }
    };
}

namespace fixed {
    struct Data {
        std::vector<unsigned char> storage;
        unsigned int dataLength = 0;
        unsigned int offset = 0;
        // Signed arithmetic with a real lower bound, and no read once the cursor is past the last
        // byte. The upper bound is dataLength, not dataLength - 1: clamping to the last byte made
        // "seek past the end, then read" return that byte again instead of nothing (the same defect
        // as B, one line up).
        void seek(int aOffset) {
            long long wanted = aOffset >= 0 ? (long long)aOffset : (long long)dataLength + aOffset;
            if (wanted < 0) wanted = 0;
            if (wanted > (long long)dataLength) wanted = (long long)dataLength;
            offset = (unsigned int)wanted;
        }
        unsigned int read(unsigned char* aDst, unsigned int aBytes) {
            if (offset >= dataLength) return 0;
            const unsigned int available = dataLength - offset;
            if (aBytes > available) aBytes = available;
            memcpy(aDst, storage.data() + offset, aBytes);
            offset += aBytes;
            return aBytes;
        }
    };
}

int main(int argc, char** argv) {
    unsigned char out[8] = {0};
    // A aborts under ASan, on purpose; `A` selects it so the other cases can be run in one binary
    const bool onlyA = argc > 1 && std::strcmp(argv[1], "A") == 0;

    if (onlyA || argc == 1) {
        std::printf("A. an empty file, seek(-4), read(8) -- the sequence a Lua script reaches\n");
        std::printf("   bug:   offset = %u (0 - 4 wrapped; the clamp against 0 - 1 wrapped too)\n", (unsigned)0 - 4u);
        std::printf("   bug:   read() computes aBytes = 0 - %u and memcpy's from dataPtr + that\n", (unsigned)0 - 4u);
        std::printf("   bug:   the next line is the defect; ASan aborts here\n");
        std::fflush(stdout);
        bug::Data d;
        d.seek(-4);
        (void)d.read(out, 8);
    }

    std::printf("\nB. a 4-byte buffer, read it all, then read one more\n");
    {
        unsigned char both[4] = {'A', 'B', 'C', 'D'};
        bug::Data d;
        d.dataPtr = both;
        d.dataLength = 4;
        (void)d.read(out, 4);
        const unsigned int n = d.read(out, 1);
        std::printf("   bug:   read(1) at the end returns %u byte(s) holding 0x%02x (the buffer's first)\n", n, out[0]);
        std::printf("          a reader that loops until read() returns 0 never stops, or stops early\n");

        fixed::Data f;
        f.storage.assign(both, both + 4);
        f.dataLength = 4;
        (void)f.read(out, 4);
        std::printf("   fix:   read(1) at the end returns 0 bytes\n");
    }

    std::printf("\nC. the same two cases through the patch\n");
    {
        // each value is read *before* the call that prints it: a printf's arguments are evaluated
        // in an unspecified order, so `d.offset, d.read(...)` may print the cursor after the read
        // has already moved it (this repro had exactly that bug for one run)
        fixed::Data empty;
        empty.seek(-4);
        const unsigned int emptyOffset = empty.offset;
        const unsigned int emptyRead = empty.read(out, 8);
        std::printf("   fix:   empty file seek(-4) -> offset %u, read(8) -> %u bytes, no memcpy\n",
                    emptyOffset, emptyRead);

        fixed::Data d;
        d.storage.assign({'A', 'B', 'C', 'D'});
        d.dataLength = 4;
        d.seek(100);
        const unsigned int pastOffset = d.offset;
        const unsigned int pastRead = d.read(out, 4);
        std::printf("   fix:   4-byte file seek(100) -> offset %u (== dataLength: past the last byte), "
                    "read -> %u\n", pastOffset, pastRead);
        d.seek(-100);
        const unsigned int backOffset = d.offset;
        const unsigned int backRead = d.read(out, 1);
        std::printf("   fix:   seek(-100) -> offset %u (the start), and the first read gives 0x%02x\n",
                    backOffset, backRead ? out[0] : 0);
    }
    std::printf("\nthe patch is in ANDROID_HANDOFF.md section 4; Data.cpp is the main agent's file\n");
    return 0;
}
