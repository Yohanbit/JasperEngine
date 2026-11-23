// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_ASSET_IMPORT_H
#define ANDROID_ASSET_IMPORT_H

// Bringing a file into a project on Android (Storage Access Framework, "Open" / "Share with"),
// without Android headers, so the core tests run it on any host (as AndroidTextInput.h does).
//
// A file picked on a phone does not arrive as a path: it arrives as a content:// URI whose last
// segment is often an opaque id ("content://com.android.providers.media.documents/document/
// image%3A1000000034"), with a percent-encoded display name, a name with characters the engine's
// path rules refuse (':' and '..' are refused by FileUtils::resolve), a name that already exists
// in the target folder, and sometimes no extension at all (cloud providers hand out
// "document/1234"). So an import is four steps, each here:
//
//  1. uriFileName()      the file's name out of the URI (percent-decoded, no query or fragment)
//  2. sanitizeFileName() that name as a name the engine's path rules accept, keeping the extension
//  3. uniqueFileName()   a name free in the target folder (Unity's "sprite (1).png")
//  4. assetTypeForImport() what the file is, from its bytes first and its extension second
//
// The canonical table of extensions per asset type is the editor's (editor/asset/AssetTypes.cpp);
// this is the runtime's own reading of what a phone hands over, used before the editor exists in
// an Android build and by a player that imports into its data:// folder.

#include "asset/AssetType.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>

namespace jasperengine::android {

    // ---- URIs

    // The decoded bytes of a percent-encoded run ("%3A" -> ':', "%2F" -> '/'); a '%' that does not
    // start two hex digits stays as it is, and '+' stays '+' (a path, never a form's space). The
    // bytes come back as they are: sanitizeFileName() is what validates the UTF-8 a name arrives in.
    inline void percentDecode(std::string_view text, std::string& decoded) {
        decoded.clear();
        decoded.reserve(text.size());
        auto hex = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '%' && i + 2 < text.size()) {
                const int high = hex(text[i + 1]);
                const int low = hex(text[i + 2]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            decoded.push_back(text[i]);
        }
    }

    // The URI without its "?query" or "#fragment": a SAF grant puts the document's name before
    // either, and only there.
    inline std::string_view uriWithoutQuery(std::string_view uri) {
        const size_t cut = uri.find_first_of("?#");
        return cut == std::string_view::npos ? uri : uri.substr(0, cut);
    }

    // The file's name in a content:// or file:// URI, percent-decoded: the segment after the last
    // '/'. "" when the URI has no segment with anything in it (a folder grant, "" ). A ':' in the
    // segment decodes to ':' here and sanitizeFileName() is what refuses it.
    inline std::string uriFileName(std::string_view uri) {
        const std::string_view path = uriWithoutQuery(uri);
        const size_t slash = path.find_last_of('/');
        std::string_view segment = slash == std::string_view::npos ? path : path.substr(slash + 1);
        while (!segment.empty() && segment.front() == '/') segment.remove_prefix(1);
        std::string decoded;
        percentDecode(segment, decoded);
        return decoded;
    }

    // A URI that names a document a provider can be asked to open (either scheme; the opener
    // checks the grant itself). "content:/x" and "" are not.
    inline bool isDocumentUri(std::string_view uri) {
        return uri.rfind("content://", 0) == 0 || uri.rfind("file://", 0) == 0;
    }

    // ---- Names

    struct SanitizeOptions {
        // Bytes of the whole name, extension included: Android's own limit on a file name is 255
        // bytes, and this leaves room for uniqueFileName()'s " (12)" suffix
        size_t maxBytes = 96;
        // A name whose extension is cut off is useless (an importer reads it), so the extension
        // keeps its bytes and the stem is what shortens
        bool keepExtension = true;
    };

