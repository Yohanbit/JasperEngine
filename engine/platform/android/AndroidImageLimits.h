// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_IMAGE_LIMITS_H
#define ANDROID_IMAGE_LIMITS_H

// How big an image is before it is decoded, and whether a phone can hold it. Plain C++ with no
// Android header (the pattern of AndroidTextInput.h), so the rules run on any host.
//
// Why it exists: a phone's camera makes 12 MP pictures and its "108 MP" mode makes 12000x9000.
// Decoded as RGBA8 that is 432 MB in one allocation, before the GPU texture that follows, on a
// device whose whole heap is 2-4 GB and whose largest single allocation is often far less. The
// failure is an OOM the OS kills the app for, with no message, in the middle of an import; the
// engine's own decoders (stb_image and friends) allocate the full bitmap before anything can ask.
// So the import path asks here first: the header gives the dimensions, the policy says whether to
// take it, and by how much it must be scaled down when not.
//
// Only the header is parsed (no pixel data is read), so a 400 MB TIFF costs a few hundred bytes
// and a seek. Formats the engine imports: PNG, JPEG, GIF, BMP, WebP, TGA and PSD; the rest answer
// Unknown and the policy lets the decoder decide as it does today (nothing is refused that was
// not refused before).

#include "AndroidAssetImport.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace jasperengine::android {

    enum class ImageFormat : uint8_t {
        Unknown = 0,
        Png,
        Jpeg,
        Gif,
        Bmp,
        Webp,
        Psd,
        Tga,
    };

    struct ImageSize {
        bool known = false;
        ImageFormat format = ImageFormat::Unknown;
        int width = 0;
        int height = 0;
        // channels the decoder will produce (3 for JPEG/PNG-without-alpha, 4 where alpha is
        // possible); 0 when the format's header does not say, and then 4 is assumed: the guess
        // has to be the large one, since the budget is what is being checked
        int channels = 0;
        // PNG's bit depth (8 or 16); 8 elsewhere. 16 doubles the decoded size
        int bitDepth = 8;

        // bytes the decoded image takes at `channels` of `bitDepth` bits each, per pixel. A
        // product that does not fit in 64 bits answers the largest value (it cannot be allowed).
        uint64_t decodedBytes() const {
            if (!known || width <= 0 || height <= 0) return 0;
            const uint64_t pixel = (uint64_t)(channels > 0 ? channels : 4) * (uint64_t)(bitDepth > 0 ? bitDepth : 8) / 8u;
            const uint64_t pixels = (uint64_t)width * (uint64_t)height;
            if (pixel != 0 && pixels > UINT64_MAX / pixel) return UINT64_MAX;
            return pixels * pixel;
        }

        uint64_t megapixels_x100() const {
            if (!known) return 0;
            return (uint64_t)width * (uint64_t)height / 10000u;
        }
    };

    namespace detail {
        inline uint32_t readBig(const uint8_t* data) {
            return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) | ((uint32_t)data[2] << 8) | (uint32_t)data[3];
        }
        inline uint16_t readBig16(const uint8_t* data) {
            return (uint16_t)(((uint16_t)data[0] << 8) | (uint16_t)data[1]);
        }
        inline uint32_t readLittle(const uint8_t* data) {
            return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) | ((uint32_t)data[1] << 8) | (uint32_t)data[0];
        }
        inline uint16_t readLittle16(const uint8_t* data) {
            return (uint16_t)(((uint16_t)data[1] << 8) | (uint16_t)data[0]);
        }
    }

    // The image's dimensions from its header, false when the bytes are not a format whose header
    // carries them or the header is cut short.
    //
    // `prefix` need not be the whole file, but must reach the dimensions: 33 bytes for PNG, BMP and
    // WebP-VP8X, up to ~1 KB for a JPEG whose SOF marker follows an EXIF block (a caller reading a
    // file should pass the first 64 KiB). A header that lies (a zero or negative size, or one past
    // INT_MAX) answers false rather than a size the decoder would refuse.
    inline ImageSize readImageSize(std::span<const uint8_t> prefix) {
        using detail::readBig;
        using detail::readBig16;
        using detail::readLittle;
        using detail::readLittle16;
        ImageSize size;

        auto accept = [&size](ImageFormat format, uint32_t width, uint32_t height, int channels, int bitDepth) {
            if (width == 0 || height == 0 || width > 0x7FFFFFFFu || height > 0x7FFFFFFFu) return false;
            size.known = true;
            size.format = format;
            size.width = (int)width;
            size.height = (int)height;
            size.channels = channels;
            size.bitDepth = bitDepth;
            return true;
        };

        // PNG: IHDR is the first chunk, always at 8
        if (prefix.size() >= 26 && prefix[0] == 0x89 && prefix[1] == 'P' && prefix[2] == 'N' && prefix[3] == 'G' &&
            prefix[12] == 'I' && prefix[13] == 'H' && prefix[14] == 'D' && prefix[15] == 'R') {
            const uint32_t width = readBig(prefix.data() + 16);
            const uint32_t height = readBig(prefix.data() + 20);
            const int bitDepth = prefix[24];
            const int colorType = prefix[25];
            // 4 and 6 carry alpha; 0, 2 and 3 do not before the PNG's own rules expand a palette
            const int channels = (colorType == 4 || colorType == 6) ? 4 : (colorType == 0 ? 1 : (colorType == 3 ? 4 : 3));
            accept(ImageFormat::Png, width, height, channels, bitDepth == 16 ? 16 : 8);
            return size;
        }

        // GIF: the logical screen descriptor at 6, little endian
        if (prefix.size() >= 10 && prefix[0] == 'G' && prefix[1] == 'I' && prefix[2] == 'F' && prefix[3] == '8') {
            accept(ImageFormat::Gif, readLittle16(prefix.data() + 6), readLittle16(prefix.data() + 8), 4, 8);
            return size;
        }

        // BMP: a DIB header whose first field is its own size; height may be negative (top-down)
        if (prefix.size() >= 26 && prefix[0] == 'B' && prefix[1] == 'M') {
            const uint32_t dib = readLittle(prefix.data() + 14);
            if (dib == 12) {
                // BITMAPCOREHEADER: 16-bit fields
                accept(ImageFormat::Bmp, readLittle16(prefix.data() + 18), readLittle16(prefix.data() + 20), 4, 8);
            } else if (dib >= 40) {
                const uint32_t width = readLittle(prefix.data() + 18);
                const int32_t height = (int32_t)readLittle(prefix.data() + 22);
                const uint32_t bits = readLittle16(prefix.data() + 28);
                accept(ImageFormat::Bmp, width, (uint32_t)(height < 0 ? -height : height), bits == 32 ? 4 : 3, 8);
            }
            return size;
        }

        // WebP: VP8X has the canvas size as two 24-bit values; a plain VP8/VP8L needs its own
        // bit layout, which is here because a phone exporter writes those
        if (prefix.size() >= 30 && prefix[0] == 'R' && prefix[1] == 'I' && prefix[2] == 'F' && prefix[3] == 'F' &&
            prefix[8] == 'W' && prefix[9] == 'E' && prefix[10] == 'B' && prefix[11] == 'P') {
            const char chunk0 = (char)prefix[12];
            const char chunk1 = (char)prefix[13];
            const char chunk2 = (char)prefix[14];
            if (chunk0 == 'V' && chunk1 == 'P' && chunk2 == '8' && prefix[15] == 'X') {
                const uint32_t width = 1u + (uint32_t)(prefix[24] | (prefix[25] << 8) | (prefix[26] << 16));
                const uint32_t height = 1u + (uint32_t)(prefix[27] | (prefix[28] << 8) | (prefix[29] << 16));
                // the flag byte says whether an alpha chunk follows; assume it does
                accept(ImageFormat::Webp, width, height, 4, 8);
            } else if (chunk0 == 'V' && chunk1 == 'P' && chunk2 == '8' && prefix[15] == ' ') {
                // VP8: a 3-byte frame tag, the start code, then 14-bit width and height
                if (prefix.size() >= 30 && prefix[23] == 0x9D && prefix[24] == 0x01 && prefix[25] == 0x2A) {
                    const uint32_t width = (uint32_t)(readLittle16(prefix.data() + 26) & 0x3FFFu);
                    const uint32_t height = (uint32_t)(readLittle16(prefix.data() + 28) & 0x3FFFu);
                    accept(ImageFormat::Webp, width, height, 3, 8);
                }
            } else if (chunk0 == 'V' && chunk1 == 'P' && chunk2 == '8' && prefix[15] == 'L') {
                // VP8L: 14 bits each of width-1 and height-1 after the 0x2F signature
                if (prefix.size() >= 25 && prefix[20] == 0x2F) {
                    const uint32_t bits = readLittle(prefix.data() + 21);
                    const uint32_t width = 1u + (bits & 0x3FFFu);
                    const uint32_t height = 1u + ((bits >> 14) & 0x3FFFu);
                    accept(ImageFormat::Webp, width, height, 4, 8);
                }
            }
            return size;
        }

        // PSD: the header's height then width, both big endian 32-bit
        if (prefix.size() >= 26 && prefix[0] == '8' && prefix[1] == 'B' && prefix[2] == 'P' && prefix[3] == 'S') {
            accept(ImageFormat::Psd, readBig(prefix.data() + 18), readBig(prefix.data() + 14), 4, 8);
            return size;
        }

        // TGA has no magic: its header's image type byte is 0/1/2/3/9/10/11 and the sizes are 16-bit
        // little endian at 12. Accepted only when that byte is one of those, so a random file is not
        // read as a TGA (a colour-mapped or true-colour image).
        if (prefix.size() >= 18) {
            const uint8_t type = prefix[2];
            const bool plausible = type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11;
            const uint16_t width = readLittle16(prefix.data() + 12);
            const uint16_t height = readLittle16(prefix.data() + 14);
            if (plausible && width > 0 && height > 0 && prefix[16] <= 32) {
                accept(ImageFormat::Tga, width, height, prefix[16] == 32 ? 4 : 3, 8);
                return size;
            }
        }

        // JPEG: walk the markers to the first SOF one, which carries the size. A segment's length
        // is big endian and includes its own two bytes.
        if (prefix.size() >= 4 && prefix[0] == 0xFF && prefix[1] == 0xD8) {
            size_t at = 2;
            while (at + 4 <= prefix.size()) {
                if (prefix[at] != 0xFF) return size;   // not a marker: a corrupt or a non-JPEG stream
                const uint8_t marker = prefix[at + 1];
                if (marker == 0xFF) { at++; continue; }               // a fill byte
                if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) { at += 2; continue; }
                if (marker == 0xD9 || marker == 0xDA) return size;    // end of image, or scan before any SOF
                const uint16_t length = readBig16(prefix.data() + at + 2);
                if (length < 2) return size;
                // SOF0..SOF15 except the DHT (C4), JPG (C8) and DAC (CC) markers
                const bool isSof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
                if (isSof) {
                    if (at + 9 > prefix.size()) return size;
                    const uint32_t height = readBig16(prefix.data() + at + 5);
                    const uint32_t width = readBig16(prefix.data() + at + 7);
                    const int channels = prefix[at + 9];
                    accept(ImageFormat::Jpeg, width, height, channels >= 1 && channels <= 4 ? channels : 3, 8);
                    return size;
                }
                at += 2u + (size_t)length;
            }
        }
        return size;
    }

    // What an import may do with the image.
    enum class ImageVerdict : uint8_t {
        Allowed = 0,
        // it does not fit as it is, but fits at `scale` (0 < scale < 1) of each side
        Downscale,
        // past the format's own limit, or so large that no downscale of a decode is sensible
        Refused,
    };

    struct ImageBudget {
        // bytes one decoded image may take; 0 means "no limit known" (a platform that did not say),
        // and then nothing is refused on size, as today
        uint64_t decodedBytesLimit = 0;
        // the longest side a GPU texture may have (Android's GLES floor is 2048; 4096 is what every
        // GLES 3.0 device this engine targets reports, and the tier system lowers it further)
        int maxTextureSize = 4096;
        // of the image's own pixels, before any limit: refusing a 300 MP image outright beats
        // computing a scale for a decode that would take minutes
        uint64_t maxPixels = 120ull * 1000ull * 1000ull;
    };

    struct ImageDecision {
        ImageVerdict verdict = ImageVerdict::Allowed;
        // 1.0 when allowed as it is; the largest scale of each side that fits otherwise
        double scale = 1.0;
        // the target size after that scale (the source's when allowed)
        int width = 0;
        int height = 0;
        // a player-facing sentence, "" when allowed
        std::string message;
    };

    // Whether an import may decode `size` within `budget`, and the scale it needs when not.
    //
    // The scale is the largest power-of-two reduction (1, 1/2, 1/4, ...) that fits both the byte
    // limit and maxTextureSize: a decoder resamples by halves cheaply and exactly, and a 2:1 or 4:1
    // decimation is what an importer applies anyway. A size that does not fit even at 1/32 is
    // refused: below that the picture is a thumbnail of itself, and the honest answer is to say so.
    inline ImageDecision decideImageImport(const ImageSize& size, const ImageBudget& budget) {
        ImageDecision decision;
        if (!size.known) {
            // an unreadable header: the decoder decides, as before
            return decision;
        }
        decision.width = size.width;
        decision.height = size.height;

        const uint64_t pixels = (uint64_t)size.width * (uint64_t)size.height;
        if (budget.maxPixels > 0 && pixels > budget.maxPixels) {
            decision.verdict = ImageVerdict::Refused;
            decision.message = "the image is " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                               ", past the largest this engine imports";
            return decision;
        }

        const bool sizeFits = budget.decodedBytesLimit == 0 || size.decodedBytes() <= budget.decodedBytesLimit;
        const bool textureFits = budget.maxTextureSize <= 0 ||
                                 (size.width <= budget.maxTextureSize && size.height <= budget.maxTextureSize);
        if (sizeFits && textureFits) {
            return decision;
        }

        for (int halvings = 1; halvings <= 5; halvings++) {
            const int width = size.width >> halvings;
            const int height = size.height >> halvings;
            if (width < 1 || height < 1) break;
            ImageSize scaled = size;
            scaled.width = width;
            scaled.height = height;
            const bool fits = (budget.decodedBytesLimit == 0 || scaled.decodedBytes() <= budget.decodedBytesLimit) &&
                              (budget.maxTextureSize <= 0 || (width <= budget.maxTextureSize && height <= budget.maxTextureSize));
            if (!fits) continue;
            decision.verdict = ImageVerdict::Downscale;
            decision.scale = 1.0 / (double)(1 << halvings);
            decision.width = width;
            decision.height = height;
            // the decoded size in MB only when it is one: "(0 MB decoded)" on a 3x3 icon is noise
            // that makes the sentence read like a bug report rather than an answer
            const uint64_t megabytes = size.decodedBytes() / (1024ull * 1024ull);
            decision.message = "the image is " + std::to_string(size.width) + "x" + std::to_string(size.height) +
                               (megabytes > 0 ? " (" + std::to_string(megabytes) + " MB decoded)" : std::string()) +
                               "; it is imported at " + std::to_string(width) + "x" + std::to_string(height);
            return decision;
        }

        decision.verdict = ImageVerdict::Refused;
        decision.message = "the image is too large for this device to decode, even at a quarter of its size";
        return decision;
    }

    // (the second block below adds decode support)

}

