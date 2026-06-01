#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include <cwchar>

namespace patch_downloader {

struct Progress {
    std::string message;
    uint64_t filesDone = 0;
    uint64_t filesTotal = 0;
    uint64_t chunksDone = 0;
    uint64_t chunksTotal = 0;
};

struct Options {
    std::string manifestUrl;
    std::wstring outputDirectory;
    std::string language = "en_US";
    int threads = 4;
    std::atomic_bool* cancelRequested = nullptr;
    std::function<void(const Progress&)> onProgress;
};

struct Result {
    bool success = false;
    bool cancelled = false;
    std::string message;
};

bool CoreAvailable();
std::string CoreUnavailableReason();
Result DownloadPatch(const Options& options);
bool ShouldRetryDownloadForTest(unsigned long statusCode, const std::string& error,
                                int attempt, int maxAttempts);
bool RetryFailureWindowExceededForTest(int consecutiveFailures, long long windowStartMs,
                                       long long currentMs);
bool ShouldRetryChunkPayloadFailureForTest(const std::string& error, int consecutiveFailures,
                                           long long windowStartMs, long long currentMs);
bool ManifestBytesLookSafeForTest(const std::vector<unsigned char>& bytes);
std::string DefaultPatchLanguageForTest();
bool ShouldIncludeFileForSelectedLanguageForTest(const std::vector<std::string>& fileLanguages,
                                                 const std::string& selectedLanguage);
std::wstring OutputRelativePathForTest(const char* fileName, const char* fileLink);
std::wstring ParsedManifestOutputPathForTest(const std::vector<unsigned char>& bytes,
                                             const char* fileName);

}
