// The import pipeline as ANDROID_INTEGRATIONS.md gives it: the bytes a SAF picker returns go
// through preflight, then through the transfer plan, then land under the name the engine scans.
#include "AndroidImportPreflight.h"
#include "AndroidImportTransfer.h"
#include <cstdio>
#include <vector>

using namespace jasperengine;
using namespace jasperengine::android;

// a 4x4 PNG header and enough of an IHDR for readImageSize to answer
static std::vector<uint8_t> tinyPng(int width, int height) {
    std::vector<uint8_t> bytes = {0x89, 'P', 'N', 'G', 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
    const uint8_t dims[8] = {(uint8_t)(width >> 24), (uint8_t)(width >> 16), (uint8_t)(width >> 8), (uint8_t)width,
                             (uint8_t)(height >> 24), (uint8_t)(height >> 16), (uint8_t)(height >> 8), (uint8_t)height};
    bytes.insert(bytes.end(), dims, dims + 8);
    bytes.insert(bytes.end(), {8, 6, 0, 0, 0});
    return bytes;
}

int main() {
    // 1. the picker hands over three files: a photo, a phone screenshot, and a name Windows cannot take
    std::vector<PreflightInput> picked;
    picked.push_back({"DCIM/Camera/IMG_20240101_120000.jpg", {}, 4u << 20, {}});
    picked.push_back({"Screenshot_2024-01-01-12-00-00.png", tinyPng(1080, 2400), 800u << 10, {}});
    picked.push_back({"CON.png", tinyPng(64, 64), 4u << 10, {}});

    PreflightOptions options;
    options.androidBitmapFactory = true;
    options.androidMediaCodec = true;
    const PreflightReport report = preflightImports(picked, options);

    std::printf("preflight: %zu files, largest decode %.1f MB, resident %.1f MB\n",
                report.findings.size(), report.largestDecodedBytes / 1048576.0,
                report.residentTextureBytes / 1048576.0);
    for (const PreflightFinding& finding : report.findings) {
        std::printf("  %-40s -> %-28s %s\n", finding.fileName.c_str(), finding.assetName.c_str(),
                    finding.message.c_str());
    }

    // 2. the copy: a plan per file, then the step the loop would act on
    for (const PreflightFinding& finding : report.findings) {
        const TransferPlan plan = planTransfer(finding.assetName, 4u << 20, 0);
        const TransferStep step = transferStep(plan, 0, 900u << 20, 8u << 20, 64u << 10);
        std::printf("transfer: %-28s -> %-28s step: copy %llu, skip %llu, %s\n",
                    plan.target.c_str(), plan.temporary.c_str(),
                    (unsigned long long)step.copyBytes, (unsigned long long)step.skipBytes,
                    step.message.empty() ? "complete" : step.message.c_str());
    }

    // 3. the volume fills at 3 MB of the 4 MB
    const TransferPlan plan = planTransfer("IMG_20240101_120000.jpg", 4u << 20, 0);
    const TransferStep stuck = transferStep(plan, 3u << 20, 100, 8u << 20, 64u << 10);
    std::printf("out of space: outcome=%s \"%s\"\n",
                stuck.outcome == TransferOutcome::OutOfSpace ? "OutOfSpace" : "other", stuck.message.c_str());
    return 0;
}
