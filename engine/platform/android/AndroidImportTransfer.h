// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

#ifndef ANDROID_IMPORT_TRANSFER_H
#define ANDROID_IMPORT_TRANSFER_H

// A file copied out of a phone's content provider, chunk by chunk, with a name that is only an
// asset once the whole copy landed.
//
// The import path this exists for is not a file read: a SAF `content://` stream can end early (the
// provider dies, the user revokes the grant, the app is backgrounded and the descriptor is closed),
// a 2 GB video arrives in megabytes over a minute, and the user may cancel at any point. Writing
// straight to the asset's name means a half-copied file *is* the asset: the Project panel lists it,
// the importer decodes it, and the failure reads "Error loading texture" rather than "the copy was
// interrupted".
//
// So the copy goes to a temporary name that no scanner picks up, and is renamed onto the final name
// only when the byte count matches. The rules:
//
//   - the temporary lives in the same folder as the target (a rename across volumes is a copy, and
//     on Android a SAF tree and app-private storage are different volumes);
//   - its name ends in `.jeimport` and starts with a dot, so `AssetTypes` never sees it and a
//     folder listing hides it;
//   - a resumed copy reopens the temporary and asks the provider to skip what is already there
//     rather than reading it again (`skip`), which a `content://` stream does by seeking;
//   - everything here is decided from counters the caller reports, so the rules are tested on the
//     host with no provider at all.
//
// Nothing in this file opens a file.

#include <cstdint>
#include <string>

namespace jasperengine::android {

    // The suffix and the hiding dot of a copy in flight. One place, because the folder scanner and
    // the recovery both have to agree with it.
    constexpr const char* IMPORT_TEMP_SUFFIX = ".jeimport";
    constexpr const char* IMPORT_TEMP_PREFIX = ".";

    struct TransferPlan {
        // the name the asset gets once the copy is complete
        std::string target;
        // the name the bytes go to while they are arriving: same folder, hidden, never scanned
        std::string temporary;
        // the size the provider reported (0: it did not say, and then only the stream's end decides)
        uint64_t expectedBytes = 0;
        // bytes already in the temporary, when this is a resumed copy
        uint64_t resumeFrom = 0;
        // a resumed copy whose temporary is already longer than the source: the bytes it holds
        // cannot be a prefix of this source, so it has to start over
        bool discardPartial = false;
    };

    // Why a copy ended.
    enum class TransferOutcome : uint8_t {
        // every byte arrived (or the stream ended where the provider said it would): rename
        Complete = 0,
        // more bytes than the provider promised, or fewer than the temporary already held
        Mismatched,
        // the stream ended before the promised size: keep the temporary, it can be resumed
        Interrupted,
        // the user or the system cancelled: the caller decides whether to keep the partial
        Cancelled,
        // the volume filled: resuming needs space, so the caller should free some first
        OutOfSpace,
    };

    struct TransferStep {
        // bytes to copy from the stream in this step (0: nothing left to read)
        uint64_t copyBytes = 0;
        // bytes to ask the provider to skip before reading, when resuming
        uint64_t skipBytes = 0;
        TransferOutcome outcome = TransferOutcome::Complete;
        // a sentence for the user, "" when Complete
        std::string message;
    };

