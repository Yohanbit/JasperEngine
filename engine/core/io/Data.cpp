//
// Inspired by work of Jari Komppa in SoLoud audio engine
// https://sol.gfxile.net/soloud/file.html
// Modified by Eduardo Doria.
//

// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#include "Data.h"
#include "ResourcePack.h"

#include <string.h>
#include <memory>

using namespace jasperengine;

Data::Data(): dataPtr(NULL), dataLength(0), offset(0), dataOwned(false) {

}

Data::Data(unsigned char *aData, unsigned int aDataLength, bool aCopy, bool aTakeOwnership): Data(){
    open(aData, aDataLength, aCopy, aTakeOwnership);
}

Data::Data(const char *aFilename): Data(){
    open(aFilename);
}

Data::Data(const Data& d){
    this->dataLength = d.dataLength;
    this->offset = d.offset;
    this->dataOwned = d.dataOwned;

    this->dataPtr = new unsigned char[this->dataLength];
    memcpy(this->dataPtr, d.dataPtr, this->dataLength);
}

Data& Data::operator = (const Data& d){
    this->dataLength = d.dataLength;
    this->offset = d.offset;
    this->dataOwned = d.dataOwned;

    this->dataPtr = new unsigned char[this->dataLength];
    memcpy(this->dataPtr, d.dataPtr, this->dataLength);

    return *this;
}

Data::~Data() {
    if (dataOwned)
        delete[] dataPtr;
}

unsigned int Data::read(unsigned char *aDst, unsigned int aBytes) {
    // The guard comes first, and in this order, because both of the old tests were wrong on an
    // unsigned length: `offset + aBytes >= dataLength` overflowed when aBytes was the remaining
    // count (so a read at the end computed `dataLength - offset` past the buffer), and nothing
    // checked the cursor or the pointer at all -- `Data()` is null with length 0, and `seek(-4)`
    // on it wrapped offset to 4294967292, so `dataPtr + offset` was a SEGV (reproduced under
    // AddressSanitizer: FileData.newFile(false) -> seek(-4) -> read8(), all reachable from Lua).
    if (dataPtr == nullptr || aDst == nullptr || aBytes == 0 || offset >= dataLength)
        return 0;

    const unsigned int available = dataLength - offset;
    if (aBytes > available)
        aBytes = available;

    memcpy(aDst, dataPtr + offset, aBytes);
    offset += aBytes;

    return aBytes;
}

unsigned int Data::write(unsigned char *aSrc, unsigned int aBytes) {
    if (dataLength > 0 || offset > 0)
        return 0;

    dataLength = aBytes;
    dataPtr = (unsigned char*)malloc(aBytes);
    memcpy(dataPtr + offset, aSrc, aBytes);
    offset += aBytes;

    return aBytes;
}

unsigned int Data::length() {
    return dataLength;
}

void Data::seek(int aOffset) {
    // Signed arithmetic throughout: `dataLength + aOffset` and `dataLength - 1` are unsigned
    // expressions, so a negative seek wrapped and the clamp against `dataLength - 1` could not
    // catch it. The upper bound is dataLength (one past the last byte is a legal cursor: that is
    // where a read returns 0), not dataLength - 1.
    long long target = aOffset >= 0 ? static_cast<long long>(aOffset)
                                    : static_cast<long long>(dataLength) + aOffset;
    if (target < 0)
        target = 0;
    if (target > static_cast<long long>(dataLength))
        target = static_cast<long long>(dataLength);
    offset = static_cast<unsigned int>(target);
}

unsigned int Data::pos() {
    return offset;
}

unsigned char * Data::getMemPtr() {
    return dataPtr;
}

unsigned int Data::open(unsigned char *aData, unsigned int aDataLength, bool aCopy, bool aTakeOwnership) {
    if (aData == NULL || aDataLength == 0)
        return FileErrors::INVALID_PARAMETER;

    if (dataOwned)
        delete[] dataPtr;
    dataPtr = 0;
    offset = 0;

    dataLength = aDataLength;

    if (aCopy)
    {
        dataOwned = true;
        dataPtr = new unsigned char[aDataLength];
        if (dataPtr == NULL)
            return FileErrors::OUT_OF_MEMORY;
        memcpy(dataPtr, aData, aDataLength);
        return FileErrors::FILEDATA_OK;
    }

    dataPtr = aData;
    dataOwned = aTakeOwnership;
    return FileErrors::FILEDATA_OK;
}

unsigned int Data::open(const char *aFilename) {
    if (!aFilename)
        return FileErrors::INVALID_PARAMETER;
    if (dataOwned)
        delete[] dataPtr;
    dataPtr = 0;
    dataLength = 0;
    dataOwned = false;
    offset = 0;

    std::unique_ptr<unsigned char[]> packedData;
    unsigned int packedSize = 0;
    if (ResourcePack::readOwned(aFilename, packedData, packedSize)) {
        dataLength = packedSize;
        dataPtr = packedData.release();
        dataOwned = true;
        return FileErrors::FILEDATA_OK;
    }

    File df;
    int res = df.open(aFilename);
    if (res != 0)
        return res;

    dataLength = df.length();
    dataPtr = new unsigned char[dataLength];
    if (dataPtr == NULL)
        return FileErrors::OUT_OF_MEMORY;
    // what the read gave: a file rewritten since its length was asked is shorter, and the rest of
    // this memory is not its content
    dataLength = df.read(dataPtr, dataLength);
    dataOwned = true;
    return FileErrors::FILEDATA_OK;
}

unsigned int Data::open(File *aFile) {
    if (dataOwned)
        delete[] dataPtr;
    dataPtr = 0;
    offset = 0;

    dataLength = aFile->length();
    dataPtr = new unsigned char[dataLength];
    if (dataPtr == NULL)
        return FileErrors::OUT_OF_MEMORY;
    // (what the read gave, as above)
    dataLength = aFile->read(dataPtr, dataLength);
    dataOwned = true;
    return FileErrors::FILEDATA_OK;
}

int Data::eof() {
    if (offset >= dataLength)
        return 1;
    return 0;
}