    // `wanted` as a file name that FileUtils::resolve() accepts and every file system here stores:
    // path separators and the reserved characters of the platforms the project may be copied to
    // ('/', '\\', ':', '*', '?', '"', '<', '>', '|') become '_' (a ':' is what a SAF id carries,
    // and resolve() refuses it), control bytes and unpaired surrogates are dropped, "." and ".."
    // are refused as a name, leading and trailing blanks and dots go (Windows drops a trailing dot
    // itself, so a name that kept one would not be the name on disk), and the result is cut to
    // maxBytes without splitting a UTF-8 sequence. Unicode is kept: a phone's photos are named in
    // the user's own script. "file" when nothing is left.
    inline std::string sanitizeFileName(std::string_view wanted, const SanitizeOptions& options = {}) {
        std::string name;
        name.reserve(wanted.size());

        // UTF-8 decoded by hand so a cut never splits a sequence and control characters
        // (including a NUL a provider may send) never reach a path
        size_t i = 0;
        while (i < wanted.size()) {
            const unsigned char lead = static_cast<unsigned char>(wanted[i]);
            size_t length = 1;
            uint32_t codepoint = lead;
            if (lead >= 0xC2 && lead <= 0xDF) {
                length = 2;
                codepoint = lead & 0x1Fu;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                length = 3;
                codepoint = lead & 0x0Fu;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                length = 4;
                codepoint = lead & 0x07u;
            } else if (lead >= 0x80) {
                // a stray continuation byte, or a lead that starts nothing valid: dropped
                i++;
                continue;
            }
            if (length > 1) {
                if (i + length > wanted.size()) break;
                bool valid = true;
                for (size_t k = 1; k < length; k++) {
                    const unsigned char next = static_cast<unsigned char>(wanted[i + k]);
                    if ((next & 0xC0) != 0x80) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 6) | (next & 0x3Fu);
                }
                if (!valid) {
                    i++;
                    continue;
                }
            }
            i += length;

            const bool control = codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F);
            const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
            const bool overlong = (length == 3 && codepoint < 0x800) || (length == 4 && codepoint < 0x10000);
            if (control || surrogate || overlong || codepoint > 0x10FFFF) continue;