    // Whether a folder entry is a copy that never finished: the import folder's recovery at start
    // lists the temporaries and either resumes or drops them. Anything else is an asset.
    inline bool isImportTemporary(const std::string& fileName) {
        const size_t slash = fileName.find_last_of('/');
        const std::string leaf = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
        const std::string suffix = IMPORT_TEMP_SUFFIX;
        const std::string prefix = IMPORT_TEMP_PREFIX;
        return leaf.size() > prefix.size() + suffix.size() && leaf.compare(0, prefix.size(), prefix) == 0 &&
               leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    // The asset name a temporary stands for: ".<name>.jeimport" -> "<name>". Empty for anything
    // that is not a temporary, so a caller may pass every entry of a folder.
    inline std::string temporaryTarget(const std::string& fileName) {
        if (!isImportTemporary(fileName)) return std::string();
        const size_t slash = fileName.find_last_of('/');
        const std::string folder = slash == std::string::npos ? std::string() : fileName.substr(0, slash + 1);
        const std::string leaf = slash == std::string::npos ? fileName : fileName.substr(slash + 1);
        const std::string suffix = IMPORT_TEMP_SUFFIX;
        const size_t start = IMPORT_TEMP_PREFIX[0] == '\0' ? 0 : 1;
        return folder + leaf.substr(start, leaf.size() - start - suffix.size());
    }

    // The plan for copying `displayName`'s bytes into `target`, given what the provider said the
    // size is and whether a temporary from an earlier attempt is still there.
    inline TransferPlan planTransfer(const std::string& wanted, uint64_t expectedBytes, uint64_t existingTemporaryBytes) {
        TransferPlan plan;
        // a caller that hands over a name that is already a temporary (a retry of a retry) must not
        // get ".<name>.jeimport.jeimport", which isImportTemporary() would no longer recognise and
        // which would leave a file the folder scan can never recover
        plan.target = isImportTemporary(wanted) ? temporaryTarget(wanted) : wanted;
        const std::string& target = plan.target;
        // the temporary is the target's own name with the suffix, hidden: a folder scan and the
        // user's file manager both pass over it, and the rename is within one directory
        const size_t slash = target.find_last_of('/');
        const std::string folder = slash == std::string::npos ? std::string() : target.substr(0, slash + 1);
        const std::string leaf = slash == std::string::npos ? target : target.substr(slash + 1);
        // a name with no leaf ("dir/", "") cannot be an asset, and "..jeimport" is a temporary the
        // folder scan would never recognise: the plan names a placeholder so that whatever is
        // written is always recoverable (found by fuzzing). The target is the placeholder too, so
        // that temporaryTarget() of the temporary is exactly the target the plan reports.
        const std::string recoverable = leaf.empty() ? std::string("import") : leaf;
        plan.target = folder + recoverable;
        plan.temporary = folder + IMPORT_TEMP_PREFIX + recoverable + IMPORT_TEMP_SUFFIX;
        plan.expectedBytes = expectedBytes;
        if (existingTemporaryBytes == 0) return plan;
        // a partial that is already the whole file (a crash between the last write and the rename)
        // needs no copy at all: the caller renames it
        if (expectedBytes > 0 && existingTemporaryBytes == expectedBytes) {
            plan.resumeFrom = existingTemporaryBytes;
            return plan;
        }
        if (expectedBytes > 0 && existingTemporaryBytes > expectedBytes) {
            // the provider changed its mind, or a different file wrote the same temporary: the
            // bytes cannot be a prefix of this one
            plan.discardPartial = true;
            plan.resumeFrom = 0;
            return plan;
        }
        plan.resumeFrom = existingTemporaryBytes;
        return plan;
    }

    // Whether this step may start: `spaceAvailable` bytes free on the target's volume, `reserve`
    // the bytes a save elsewhere must still find (AndroidStorageSpace's policy).
    //
    // The room needed is the *remaining* bytes, because the ones already copied are on disk.
    inline TransferStep transferStep(const TransferPlan& plan, uint64_t copiedBytes, uint64_t spaceAvailable,
                                     uint64_t reserve, uint64_t chunkBytes) {
        TransferStep step;
        if (plan.discardPartial) {
            // start over, and say so: a user who waited is owed the reason
            step.skipBytes = 0;
            step.copyBytes = chunkBytes;
            step.outcome = TransferOutcome::Interrupted;
            step.message = "the copy starts over: the file it was making is not this one";
            return step;
        }
        // the source is where the copy resumes: its bytes before `resumeFrom` are already on disk
        step.skipBytes = plan.resumeFrom;
        const uint64_t total = plan.expectedBytes;
        if (total > 0) {
            if (copiedBytes > total) {
                step.outcome = TransferOutcome::Mismatched;
                step.copyBytes = 0;
                step.message = "the file is longer than the size it reported; it was not imported";
                return step;
            }
            const uint64_t remaining = total - copiedBytes;
            step.copyBytes = remaining < chunkBytes ? remaining : chunkBytes;
            if (remaining == 0) {
                step.outcome = TransferOutcome::Complete;
                return step;
            }
            // the room the rest of the copy needs, plus what the rest of the app must keep
            const uint64_t needed = remaining > UINT64_MAX - reserve ? UINT64_MAX : remaining + reserve;
            if (spaceAvailable < needed) {
                step.outcome = TransferOutcome::OutOfSpace;
                step.copyBytes = 0;
                step.message = "not enough space to finish the copy: free some space and try again, " +
                               std::to_string(copiedBytes / (1u << 20)) + " MB are already there";
                return step;
            }
            step.outcome = TransferOutcome::Complete;   // "this step is fine"
            return step;
        }
        // no size from the provider: read until the stream ends, so every step is fine and the
        // stream's own end is the only completion signal
        step.copyBytes = chunkBytes;
        return step;
    }

    // What to do with a copy that stopped. `bytesNow` is the temporary's size after the last write.
    inline TransferOutcome settleTransfer(const TransferPlan& plan, uint64_t bytesNow, bool cancelled,
                                          bool streamEnded) {
        if (cancelled) return TransferOutcome::Cancelled;
        if (plan.expectedBytes > 0) {
            if (bytesNow == plan.expectedBytes) return TransferOutcome::Complete;
            if (bytesNow > plan.expectedBytes) return TransferOutcome::Mismatched;
            // the stream ended before the size it promised: the provider lied or died
            if (streamEnded) return TransferOutcome::Interrupted;
            return TransferOutcome::Interrupted;
        }
        // no promised size: the stream's end is the completion
        return streamEnded ? TransferOutcome::Complete : TransferOutcome::Interrupted;
    }

}

#endif /* ANDROID_IMPORT_TRANSFER_H */
