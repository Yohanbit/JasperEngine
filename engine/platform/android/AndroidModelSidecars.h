// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_MODEL_SIDECARS_H
#define ANDROID_MODEL_SIDECARS_H

// Which files a model needs beside it, and whether a phone can give them to it.
//
// A .glb is self-contained: one file, nothing to lose. An ASCII .gltf is not. Its `buffers` and
// `images` name files by relative URI ("Body.bin", "textures/skin.png"), and the engine resolves
// them through tinygltf's file callbacks, relative to the directory of the .gltf
// (MeshSystem.cpp:2116). On a phone there is no directory: a picker hands over one
// `content://` URI per file, each with no path and no relation to the others, so a .gltf that
// arrives alone loads with missing geometry and missing textures. The engine already knows this
// happens and *suppresses* the warnings for a cooked model (MeshSystem.cpp:455-482), which is
// right for a cook and wrong for an import: the user is told nothing.
//
// This reads the .gltf's own JSON and answers the question an import dialog asks: what else do I
// need to bring, and can I get it? Nothing here decodes an image or opens a file.

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"

#include "AndroidAssetImport.h"   // percentDecode: a glTF URI is percent-encoded, a file name is not

namespace jasperengine::android {

    // What a glTF names as a dependency.
    enum class SidecarKind : uint8_t {
        Buffer = 0,   // geometry, animation, skinning: without it there is no mesh at all
        Image,        // a texture: without it the mesh is there and untextured
    };

    struct Sidecar {
        std::string uri;          // as the glTF wrote it, percent-encoded, maybe a data: URI
        SidecarKind kind = SidecarKind::Buffer;
        // a data: URI carries its bytes in the file: nothing to bring. Counted, not listed, since
        // there is nothing the user can do about it and on a phone it is often the whole model
        bool embedded = false;
        // the glTF points at a bufferView instead of a URI: also embedded
        bool fromBufferView = false;
    };

    struct SidecarReport {
        std::vector<Sidecar> sidecars;     // in the file's order, embedded ones included
        int externalBuffers = 0;
        int externalImages = 0;
        // false when the text is not a readable glTF: `error` then says why
        bool parsed = false;
        std::string error;
        // every external URI, de-duplicated (a texture atlas named by four materials is one file)
        std::vector<std::string> requiredFiles() const {
            std::set<std::string> unique;
            for (const Sidecar& sidecar : sidecars) {
                if (!sidecar.embedded && !sidecar.fromBufferView) unique.insert(sidecar.uri);
            }
            return std::vector<std::string>(unique.begin(), unique.end());
        }
        // true when the model loads from one file (a .glb, or a .gltf with everything inline)
        bool isSelfContained() const { return externalBuffers == 0 && externalImages == 0; }
    };

    namespace detail {

        // A glTF URI is a URI, not a path: it may be percent-encoded UTF-8, and on Windows it may
        // have backslashes a hand-edited exporter left. Returns the name a picker would show, which
        // is decoded: a report that names "skin%20map.png" names a file that does not exist.
        inline std::string sidecarFileName(std::string_view uri) {
            std::string name(uri);
            // the query and fragment are not part of the name
            const size_t cut = name.find_first_of("?#");
            if (cut != std::string::npos) name.resize(cut);
            // a URI's separators are always '/' (a backslash is a character a file may have);
            // exporters on Windows write one anyway, and a file named "a\b.png" is not what they meant
            for (char& c : name) {
                if (c == '\\') c = '/';
            }
            const size_t slash = name.find_last_of('/');
            if (slash != std::string::npos) name = name.substr(slash + 1);
            std::string decoded;
            percentDecode(name, decoded);
            return decoded;
        }

        inline bool isDataUri(std::string_view uri) {
            return uri.size() >= 5 && uri.compare(0, 5, "data:") == 0;
        }
    }