// ---- what this engine can actually decode
//
// The engine decodes a texture through the vendored stb_image (engine/core/texture/TextureData.cpp,
// `stbi_load_from_memory`), plus its own KTX2/Basis path and the SVG/EXR/HDR decoders beside it.
// stb_image reads PNG, JPEG, BMP, TGA, GIF, PSD, PNM, HDR and PIC; it has **no WebP decoder**, and
// no HEIC/AVIF one either. `.webp` is nevertheless in the editor's Texture extensions
// (editor/asset/AssetTypes.cpp), so a phone photo shared as a WebP is accepted by an import, gets a
// texture name, and fails later at decode with "Error loading texture" — on a phone, where the
// share sheet offers WebP for photos.
//
// So an import asks first. This is the one place that knows the answer without a GPU or a decode:
// the same verdict `assetTypeForImport` gives, plus whether a decoder exists for those bytes.

namespace jasperengine::android {

    enum class ImageSupport : uint8_t {
        // a decoder in this build reads it
        Supported = 0,
        // no decoder for the container; `convertTo` names a container a decoder does read, and the
        // import must convert before it writes (a phone can do it through BitmapFactory, which
        // decodes WebP and HEIC/AVIF on Android 12+)
        NeedsConversion,
        // no decoder, and no conversion either (a format nothing in the engine or the platform
        // reads): the import is refused with `reason`
        Unsupported,
    };

