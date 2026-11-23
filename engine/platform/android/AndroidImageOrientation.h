// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_IMAGE_ORIENTATION_H
#define ANDROID_IMAGE_ORIENTATION_H

// A photo's orientation, and the transform an importer applies so it is not upside down or
// sideways. Plain C++ with no Android header (the pattern of AndroidTextInput.h), so the rules
// run on any host.
//
// Why it matters more on a phone than on a desktop: a camera and a screenshot tool store the
// pixels in the sensor's own order and record how to turn them in EXIF Orientation (0x0112).
// Every phone gallery applies it. An engine that reads the pixels and ignores the tag imports a
// portrait photo as a landscape one, rotated 90 degrees, or upside down — the single most common
// "my game's textures are wrong" report from a phone, and it also makes the texture's width and
// height wrong, which the import's budget (AndroidImageLimits) and any aspect-fitted UI then get
// wrong too.
//
// The engine has no EXIF reader today (grep for exif/orientation under engine/core/texture is
// empty) and `TextureData` offers `flipVertical` only, so an importer needs the answer here and
// the transform to apply itself. PNG carries the same tag in an `eXIf` chunk; both are parsed.
//
// Only the tag is read: no GPS, no timestamps, nothing else leaves the file (a privacy point: an
// import must not copy EXIF into a project, where a shipped game would publish a user's location).

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace jasperengine::android {

    // EXIF Orientation's eight values, named by what the stored pixels need (EXIF 2.3 §4.6.4).
    enum class Orientation : uint8_t {
        Unknown = 0,
        // 1: no transform
        Normal,
        // 2: mirrored left-right (a selfie camera's front sensor)
        MirrorHorizontal,
        // 3: 180 degrees
        Rotate180,
        // 4: mirrored top-bottom
        MirrorVertical,
        // 5: mirrored across the main diagonal, then rotated
        Transpose,
        // 6: 90 degrees clockwise (the common portrait phone photo)
        Rotate90,
        // 7: mirrored across the anti-diagonal
        Transverse,
        // 8: 90 degrees counter-clockwise
        Rotate270,
    };

    // What an importer does to the stored pixels, in an order that composes: flip, then rotate.
    struct OrientationTransform {
        // the stored pixels are turned this many degrees clockwise after the flips
        int rotateClockwise = 0;
        bool flipHorizontal = false;
        bool flipVertical = false;
        // the tag swaps the stored width and height (every 90-degree case)
        bool swapDimensions = false;
        // false for Unknown and Normal: nothing to do, and the pixels are already right
        bool needed = false;
    };

    inline Orientation orientationFromValue(uint32_t value) {
        if (value >= 1 && value <= 8) return static_cast<Orientation>(value);
        return Orientation::Unknown;
    }

    // The picture is already upright: the tag said 1, or there is no tag at all. (Both leave the
    // pixels alone; only "the file carried a tag" differs, which an import does not care about.)
    inline bool isUpright(Orientation orientation) {
        return orientation == Orientation::Normal || orientation == Orientation::Unknown;
    }

    // What a tag is, in the words a report uses for it: "upright", "turned 90 clockwise", ...
    inline const char* orientationName(Orientation orientation) {
        switch (orientation) {
            case Orientation::Normal: return "upright";
            case Orientation::MirrorHorizontal: return "mirrored left to right";
            case Orientation::Rotate180: return "upside down";
            case Orientation::MirrorVertical: return "mirrored top to bottom";
            case Orientation::Transpose: return "transposed";
            case Orientation::Rotate90: return "turned 90 clockwise";
            case Orientation::Transverse: return "turned 90 clockwise and mirrored";
            case Orientation::Rotate270: return "turned 90 counter-clockwise";
            default: return "with no tag";
        }
    }

    // The transform for a tag. A mirrored rotation is expressed as one flip and one rotation, which
    // is what a resampler can do in a single pass (Transpose/Transverse are a rotation plus a flip).
    inline OrientationTransform transformFor(Orientation orientation) {
        OrientationTransform transform;
        switch (orientation) {
            case Orientation::Normal:
            case Orientation::Unknown:
                // a tag that says "already upright" changes no pixel; an absent tag is Unknown, so
                // isUpright() is what tells the two apart
                return transform;
            case Orientation::MirrorHorizontal:
                transform.flipHorizontal = true;
                break;
            case Orientation::Rotate180:
                transform.rotateClockwise = 180;
                break;
            case Orientation::MirrorVertical:
                transform.flipVertical = true;
                break;
            case Orientation::Transpose:
                // stored transposed: rotate 90 clockwise, then mirror horizontally
                transform.rotateClockwise = 90;
                transform.flipHorizontal = true;
                transform.swapDimensions = true;
                break;
            case Orientation::Rotate90:
                transform.rotateClockwise = 90;
                transform.swapDimensions = true;
                break;
            case Orientation::Transverse:
                // rotate 90 clockwise, then mirror vertically
                transform.rotateClockwise = 90;
                transform.flipVertical = true;
                transform.swapDimensions = true;
                break;
            case Orientation::Rotate270:
                transform.rotateClockwise = 270;
                transform.swapDimensions = true;
                break;
        }
        transform.needed = true;
        return transform;
    }

    namespace detail {
        inline uint16_t tiff16(const uint8_t* data, bool little) {
            return little ? (uint16_t)(data[0] | (data[1] << 8)) : (uint16_t)((data[0] << 8) | data[1]);
        }
        inline uint32_t tiff32(const uint8_t* data, bool little) {
            return little ? ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | data[0]
                          : ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | data[3];
        }

        // The Orientation tag (0x0112, SHORT, count 1) of IFD0 of a TIFF structure that starts at
        // `tiff`. Bounded: every offset is checked against `end`, so a malformed file cannot walk
        // out of the buffer (which is what a hostile share would try).
        inline uint32_t orientationFromTiff(const uint8_t* tiff, size_t size) {
            if (size < 8) return 0;
            bool little = false;
            if (tiff[0] == 'I' && tiff[1] == 'I') little = true;
            else if (!(tiff[0] == 'M' && tiff[1] == 'M')) return 0;
            // the 42 magic is what tells a TIFF from anything else with the same two bytes
            if (tiff16(tiff + 2, little) != 42) return 0;

            const uint32_t ifdOffset = tiff32(tiff + 4, little);
            if (ifdOffset > size || size - ifdOffset < 2) return 0;
            const uint8_t* ifd = tiff + ifdOffset;
            const uint16_t count = tiff16(ifd, little);
            // each entry is 12 bytes; a count that does not fit is truncated to what does
            const size_t room = (size - ifdOffset - 2) / 12;
            const size_t entries = count < room ? count : room;
            for (size_t i = 0; i < entries; i++) {
                const uint8_t* entry = ifd + 2 + i * 12;
                const uint16_t tag = tiff16(entry, little);
                if (tag != 0x0112) continue;
                const uint16_t type = tiff16(entry + 2, little);
                // SHORT (3) is what the tag must be; anything else is a malformed file
                if (type != 3) return 0;
                // the value sits in the first two bytes of the 4-byte value field
                return tiff16(entry + 8, little);
            }
            return 0;
        }
    }

    // The orientation of a JPEG, from an APP1 "Exif" segment before the first scan. 0 when there is
    // none (a screenshot, a re-saved file) or when the segment is malformed. `prefix` must reach
    // the segment: 64 KiB always does (EXIF's own limit is 64 KiB and it is the first segment).
    inline uint32_t readJpegOrientation(std::span<const uint8_t> prefix) {
        if (prefix.size() < 4 || prefix[0] != 0xFF || prefix[1] != 0xD8) return 0;
        size_t at = 2;
        while (at + 4 <= prefix.size()) {
            if (prefix[at] != 0xFF) return 0;
            const uint8_t marker = prefix[at + 1];
            if (marker == 0xFF) { at++; continue; }
            // standalone markers carry no length
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
            // before the first scan there is one (the image data follows, and EXIF is before it)
            if (marker == 0xD9 || marker == 0xDA) return 0;
            const uint16_t length = (uint16_t)((prefix[at + 2] << 8) | prefix[at + 3]);
            if (length < 2) return 0;
            const size_t segmentEnd = at + 2 + (size_t)length;
            if (marker == 0xE1 && length >= 8) {
                // "Exif\0\0" then the TIFF header
                const uint8_t* body = prefix.data() + at + 4;
                const size_t bodySize = segmentEnd <= prefix.size() ? (size_t)length - 2 : prefix.size() - (at + 4);
                if (bodySize >= 6 && body[0] == 'E' && body[1] == 'x' && body[2] == 'i' && body[3] == 'f' &&
                    body[4] == 0 && body[5] == 0) {
                    return detail::orientationFromTiff(body + 6, bodySize - 6);
                }
            }
            at = segmentEnd;
        }
        return 0;
    }

    // The orientation of a PNG, from an `eXIf` chunk (PNG 1.5 extension) anywhere before IEND.
    inline uint32_t readPngOrientation(std::span<const uint8_t> bytes) {
        if (bytes.size() < 8 + 12 || bytes[0] != 0x89 || bytes[1] != 'P' || bytes[2] != 'N' || bytes[3] != 'G') return 0;
        size_t at = 8;
        while (at + 8 <= bytes.size()) {
            const uint32_t length = ((uint32_t)bytes[at] << 24) | ((uint32_t)bytes[at + 1] << 16) |
                                    ((uint32_t)bytes[at + 2] << 8) | (uint32_t)bytes[at + 3];
            // the buffer may stop right after a chunk's CRC: the next header is only complete
            // with its 8 bytes (length and type), which is what `at + 8 <= size` promised
            const uint8_t* type = bytes.data() + at + 4;
            if (type[0] == 'e' && type[1] == 'X' && type[2] == 'I' && type[3] == 'f') {
                // the chunk holds a bare TIFF structure (no "Exif\0\0"), then its CRC; the
                // TIFF reader is bounded by the data size, so a chunk the buffer cuts short is
                // parsed as far as it goes
                const size_t available = bytes.size() - at - 8;
                return detail::orientationFromTiff(bytes.data() + at + 8, length < available ? length : available);
            }
            if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') return 0;
            // A chunk has 8 header bytes, `length` of data and a 4-byte CRC. The last chunk a
            // buffer holds may be cut short: when the data is all there the CRC is optional (a
            // caller reading a prefix need not have it), and when the data is not, the chunk ends
            // the buffer.
            const size_t remaining = bytes.size() - at - 8;
            if (length > remaining) return 0;
            const size_t afterData = at + 8 + (size_t)length;
            const size_t crcEnd = afterData + 4;
            if (crcEnd > bytes.size()) return 0;   // a truncated CRC: the next header is not there
            at = crcEnd;
        }
        return 0;
    }

    // The orientation of a JPEG or a PNG by its signature; 0 for anything else or when the tag is
    // there but unreadable.
    inline uint32_t readOrientation(std::span<const uint8_t> bytes) {
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8) return readJpegOrientation(bytes);
        if (bytes.size() >= 4 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G') {
            return readPngOrientation(bytes);
        }
        return 0;
    }

    // The size an import records for a texture: the stored size, or the other way round when the
    // orientation turns the picture (TextureData's originalWidth/originalHeight must agree with
    // what the player sees, or an aspect-fitted sprite is stretched).
    struct OrientedSize {
        int width = 0;
        int height = 0;
        bool swapped = false;
    };

    inline OrientedSize orientedSize(int storedWidth, int storedHeight, Orientation orientation) {
        OrientedSize size;
        const OrientationTransform transform = transformFor(orientation);
        size.swapped = transform.swapDimensions;
        size.width = transform.swapDimensions ? storedHeight : storedWidth;
        size.height = transform.swapDimensions ? storedWidth : storedHeight;
        return size;
    }

}

#endif // ANDROID_IMAGE_ORIENTATION_H
