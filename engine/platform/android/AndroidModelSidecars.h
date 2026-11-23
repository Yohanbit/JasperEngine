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

    // One sidecar's landing place in the import folder.
    struct SidecarImport {
        std::string uri;          // the URI the .gltf wrote ("textures/skin.png")
        std::string assetName;    // the name it lands under, flat ("body_skin.png")
        bool renamed = false;     // the provider/import gave it another name (a collision, a bad char)
    };

    struct ModelImportPlan {
        std::vector<SidecarImport> sidecars;    // one per external file, in the glTF's order
        // the .gltf's text with every external URI replaced by its sidecar's assetName: a flat
        // folder needs a flat document, or the model keeps pointing at folders that are not there
        std::string gltfText;
        // false when the document did not parse (gltfText is then the input, unchanged)
        bool parsed = false;
    };

    namespace detail {

        // The identifier an imported model's sidecar is named after: "<model>_<leaf stem>.<ext>".
        // The model's own name is the prefix because two models routinely ship a "skin.png" or a
        // "common.png" in different folders, and a flat import folder cannot hold both.
        inline std::string sidecarStemOf(const std::string& modelLeaf, const std::string& uri) {
            const std::string leaf = detail::sidecarFileName(uri);
            const size_t dot = leaf.find_last_of('.');
            const std::string stem = dot == std::string::npos ? leaf : leaf.substr(0, dot);
            std::string out;
            // lowercase ASCII and '_' only: the same rule importedAssetName applies to a model's
            // own file name, so the pair reads alike in the Project panel
            auto push = [&out](const std::string& text) {
                bool lastUnderscore = false;
                for (char c : text) {
                    const char lowered = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
                    const bool keep = (lowered >= 'a' && lowered <= 'z') || (lowered >= '0' && lowered <= '9');
                    if (keep) {
                        out.push_back(lowered);
                        lastUnderscore = false;
                    } else if (!out.empty() && !lastUnderscore) {
                        out.push_back('_');
                        lastUnderscore = true;
                    }
                }
            };
            push(modelLeaf);
            out.push_back('_');
            push(stem);
            while (!out.empty() && out.back() == '_') out.pop_back();
            if (out.empty()) out = "asset";
            if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
            return out;
        }

        inline std::string extensionWithDotOf(const std::string& uri) {
            const std::string leaf = detail::sidecarFileName(uri);
            const size_t dot = leaf.find_last_of('.');
            if (dot == std::string::npos) return std::string();
            // lowercased: Android's volumes are case-sensitive and assetTypeForImport() compares
            // lowercase, so "skin.PNG" imported as-is was a file the type table could not name
            std::string extension = leaf.substr(dot);
            for (char& c : extension) {
                if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            }
            return extension;
        }
    }

    // How a .gltf and its files land in one flat import folder.
    //
    // A phone's import has no directories to spare: every file arrives separately and lands in one
    // folder, so a model's "textures/skin.png" becomes "body_skin.png" -- and the document has to
    // say so, or it keeps naming a relative path that no longer exists. This rewrites the URIs and
    // gives every sidecar a name derived from its model's, which is what keeps "Body/textures/
    // common.png" and "Head/textures/common.png" two different assets instead of one overwriting
    // the other.
    inline ModelImportPlan planModelImport(const std::string& gltfText, const std::string& modelFileName) {
        ModelImportPlan plan;
        const std::string modelLeaf = detail::sidecarFileName(modelFileName);
        const SidecarReport report = gltfSidecars(gltfText);
        if (!report.parsed) return plan;
        // the model's stem, without its .gltf
        std::string modelStem = modelLeaf;
        const size_t dot = modelStem.find_last_of('.');
        if (dot != std::string::npos) modelStem = modelStem.substr(0, dot);

        // each distinct URI gets one name, assigned in the document's order so a run is repeatable
        std::map<std::string, std::string> assigned;
        std::set<std::string> used;
        for (const Sidecar& sidecar : report.sidecars) {
            if (sidecar.embedded || sidecar.fromBufferView) continue;
            if (assigned.count(sidecar.uri)) continue;   // the same file named twice is one asset
            const std::string name = detail::sidecarStemOf(modelStem, sidecar.uri) + detail::extensionWithDotOf(sidecar.uri);
            std::string unique = name;
            int counter = 1;
            while (!used.insert(unique).second) {
                // two URIs with the same leaf ("a/skin.png", "b/skin.png"): numbered, as the
                // editor's own uniqueFileName does
                const size_t ext = name.find_last_of('.');
                const std::string stem = ext == std::string::npos ? name : name.substr(0, ext);
                const std::string extension = ext == std::string::npos ? std::string() : name.substr(ext);
                unique = stem + "_" + std::to_string(counter++) + extension;
            }
            assigned[sidecar.uri] = unique;
        }
        for (const auto& [uri, name] : assigned) {
            SidecarImport entry;
            entry.uri = uri;
            entry.assetName = name;
            plan.sidecars.push_back(entry);
        }

        // rewrite the document: the same parse, with each "uri" the plan renamed replaced. Done on
        // the JSON rather than on the bytes so a URI that happens to appear in a name or a comment
        // is not touched.
        nlohmann::json document;
        try {
            document = nlohmann::json::parse(gltfText.begin(), gltfText.end());
        } catch (const std::exception&) {
            return plan;
        }
        for (const char* key : {"buffers", "images"}) {
            const auto it = document.find(key);
            if (it == document.end() || !it->is_array()) continue;
            for (nlohmann::json& entry : *it) {
                if (!entry.is_object()) continue;
                const auto uri = entry.find("uri");
                if (uri == entry.end() || !uri->is_string()) continue;
                const std::string text = uri->get<std::string>();
                const auto found = assigned.find(text);
                if (found != assigned.end()) *uri = found->second;
            }
        }
        plan.gltfText = document.dump();
        plan.parsed = true;
        return plan;
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
