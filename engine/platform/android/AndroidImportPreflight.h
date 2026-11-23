// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_IMPORT_PREFLIGHT_H
#define ANDROID_IMPORT_PREFLIGHT_H

// What a project's assets will do when they are imported on a phone, answered from the bytes
// alone, before anything is packaged or the game runs.
//
// The four pieces this composes each answer one question on their own (what the file is, whether
// its format decodes, whether its size fits, which way up it is); none of them can answer "is my
// project going to work on this device", and the answer to that is a list of files with a reason
// each. The expensive failures on a phone are the ones a desktop never shows: a .webp texture that
// the editor accepted because the extension is in its table and the engine has no decoder for it
// (ANDROID_BUGS 6a), a 108 MP photo whose decode peaks at 432 MB (6), a portrait photo that lands
// sideways (6c), and two files the phone's content provider named the same (7).
//
// The report is a value: computed for a set of files, printed by the editor before Play, or
// asserted by a test. Nothing here reads a file or touches Android.

#include "AndroidAssetImport.h"
#include "AndroidImageLimits.h"
#include "AndroidImageOrientation.h"
#include "AndroidModelSidecars.h"
#include "asset/AssetType.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace jasperengine::android {

    enum class PreflightSeverity : uint8_t {
        // nothing to do: the import gives the asset the engine expects
        Fine = 0,
        // it imports, but what you get is not what you see on the desktop (a downscale, a
        // converted container, a rotated photo)
        Note,
        // it imports and will not draw or will not play
        Broken,
    };

    struct PreflightOptions {
        // what the platform can decode beyond the engine's own decoders; both are what a phone
        // reports at startup (BitmapFactory for images, MediaCodec for audio)
        bool androidBitmapFactory = false;
        bool androidMediaCodec = false;
        // the device's budget; 0/0 leaves the limits to the caller's ImageBudget
        ImageBudget budget;
        // below this, one decode's memory is not worth a line in the report: a 100x100 icon
        // announcing "0.0 MB of memory" is noise that hides the 432 MB photo next to it
        uint64_t memoryNoteThreshold = 16ull << 20;
        // the bytes this device has for textures once the project is loaded: 0 for "not known".
        // A phone's GPU heap is a fraction of its RAM, and a project that loads on a desktop
        // fails here rather than at any single file
        uint64_t residentLimitBytes = 0;
        // the folder the imports land in, for the name collisions: a set of names already taken.
        // nullptr: no folder, every name is free
        const std::vector<std::string>* takenNames = nullptr;
    };

    // One file, as the platform hands it over: the name it arrives with, the first bytes (enough
    // for the magic and the image header; 64 KiB is plenty and what an importer reads), and the
    // size on disk when it is known (0 when it is not).
    struct PreflightInput {
        std::string fileName;
        std::vector<uint8_t> prefix;
        uint64_t fileBytes = 0;
    };

    struct PreflightFinding {
        std::string fileName;       // as it arrived
        std::string assetName;      // what the import will call it
        AssetType type = AssetType::Unknown;
        PreflightSeverity severity = PreflightSeverity::Fine;
        // one line, a sentence the user reads: "" only when nothing is worth saying
        std::string message;
        // the image's own size, 0 when it is not an image
        int width = 0;
        int height = 0;
        // the size it is imported at (after the budget's downscale and, for a photo that is not
        // upright, the orientation's swap)
        int importedWidth = 0;
        int importedHeight = 0;
        // EXIF orientation, 1..8; 0 when the file has none (which is upright)
        uint32_t orientation = 0;
        // a model's external dependencies, by the URI the file wrote (empty for anything else)
        std::vector<std::string> missingSidecars;
        // and the name each of them lands under once the import flattens the folder: the caller
        // asks for that file and writes it under this name (a two-model project cannot collide)
        std::vector<std::string> sidecarAssetNames;
        // the image is reduced to this scale of each side (1.0 when it is not)
        double scale = 1.0;
    };

    struct PreflightReport {
        std::vector<PreflightFinding> findings;   // in the order the files were given
        // the tallest peak one decode of these files needs, when the platform knows the sizes
        uint64_t largestDecodedBytes = 0;
        // every texture's decoded size added up: the textures are resident together, so this is
        // what the GPU heap pays once the project loads, not the peak of the largest one
        uint64_t residentTextureBytes = 0;
        // the memory a phone is assumed to have for textures: 0 means "not known, say nothing".
        // A budget overrun is reported once, for the project, because no single file is at fault
        uint64_t residentLimitBytes = 0;
        // true when residentTextureBytes is past residentLimitBytes: the project loads on a desktop
        // and runs out of memory on the phone it was built for
        bool overResidentBudget = false;
        // the images whose size a downscale would bring the project back inside the budget (the
        // largest first: three of them are usually enough), empty when it is inside already
        std::vector<std::string> suggestDownscale;
        // the files that will not draw or will not play, whether or not the import is attempted
        int broken = 0;
        int notes = 0;
        // "" when the project imports as it is; else the first thing to fix, in the report's order
        std::string summary;
    };

    namespace detail {

        inline bool preflightIsImage(AssetType type) {
            return type == AssetType::Texture;
        }

        inline bool preflightIsAudio(AssetType type) {
            return type == AssetType::Audio;
        }

        // "4000x3000" or "" for a size that is not known
        inline std::string preflightSizeText(int width, int height) {
            if (width <= 0 || height <= 0) return std::string();
            return std::to_string(width) + "x" + std::to_string(height);
        }

        inline std::string preflightPercent(double scale) {
            // 1/2 -> "50%", 1/4 -> "25%": the report says what is kept, not what is dropped
            const int percent = (int)(scale * 100.0 + 0.5);
            return std::to_string(percent) + "%";
        }

        inline std::string preflightMegabytes(uint64_t bytes) {
            // a decode is megabytes, and one decimal is the precision a person acts on
            const uint64_t tenths = (bytes * 10u + (1ull << 19)) >> 20;
            return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " MB";
        }

        // Whether a name is already taken, and what a second file with the same name becomes: the
        // report has to name the file the import will produce, which is uniqueFileName's answer
        inline bool preflightNameTaken(const std::vector<std::string>* taken, const std::string& name) {
            if (!taken) return false;
            for (const std::string& entry : *taken) {
                if (entry == name) return true;
            }
            return false;
        }

    }

    // The report of one file, against the options. Exposed because a caller inspecting a single
    // import (a drop on the editor's Project panel) wants exactly this and not a list.
    inline PreflightFinding preflightOne(const PreflightInput& input, const PreflightOptions& options) {
        PreflightFinding finding;
        finding.fileName = input.fileName;
        // a camera hands over "DCIM/IMG_4821.JPG" and a provider a content:// URI; the asset is
        // the leaf, which is what importedAssetName() is meant to be given
        const std::string leaf = uriFileName(input.fileName);
        finding.type = assetTypeForImport(leaf, input.prefix);
        finding.assetName = importedAssetName(leaf, input.prefix);
        if (detail::preflightNameTaken(options.takenNames, finding.assetName)) {
            finding.assetName = uniqueFileName(finding.assetName, [&options](const std::string& name) {
                return detail::preflightNameTaken(options.takenNames, name);
            });
        }
        const std::span<const uint8_t> bytes(input.prefix.data(), input.prefix.size());

        if (finding.type == AssetType::Unknown) {
            // not an asset at all: the import leaves it where it is, which is not a failure
            finding.message = "not an asset type this engine imports; it is copied as it is";
            return finding;
        }

        if (detail::preflightIsImage(finding.type)) {
            const ImageSupportAnswer support = decodeSupportOf(std::string_view(leaf), bytes, options.androidBitmapFactory);
            const ImageSize size = readImageSize(bytes);
            if (size.known) {
                finding.width = size.width;
                finding.height = size.height;
            }
            if (support.support == ImageSupport::Unsupported ||
                (support.support == ImageSupport::NeedsConversion && !options.androidBitmapFactory)) {
                // a conversion that needs a decoder the device does not have is a texture that
                // never draws: reporting it as a note would be the same "Error loading texture"
                // the extension table produces today (ANDROID_BUGS 6a)
                finding.severity = PreflightSeverity::Broken;
                finding.message = support.reason.empty() ? "this engine cannot decode it" : support.reason;
                return finding;
            }

            // the orientation first: it decides which side is which for every limit below
            finding.orientation = readOrientation(bytes);
            const Orientation orientation = orientationFromValue(finding.orientation);
            const OrientationTransform transform = transformFor(orientation);
            int shownWidth = size.known ? size.width : 0;
            int shownHeight = size.known ? size.height : 0;
            if (transform.swapDimensions) {
                const int side = shownWidth;
                shownWidth = shownHeight;
                shownHeight = side;
            }

            ImageDecision decision;
            if (size.known) {
                decision = decideImageImport(size, options.budget);
                if (decision.verdict == ImageVerdict::Refused) {
                    finding.severity = PreflightSeverity::Broken;
                    finding.message = decision.message;
                    return finding;
                }
            }
            finding.scale = decision.scale;
            finding.importedWidth = shownWidth > 0 ? (int)(shownWidth * decision.scale) : 0;
            finding.importedHeight = shownHeight > 0 ? (int)(shownHeight * decision.scale) : 0;

            const uint64_t decoded = size.known ? size.decodedBytes() : 0;
            std::string message;
            if (support.support == ImageSupport::NeedsConversion) {
                // WebP/HEIC: the platform converts, so it draws; on a device without the
                // platform decoder it is broken above, and here it is only a note
                message = "converted to " + std::string(support.convertTo ? support.convertTo : "png") +
                          " on import (" + (support.reason.empty() ? std::string("this engine has no decoder") : support.reason) + ")";
            }
            if (size.known && decision.verdict == ImageVerdict::Downscale) {
                const std::string from = detail::preflightSizeText(size.width, size.height);
                const std::string to = detail::preflightSizeText(finding.importedWidth, finding.importedHeight);
                if (!message.empty()) message += "; ";
                message += "reduced from " + from + " to " + to + " (" + detail::preflightPercent(decision.scale) +
                           " of each side) to fit a " + std::to_string(options.budget.maxTextureSize) + " texture";
            }
            if (decoded >= options.memoryNoteThreshold) {
                if (!message.empty()) message += "; ";
                message += "one decode peaks at " + detail::preflightMegabytes(decoded) + " of memory";
            }
            if (transform.needed) {
                if (!message.empty()) message += "; ";
                message += std::string("stored ") + orientationName(orientation) + " and turned upright on import";
            }
            finding.message = message;
            if (!message.empty() && finding.severity == PreflightSeverity::Fine) {
                finding.severity = PreflightSeverity::Note;
            }
            return finding;
        }

        if (detail::preflightIsAudio(finding.type)) {
            const MediaSupportAnswer support = playbackSupportOf(std::string_view(leaf), bytes, options.androidMediaCodec);
            if (support.support == MediaSupport::Unsupported) {
                finding.severity = PreflightSeverity::Broken;
                finding.message = support.reason.empty() ? "this engine cannot play it" : support.reason;
                return finding;
            }
            if (support.support == MediaSupport::NeedsConversion) {
                finding.severity = PreflightSeverity::Note;
                finding.message = "converted to " + std::string(support.convertTo ? support.convertTo : "wav") +
                                  " on import (" + (support.reason.empty() ? std::string("no decoder") : support.reason) + ")";
            }
            return finding;
        }

        if (finding.type == AssetType::Model) {
            // an ASCII .gltf may name buffers and textures beside itself, which a phone cannot
            // hand over as one import: the report has to say which files to bring
            if (isAsciiGltf(leaf)) {
                const std::string gltfText(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                const SidecarReport sidecars = gltfSidecars(gltfText);
                finding.missingSidecars = sidecars.requiredFiles();
                const ModelImportPlan import = planModelImport(std::string(gltfText), leaf);
                for (const SidecarImport& sidecar : import.sidecars) {
                    finding.sidecarAssetNames.push_back(sidecar.assetName);
                }
                const std::string sentence = sidecarSentence(sidecars);
                if (!sentence.empty()) {
                    finding.severity = sidecars.parsed ? PreflightSeverity::Note : PreflightSeverity::Broken;
                    finding.message = sentence;
                }
                return finding;
            }
            // a model is binary glTF or FBX; the runtime has a glTF reader and no FBX one, and the
            // extension alone cannot tell a .glb from a .gltf that names external buffers
            const ImportKind kind = kindFromMagic(bytes);
            if (kind == ImportKind::Unknown) {
                const std::string extension = extensionOf(input.fileName);
                if (extension == "fbx") {
                    finding.severity = PreflightSeverity::Note;
                    finding.message = "FBX is converted to glTF by the editor's pipeline, not by the player";
                } else {
                    finding.severity = PreflightSeverity::Note;
                    finding.message = "the header is not a model this engine reads; it is imported as text";
                }
            }
            return finding;
        }

        return finding;
    }

    // The report of a whole project's imports.
    inline PreflightReport preflightImports(const std::vector<PreflightInput>& inputs, const PreflightOptions& options) {
        PreflightReport report;
        report.findings.reserve(inputs.size());
        report.residentLimitBytes = options.residentLimitBytes;
        std::vector<std::pair<uint64_t, std::string>> textureSizes;
        for (const PreflightInput& input : inputs) {
            PreflightFinding finding = preflightOne(input, options);
            ImageSize size;
            size.known = finding.width > 0 && finding.height > 0;
            size.width = finding.width;
            size.height = finding.height;
            const uint64_t decoded = size.decodedBytes();
            if (decoded > report.largestDecodedBytes) report.largestDecodedBytes = decoded;
            if (finding.type == AssetType::Texture && decoded > 0) {
                // a texture's decoded bytes are what stays resident (the import downscales the
                // decode, so the encoded file is not the number that matters)
                const double scale = finding.scale > 0.0 && finding.scale < 1.0 ? finding.scale : 1.0;
                report.residentTextureBytes += (uint64_t)((double)decoded * scale * scale);
                textureSizes.push_back({decoded, finding.fileName});
            }
            if (finding.severity == PreflightSeverity::Broken) {
                if (report.summary.empty()) {
                    report.summary = finding.fileName + ": " + finding.message;
                }
                report.broken++;
            } else if (finding.severity == PreflightSeverity::Note) {
                report.notes++;
            }
            report.findings.push_back(std::move(finding));
        }
        if (report.residentLimitBytes > 0 && report.residentTextureBytes > report.residentLimitBytes) {
            report.overResidentBudget = true;
            // the files that would bring it back inside, largest first: downscaling one texture is
            // a decision a user can act on, and three of them usually close a gap of this shape
            uint64_t over = report.residentTextureBytes - report.residentLimitBytes;
            // largest first, and by name within a size: std::sort alone leaves equally sized
            // textures in an order that depends on the input, so the advice would change between
            // two runs of the same report
            std::sort(textureSizes.begin(), textureSizes.end(),
                      [](const auto& left, const auto& right) {
                          if (left.first != right.first) return left.first > right.first;
                          return left.second < right.second;
                      });
            for (const auto& entry : textureSizes) {
                if (over == 0) break;
                report.suggestDownscale.push_back(entry.second);
                // half of each side is a quarter of the bytes: the cheapest real reduction
                const uint64_t saved = entry.first - entry.first / 4;
                over = saved >= over ? 0 : over - saved;
            }
        }
        if (report.summary.empty() && report.overResidentBudget) {
            report.summary = "the project's textures take " + detail::preflightMegabytes(report.residentTextureBytes) +
                             ", past the " + detail::preflightMegabytes(report.residentLimitBytes) +
                             " this device has for them";
        }
        if (report.summary.empty() && report.notes > 0) {
            report.summary = std::to_string(report.notes) + (report.notes == 1 ? " import is" : " imports are") +
                             " not what you see on the desktop";
        }
        return report;
    }

}

#endif /* ANDROID_IMPORT_PREFLIGHT_H */
