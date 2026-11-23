// The two Data I/O defects reported in ANDROID_BUGS.md (10b, 10b-ii), as a test that fails
// loudly. Compiled and run by tools/ci/lint.sh with AddressSanitizer and UBSan, so the read past
// the end is an abort and not a silent success:
//
//   g++ -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
//       engine/tests/core/io/DataSeekBoundsTest.cpp -o /tmp/dataseek && /tmp/dataseek
//
// The arithmetic is copied from engine/core/io/Data.cpp (the private members are the point of the
// bug, so the class is modelled here rather than linked: Data.h drags in File, ResourcePack and the
// whole io layer for three methods). If Data.cpp changes, this file must change with it -- the
// two are checked against each other by tools/android/test_data_seek_bounds.py, which extracts the
// real bodies and asserts they still read as they do here.

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// --- engine/core/io/Data.cpp, verbatim (the fixed versions) --------------------------------
struct Data {
    unsigned char* dataPtr = nullptr;
    unsigned int dataLength = 0;
    unsigned int offset = 0;
    bool dataOwned = false;

    Data() = default;
    explicit Data(unsigned int n) : dataLength(n) {
        dataPtr = new unsigned char[n];
        std::memset(dataPtr, 0, n);
        dataOwned = true;
    }
    ~Data() { if (dataOwned) delete[] dataPtr; }

    unsigned int read(unsigned char* aDst, unsigned int aBytes) {
        if (dataPtr == nullptr || aDst == nullptr || aBytes == 0 || offset >= dataLength)
            return 0;

        const unsigned int available = dataLength - offset;
        if (aBytes > available)
            aBytes = available;

        std::memcpy(aDst, dataPtr + offset, aBytes);
        offset += aBytes;
        return aBytes;
    }

    void seek(int aOffset) {
        long long target = aOffset >= 0 ? static_cast<long long>(aOffset)
                                        : static_cast<long long>(dataLength) + aOffset;
        if (target < 0) target = 0;
        if (target > static_cast<long long>(dataLength)) target = static_cast<long long>(dataLength);
        offset = static_cast<unsigned int>(target);
    }

    unsigned int pos() const { return offset; }
    unsigned int length() const { return dataLength; }
};

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::printf("FAIL %s\n", what);
        ++failures;
    }
}

} // namespace

int main() {
    unsigned char out[16];

    // 10b: an empty Data, a negative seek, then a read. This is the reported SIGSEGV:
    // FileData.newFile(false) -> seek(-4) -> read8(). ASan turns any slip into an abort here.
    {
        Data empty;
        empty.seek(-4);
        check(empty.pos() == 0, "seek(-4) on an empty Data stops at 0");
        const unsigned n = empty.read(out, 8);
        check(n == 0, "read on an empty Data returns 0");
        check(empty.pos() == 0, "the cursor does not move on a refused read");
    }

    // a negative seek deeper than the buffer, and one that lands inside it
    {
        Data four(4);
        four.seek(-100);
        check(four.pos() == 0, "seek(-100) clamps to 0");
        four.seek(-1);
        check(four.pos() == 3, "seek(-1) is the last byte");
        four.seek(0);
        check(four.pos() == 0, "seek(0) is the first");
    }

    // a seek past the end is legal and lands one past the last byte: that is where read() returns 0
    {
        Data four(4);
        four.seek(99);
        check(four.pos() == 4, "seek past the end stops at dataLength");
        const unsigned n = four.read(out, 4);
        check(n == 0, "read at the end returns 0");
        check(four.pos() == 4, "and leaves the cursor at the end");
    }

    // 10b-ii: a read that starts inside the buffer and asks for more than remains gets the rest,
    // not a wrapped count
    {
        Data four(4);
        unsigned char buf[8];
        std::memset(buf, 0xEE, sizeof(buf));
        check(four.read(buf, 4) == 4, "a full read returns 4");
        check(four.pos() == 4, "and moves the cursor to the end");
        check(four.read(buf, 1) == 0, "the read after it returns 0, not the buffer's first byte");

        four.seek(2);
        const unsigned n = four.read(buf, 8);
        check(n == 2, "a read past the end returns only what remains");
        check(four.pos() == 4, "and stops at the end");
    }

    // a null destination is refused rather than written through
    {
        Data four(4);
        check(four.read(nullptr, 4) == 0, "a null destination reads nothing");
        check(four.pos() == 0, "and does not consume the buffer");
    }

    // seeking to the very end and reading is the loop-termination case every parser uses
    {
        Data four(4);
        four.seek(4);
        check(four.read(out, 1) == 0, "a cursor at dataLength reads nothing");
    }

    if (failures == 0) std::printf("Data seek/read bounds: all checks pass\n");
    return failures == 0 ? 0 : 1;
}