            const bool reserved = codepoint == '/' || codepoint == '\\' || codepoint == ':' || codepoint == '*' ||
                                  codepoint == '?' || codepoint == '"' || codepoint == '<' || codepoint == '>' ||
                                  codepoint == '|';
            if (reserved) {
                name.push_back('_');
                continue;
            }
            name.append(wanted.substr(i - length, length));
        }

        auto trimEnds = [](std::string& text) {
            while (!text.empty() && (text.front() == ' ' || text.front() == '.' || text.front() == '\t')) text.erase(text.begin());
            while (!text.empty() && (text.back() == ' ' || text.back() == '.' || text.back() == '\t')) text.pop_back();
        };
        trimEnds(name);
        if (name.empty() || name == "." || name == "..") return "file";

        const size_t dot = name.find_last_of('.');
        const bool hasExtension = options.keepExtension && dot != std::string::npos && dot > 0 && dot + 1 < name.size();
        size_t extensionBytes = hasExtension ? name.size() - dot : 0;
        if (name.size() > options.maxBytes) {
            const size_t stemLimit = options.maxBytes > extensionBytes ? options.maxBytes - extensionBytes : 0;
            size_t cut = stemLimit;
            // not inside a UTF-8 sequence: a continuation byte (10xxxxxx) is stepped back over
            while (cut > 0 && cut < name.size() && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) cut--;
            name = name.substr(0, cut) + (hasExtension ? name.substr(dot) : std::string());
            trimEnds(name);
            if (name.empty() || name == "." || name == "..") return "file";
        }
        return name;
    }

    // `wanted` sanitized, and then the first name free according to `isTaken`: "sprite.png",
    // "sprite (1).png", "sprite (2).png"... (Unity's rule in the Project window, so a file
    // imported twice reads the same there and on a phone). `isTaken` is a predicate rather than a
    // container so a caller can ask the folder itself (FileUtils::exists) or an index.
    inline std::string uniqueFileName(std::string_view wanted, const std::function<bool(const std::string&)>& isTaken,
                                      const SanitizeOptions& options = {}) {
        const std::string base = sanitizeFileName(wanted, options);
        if (!isTaken || !isTaken(base)) return base;

        const size_t dot = base.find_last_of('.');
        const bool hasExtension = dot != std::string::npos && dot > 0 && dot + 1 < base.size();
        const std::string stem = hasExtension ? base.substr(0, dot) : base;
        const std::string extension = hasExtension ? base.substr(dot) : std::string();
        // past this a folder holds a thousand copies of one name already: give up asking rather
        // than spin, and hand back the last name tried (the caller's write then says why)
        constexpr int MAX_TRIES = 1000;
        std::string candidate;
        for (int n = 1; n <= MAX_TRIES; n++) {
            candidate = stem + " (" + std::to_string(n) + ")" + extension;
            if (!isTaken(candidate)) return candidate;
        }
        return candidate;
    }

    // ---- What the file is

    // The container a picked file is in, read from its first bytes. Anything not here (a .json, a
    // .lua, a text file) is Unknown and the extension decides.
    enum class ImportKind : uint8_t {
        Unknown = 0,
        Png,      // 89 50 4E 47
        Jpeg,     // FF D8 FF
        Gif,      // "GIF8"
        Bmp,      // "BM"
        Webp,     // "RIFF" .... "WEBP"
        Tga,      // no magic at all (an extension decides; a footer is not read here)
        Psd,      // "8BPS"
        Hdr,      // "#?RADIANCE"
        OpenExr,  // 76 2F 31 01
        Ktx2,     // AB 4B 54 58 20 32 30
        Glb,      // "glTF"
        Gltf,     // '{"asset"' JSON, read as text by the importer
        Obj,      // "mtllib "/"# " text
        Fbx,      // "Kaydara FBX Binary"
        Wav,      // "RIFF" .... "WAVE"
        Ogg,      // "OggS"
        Mp3,      // "ID3" or an MPEG frame sync
        Flac,     // "fLaC"
        Ttf,      // 00 01 00 00 or "true"
        Otf,      // "OTTO"
        Aseprite, // 20 A5 E0 A1 (the header's magic, little endian as stored)
    };

    namespace detail {
        inline bool startsWithBytes(std::span<const uint8_t> bytes, std::initializer_list<uint8_t> prefix) {
            if (bytes.size() < prefix.size()) return false;
            size_t i = 0;
            for (uint8_t value : prefix) {
                if (bytes[i++] != value) return false;
            }
            return true;
        }

        inline bool startsWithText(std::span<const uint8_t> bytes, std::string_view prefix) {
            if (bytes.size() < prefix.size()) return false;
            for (size_t i = 0; i < prefix.size(); i++) {
                if (bytes[i] != static_cast<uint8_t>(prefix[i])) return false;
            }
            return true;
        }

        // "RIFF" <size> "WAVE" / "WEBP": the form type sits 8 bytes in, so a span must cover it
        // (a span holds empty bytes: the size field's NULs are part of the data)
        inline bool riffForm(std::span<const uint8_t> bytes, std::string_view form) {
            return bytes.size() >= 12 && startsWithText(bytes, "RIFF") && startsWithText(bytes.subspan(8), form);
        }
    }

    inline ImportKind kindFromMagic(std::span<const uint8_t> bytes) {
        using detail::startsWithBytes;
        using detail::startsWithText;
        if (bytes.size() >= 8 && startsWithBytes(bytes, {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A})) return ImportKind::Png;
        if (startsWithBytes(bytes, {0xFF, 0xD8, 0xFF})) return ImportKind::Jpeg;
        if (startsWithText(bytes, "GIF87a") || startsWithText(bytes, "GIF89a")) return ImportKind::Gif;
        if (detail::riffForm(bytes, "WEBP")) return ImportKind::Webp;
        if (detail::riffForm(bytes, "WAVE")) return ImportKind::Wav;
        if (startsWithText(bytes, "8BPS")) return ImportKind::Psd;
        if (startsWithText(bytes, "#?RADIANCE")) return ImportKind::Hdr;
        if (startsWithBytes(bytes, {0x76, 0x2F, 0x31, 0x01})) return ImportKind::OpenExr;
        if (startsWithBytes(bytes, {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30})) return ImportKind::Ktx2;
        if (startsWithText(bytes, "glTF")) return ImportKind::Glb;
        if (startsWithText(bytes, "Kaydara FBX Binary")) return ImportKind::Fbx;
        if (startsWithText(bytes, "OggS")) return ImportKind::Ogg;
        if (startsWithText(bytes, "fLaC")) return ImportKind::Flac;
        if (startsWithBytes(bytes, {0x20, 0xA5, 0xE0, 0xA1})) return ImportKind::Aseprite;
        if (startsWithBytes(bytes, {0x00, 0x01, 0x00, 0x00}) || startsWithText(bytes, "true")) return ImportKind::Ttf;
        if (startsWithText(bytes, "OTTO")) return ImportKind::Otf;
        // "BM" is two ASCII letters and short: last, so nothing above is read as a BMP
        if (bytes.size() >= 14 && startsWithText(bytes, "BM")) return ImportKind::Bmp;
        if (startsWithText(bytes, "ID3")) return ImportKind::Mp3;
        if (startsWithBytes(bytes, {0xFF, 0xFB}) || startsWithBytes(bytes, {0xFF, 0xF3}) ||
            startsWithBytes(bytes, {0xFF, 0xF2}) || startsWithBytes(bytes, {0xFF, 0xFA})) return ImportKind::Mp3;
        return ImportKind::Unknown;
    }

    // The extension of `fileName` lowercased and without its dot ("" when it has none)
    inline std::string extensionOf(std::string_view fileName) {
        const size_t slash = fileName.find_last_of("/\\");
        const size_t dot = fileName.find_last_of('.');
        const size_t nameStart = slash == std::string_view::npos ? 0 : slash + 1;
        // a dot with nothing before it names a hidden file (".bashrc"), not an extension
        if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash) ||
            dot <= nameStart || dot + 1 >= fileName.size()) {
            return std::string();
        }
        std::string extension(fileName.substr(dot + 1));
        for (char& c : extension) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        return extension;
    }

    // What asset type an imported file becomes: its bytes first (a provider that hands over
    // "document/1234" carries no extension, and a renamed file lies), its extension second. The
    // editor's database (editor/asset/AssetTypes.cpp) is the canonical extension table; this one
    // covers what a picker hands over and is only consulted when the bytes are Unknown.
    inline jasperengine::AssetType assetTypeForImport(std::string_view fileName, std::span<const uint8_t> bytes) {
        using jasperengine::AssetType;
        switch (kindFromMagic(bytes)) {
            case ImportKind::Png:
            case ImportKind::Jpeg:
            case ImportKind::Gif:
            case ImportKind::Bmp:
            case ImportKind::Webp:
            case ImportKind::Psd:
            case ImportKind::Hdr:
            case ImportKind::OpenExr:
            case ImportKind::Ktx2:
                return AssetType::Texture;
            case ImportKind::Glb:
            case ImportKind::Fbx:
                return AssetType::Model;
            case ImportKind::Wav:
            case ImportKind::Ogg:
            case ImportKind::Mp3:
            case ImportKind::Flac:
                return AssetType::Audio;
            case ImportKind::Ttf:
            case ImportKind::Otf:
                return AssetType::Font;
            case ImportKind::Aseprite:
                return AssetType::AsepriteSprite;
            case ImportKind::Unknown:
            case ImportKind::Tga:
            case ImportKind::Gltf:
            case ImportKind::Obj:
                break;
        }

        const std::string extension = extensionOf(fileName);
        if (extension == "png" || extension == "jpg" || extension == "jpeg" || extension == "bmp" ||
            extension == "tga" || extension == "gif" || extension == "hdr" || extension == "exr" ||
            extension == "psd" || extension == "ktx2") {
            return AssetType::Texture;
        }
        if (extension == "gltf" || extension == "glb" || extension == "obj" || extension == "fbx") {
            return AssetType::Model;
        }
        if (extension == "wav" || extension == "ogg" || extension == "mp3" || extension == "flac") {
            return AssetType::Audio;
        }
        if (extension == "ttf" || extension == "otf" || extension == "ttc") return AssetType::Font;
        if (extension == "aseprite" || extension == "ase") return AssetType::AsepriteSprite;
        if (extension == "html" || extension == "htm") return AssetType::UIDocument;
        if (extension == "css" || extension == "rcss") return AssetType::Stylesheet;
        if (extension == "lua") return AssetType::Script;
        if (extension == "json" || extension == "txt" || extension == "csv" || extension == "xml") {
            return AssetType::Data;
        }
        return AssetType::Unknown;
    }

    namespace detail {
        // A stem the engine may derive a Lua/C++ symbol or a scene name from: lowercased, every
        // run of anything that is not a-z, 0-9 or '_' is one '_', no leading or trailing '_'.
        // Non-ASCII is dropped by that rule on purpose: a symbol is spelled in ASCII on every
        // toolchain (an Android project file name may still keep its own script).
        inline std::string identifierStemOf(std::string_view fileName) {
            const size_t dot = fileName.find_last_of('.');
            std::string_view stem = dot == std::string_view::npos ? fileName : fileName.substr(0, dot);
            std::string out;
            out.reserve(stem.size());
            bool lastUnderscore = false;
            for (char c : stem) {
                char normalized = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                const bool keep = (normalized >= 'a' && normalized <= 'z') || (normalized >= '0' && normalized <= '9') || normalized == '_';
                if (keep) {
                    if (normalized == '_' && (out.empty() || lastUnderscore)) {
                        lastUnderscore = true;
                        continue;
                    }
                    out.push_back(normalized);
                    lastUnderscore = normalized == '_';
                } else if (!out.empty() && !lastUnderscore) {
                    out.push_back('_');
                    lastUnderscore = true;
                }
            }
            while (!out.empty() && out.back() == '_') out.pop_back();
            if (out.empty()) return "asset";
            if (out.front() >= '0' && out.front() <= '9') out.insert(out.begin(), '_');
            return out;
        }
    }

    // The extension an extension-less audio import gets: the container the bytes are in (the
    // base's own choice, for an .ogg song, is not a .wav)
    inline const char* audioExtensionFor(ImportKind kind) {
        switch (kind) {
            case ImportKind::Ogg: return "ogg";
            case ImportKind::Mp3: return "mp3";
            case ImportKind::Flac: return "flac";
            default: return "wav";
        }
    }

    // The name an imported file gets as an asset of the engine: a lowercase identifier
    // (identifierStemOf) plus the extension the type needs, so a phone import lands under a name
    // the editor, Lua and the generators already accept. A Texture keeps its own extension (the
    // importer reads it); anything with no extension gets the type's usual one when it is known
    // ("photo" from a cloud provider -> "photo.png").
    inline std::string importedAssetName(std::string_view fileName, std::span<const uint8_t> bytes, const SanitizeOptions& options = {}) {
        using jasperengine::AssetType;
        const AssetType type = assetTypeForImport(fileName, bytes);
        std::string extension = extensionOf(fileName);
        if (extension.empty()) {
            switch (type) {
                case AssetType::Texture: extension = "png"; break;
                case AssetType::Audio: extension = audioExtensionFor(kindFromMagic(bytes)); break;
                case AssetType::Font: extension = "ttf"; break;
                case AssetType::Model: extension = "glb"; break;
                default: break;
            }
        }
        const std::string stem = detail::identifierStemOf(fileName);
        const std::string wanted = extension.empty() ? stem : stem + "." + extension;
        // the extension is known and wanted as it is: a cut keeps it, nothing is added
        return sanitizeFileName(wanted, options);
    }

}

#endif // ANDROID_ASSET_IMPORT_H