    struct ImageSupportAnswer {
        ImageSupport support = ImageSupport::Supported;
        ImageFormat format = ImageFormat::Unknown;
        // for NeedsConversion: "png", the container the import should be converted to
        const char* convertTo = nullptr;
        // a sentence for the user (an empty one when Supported)
        std::string reason;
    };

    // Whether the engine can decode the bytes at `path` (its extension only matters when the bytes
    // say nothing). `androidBitmapFactory` is what the caller knows about the device: Android's own
    // `BitmapFactory` decodes WebP on every API level this engine targets, and HEIC/AVIF from 12
    // (API 31), so the platform can convert what stb_image cannot read.
    inline ImageSupportAnswer decodeSupportOf(std::string_view fileName, std::span<const uint8_t> bytes,
                                             bool androidBitmapFactory) {
        ImageSupportAnswer answer;
        (void)fileName;   // the bytes decide; the extension matters only for a format with no magic
        const ImportKind kind = kindFromMagic(bytes);
        auto formatOf = [](ImportKind k) {
            switch (k) {
                case ImportKind::Png: return ImageFormat::Png;
                case ImportKind::Jpeg: return ImageFormat::Jpeg;
                case ImportKind::Gif: return ImageFormat::Gif;
                case ImportKind::Bmp: return ImageFormat::Bmp;
                case ImportKind::Webp: return ImageFormat::Webp;
                case ImportKind::Psd: return ImageFormat::Psd;
                case ImportKind::Tga: return ImageFormat::Tga;
                default: return ImageFormat::Unknown;
            }
        };
        answer.format = formatOf(kind);

        switch (kind) {
            case ImportKind::Png:
            case ImportKind::Jpeg:
            case ImportKind::Gif:
            case ImportKind::Bmp:
            case ImportKind::Psd:
            case ImportKind::Tga:
            case ImportKind::Hdr:
            case ImportKind::Ktx2:
            case ImportKind::Unknown:
                // Unknown is a text format, a container with no magic of its own, or not an image:
                // the decoder decides, as before
                answer.support = ImageSupport::Supported;
                return answer;
            case ImportKind::Webp:
                if (androidBitmapFactory) {
                    answer.support = ImageSupport::NeedsConversion;
                    answer.convertTo = "png";
                    answer.reason = "this build has no WebP decoder; the photo is converted to PNG when it is imported";
                } else {
                    answer.support = ImageSupport::NeedsConversion;
                    answer.convertTo = "png";
                    answer.reason = "this build has no WebP decoder; the photo must be converted to PNG before it can be used";
                }
                return answer;
            case ImportKind::OpenExr:
                // tinyexr is linked, and TextureData has a float decoder registry for it
                answer.support = ImageSupport::Supported;
                return answer;
            default:
                break;
        }
        answer.support = ImageSupport::Unsupported;
        answer.reason = "no decoder in this engine reads this image format; export it as PNG or JPEG";
        return answer;
    }

}

#endif // ANDROID_IMAGE_LIMITS_H