    // The dependencies of an ASCII glTF, from its text.
    //
    // A malformed document answers `parsed == false` with the JSON error, because an import that
    // silently reports "no dependencies" for a file that does not parse is worse than one that
    // says the model is broken: the user would bring the .gltf and nothing else.
    inline SidecarReport gltfSidecars(std::string_view text) {
        SidecarReport report;
        nlohmann::json document;
        try {
            document = nlohmann::json::parse(text.begin(), text.end());
        } catch (const std::exception& error) {
            report.error = error.what();
            return report;
        }
        if (!document.is_object()) {
            report.error = "the file is not a JSON object";
            return report;
        }
        report.parsed = true;

        auto readArray = [&document](const char* key) -> const nlohmann::json* {
            const auto it = document.find(key);
            if (it == document.end() || !it->is_array()) return nullptr;
            return &*it;
        };

        if (const nlohmann::json* buffers = readArray("buffers")) {
            for (const nlohmann::json& buffer : *buffers) {
                if (!buffer.is_object()) continue;
                Sidecar sidecar;
                sidecar.kind = SidecarKind::Buffer;
                const auto uri = buffer.find("uri");
                if (uri == buffer.end() || !uri->is_string()) {
                    // a GLB's single buffer has no URI: its bytes are in the BIN chunk
                    sidecar.fromBufferView = true;
                    report.sidecars.push_back(sidecar);
                    continue;
                }
                sidecar.uri = uri->get<std::string>();
                // an empty URI names no file: it is as unusable as an absent one, and listing it
                // would tell the user to bring a nameless file (fuzzing found exactly this)
                sidecar.embedded = sidecar.uri.empty() || detail::isDataUri(sidecar.uri);
                if (!sidecar.embedded) report.externalBuffers++;
                report.sidecars.push_back(sidecar);
            }
        }

        if (const nlohmann::json* images = readArray("images")) {
            for (const nlohmann::json& image : *images) {
                if (!image.is_object()) continue;
                Sidecar sidecar;
                sidecar.kind = SidecarKind::Image;
                if (image.find("bufferView") != image.end()) {
                    sidecar.fromBufferView = true;
                    report.sidecars.push_back(sidecar);
                    continue;
                }
                const auto uri = image.find("uri");
                if (uri == image.end() || !uri->is_string()) {
                    // neither a bufferView nor a URI: nothing to fetch, and nothing to complain of
                    sidecar.fromBufferView = true;
                    report.sidecars.push_back(sidecar);
                    continue;
                }
                sidecar.uri = uri->get<std::string>();
                sidecar.embedded = sidecar.uri.empty() || detail::isDataUri(sidecar.uri);
                if (!sidecar.embedded) report.externalImages++;
                report.sidecars.push_back(sidecar);
            }
        }
        return report;
    }

    // What an import says of a model it is about to bring in. A .glb is one file and no question.
    // A .gltf with external dependencies is the phone's problem, so the sentence names how many
    // and of what kind: "needs 1 buffer and 3 textures".
    inline std::string sidecarSentence(const SidecarReport& report) {
        if (!report.parsed) return "the model's JSON is malformed: " + report.error;
        if (report.isSelfContained()) return std::string();
        auto part = [](int count, const char* one, const char* many) {
            if (count <= 0) return std::string();
            return std::to_string(count) + " " + (count == 1 ? one : many);
        };
        std::string sentence = "needs ";
        const std::string buffers = part(report.externalBuffers, "buffer file", "buffer files");
        const std::string images = part(report.externalImages, "texture file", "texture files");
        if (!buffers.empty()) sentence += buffers;
        if (!buffers.empty() && !images.empty()) sentence += " and ";
        if (!images.empty()) sentence += images;
        sentence += "; a phone imports one file at a time, so they must be imported too";
        return sentence;
    }

    // A model's extension, deciding which parser reads it: "gltf" is the only one that is text.
    inline bool isAsciiGltf(std::string_view fileName) {
        const size_t dot = fileName.find_last_of('.');
        if (dot == std::string_view::npos) return false;
        std::string_view extension = fileName.substr(dot + 1);
        return extension == "gltf" || extension == "GLTF" || extension == "Gltf";
    }

}

#endif /* ANDROID_MODEL_SIDECARS_H */
