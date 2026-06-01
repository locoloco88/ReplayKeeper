#include "patch_downloader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>
#include <windows.h>
#include <winhttp.h>

#ifndef REPLAYKEEPER_PATCH_DOWNLOAD_CORE
#define REPLAYKEEPER_PATCH_DOWNLOAD_CORE 0
#endif

#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
extern "C" {
typedef struct {
    uint64_t length;
    uint8_t* data;
} BinaryData;

typedef enum {
    HASHTYPE_SHA512 = 1,
    HASHTYPE_SHA256,
    HASHTYPE_HKDF,
    HASHTYPE_BLAKE3,
} HashType;

typedef struct chunk {
    uint32_t compressed_size;
    uint32_t uncompressed_size;
    uint64_t chunk_id;
    uint64_t bundle_id;
    uint32_t bundle_offset;
    uint64_t file_offset;
    HashType hashType;
} Chunk;

typedef struct {
    uint32_t length;
    uint32_t allocated_length;
    Chunk* objects;
} ChunkList;

typedef struct bundle {
    uint64_t bundle_id;
    ChunkList chunks;
} Bundle;

typedef struct {
    uint32_t length;
    uint32_t allocated_length;
    Bundle* objects;
} BundleList;

typedef struct language {
    uint8_t language_id;
    char* name;
} Language;

typedef struct {
    uint32_t length;
    uint32_t allocated_length;
    Language* objects;
} LanguageList;

typedef struct file {
    char* name;
    char* link;
    LanguageList languages;
    uint64_t file_size;
    ChunkList chunks;
} File;

typedef struct {
    uint32_t length;
    uint32_t allocated_length;
    File* objects;
} FileList;

typedef struct parameters {
    HashType hashType;
    uint32_t min_chunk_size;
    uint32_t max_chunk_size;
    uint32_t max_uncompressed_size;
} Parameters;

typedef struct {
    uint32_t length;
    uint32_t allocated_length;
    Parameters* objects;
} ParametersList;

typedef struct manifest {
    uint64_t manifest_id;
    ChunkList chunks;
    BundleList bundles;
    LanguageList languages;
    FileList files;
    ParametersList parameters;
} Manifest;

int parse_body(Manifest* manifest, uint8_t* body);
void free_manifest(Manifest* manifest);
bool chunk_valid(BinaryData* chunk, uint64_t chunk_id, HashType hashType);
extern bool hasShaExtension;
bool checkShaExtension(void);
}
#include "zstd/lib/zstd.h"
#endif

namespace fs = std::filesystem;

#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
extern "C" {
int VERBOSE = 0;
}
#endif

namespace patch_downloader {
namespace {

constexpr const char* kBundleBase = "https://lol.dyn.riotcdn.net/channels/public/bundles";
constexpr int kMaxRetryFailuresPerMinute = 10;
constexpr long long kRetryFailureWindowMs = 60'000;
constexpr const char* kDefaultLanguage = "en_US";
constexpr const char* kWindowsPlatformLanguage = "windows";

struct HttpBytes {
    bool success = false;
    DWORD statusCode = 0;
    std::vector<uint8_t> data;
    std::string error;
};

bool HasFolderSeparator(const std::string& value);
std::string ToLowerAscii(std::string value);

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), result.data(), size);
    return result;
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
        nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), (int)value.size(),
        result.data(), size, nullptr, nullptr);
    return result;
}

std::string LastWinHttpError(const char* action) {
    return std::string(action) + " failed (" + std::to_string(GetLastError()) + ")";
}

bool ErrorLooksRetryable(const std::string& error) {
    return error.find("Network") != std::string::npos
        || error.find("connect") != std::string::npos
        || error.find("request") != std::string::npos
        || error.find("response") != std::string::npos
        || error.find("read") != std::string::npos
        || error.find("Expected ") != std::string::npos;
}

bool ShouldRetryDownload(DWORD statusCode, const std::string& error, int attempt, int maxAttempts) {
    (void)attempt;
    (void)maxAttempts;
    if (statusCode == 0) return ErrorLooksRetryable(error);
    if (statusCode == 408 || statusCode == 425 || statusCode == 429) return true;
    if (statusCode >= 500 && statusCode <= 599) return true;
    return ErrorLooksRetryable(error);
}

bool RetryFailureWindowExceeded(int consecutiveFailures, long long windowStartMs,
                                long long currentMs) {
    return consecutiveFailures >= kMaxRetryFailuresPerMinute
        && currentMs - windowStartMs <= kRetryFailureWindowMs;
}

bool ChunkPayloadErrorLooksRetryable(const std::string& error) {
    return error == "Chunk verification failed"
        || error == "ZSTD decompression failed";
}

bool ShouldRetryChunkPayloadFailure(const std::string& error, int consecutiveFailures,
                                    long long windowStartMs, long long currentMs) {
    return ChunkPayloadErrorLooksRetryable(error)
        && !RetryFailureWindowExceeded(consecutiveFailures, windowStartMs, currentMs);
}

bool RecordRetryFailure(int& failuresInWindow,
                        std::chrono::steady_clock::time_point& windowStart,
                        long long& elapsedMs) {
    auto now = std::chrono::steady_clock::now();
    elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - windowStart).count();
    if (failuresInWindow == 0 || elapsedMs > kRetryFailureWindowMs) {
        windowStart = now;
        failuresInWindow = 1;
        elapsedMs = 0;
    } else {
        ++failuresInWindow;
    }
    return RetryFailureWindowExceeded(failuresInWindow, 0, elapsedMs);
}

uint32_t ReadLe32(const uint8_t* data) {
    return (uint32_t)data[0]
        | ((uint32_t)data[1] << 8)
        | ((uint32_t)data[2] << 16)
        | ((uint32_t)data[3] << 24);
}

uint64_t ReadLe64(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= ((uint64_t)data[i]) << (i * 8);
    }
    return value;
}

struct ManifestHeader {
    uint32_t contentOffset = 0;
    uint32_t compressedSize = 0;
    uint64_t manifestId = 0;
    uint32_t uncompressedSize = 0;
};

bool ReadManifestHeader(const std::vector<uint8_t>& bytes, ManifestHeader& header, std::string* error = nullptr) {
    if (bytes.size() < 28) {
        if (error) *error = "Manifest is too small";
        return false;
    }
    if (std::memcmp(bytes.data(), "RMAN", 4) != 0) {
        if (error) *error = "Manifest is missing RMAN magic bytes";
        return false;
    }
    if (bytes[4] != 2) {
        if (error) *error = "Unsupported manifest version "
            + std::to_string(bytes[4]) + "." + std::to_string(bytes[5]);
        return false;
    }

    header.contentOffset = ReadLe32(bytes.data() + 8);
    header.compressedSize = ReadLe32(bytes.data() + 12);
    header.manifestId = ReadLe64(bytes.data() + 16);
    header.uncompressedSize = ReadLe32(bytes.data() + 24);

    if (header.contentOffset > bytes.size()
        || header.compressedSize > bytes.size() - header.contentOffset) {
        if (error) *error = "Manifest content range is outside downloaded data";
        return false;
    }
    if (header.compressedSize == 0 || header.uncompressedSize == 0) {
        if (error) *error = "Manifest contains empty compressed or uncompressed body";
        return false;
    }
    return true;
}

bool Cancelled(const Options& options);

int RetryDelayMs(int attempt) {
    int delay = 250 << std::min(attempt - 1, 3);
    return std::min(delay, 2000);
}

bool SleepBeforeRetry(const Options& options, int attempt) {
    int remainingMs = RetryDelayMs(attempt);
    while (remainingMs > 0) {
        if (Cancelled(options)) return false;
        int stepMs = std::min(remainingMs, 50);
        std::this_thread::sleep_for(std::chrono::milliseconds(stepMs));
        remainingMs -= stepMs;
    }
    return !Cancelled(options);
}

HttpBytes HttpGetBytes(const std::string& url, const std::wstring& extraHeaders = L"",
                       size_t expectedSize = 0) {
    HttpBytes result;
    std::wstring urlW = Utf8ToWide(url);

    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = (DWORD)-1;
    parts.dwHostNameLength = (DWORD)-1;
    parts.dwUrlPathLength = (DWORD)-1;
    parts.dwExtraInfoLength = (DWORD)-1;

    if (!WinHttpCrackUrl(urlW.c_str(), 0, 0, &parts)) {
        result.error = "Invalid URL";
        return result;
    }

    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    HINTERNET session = WinHttpOpen(L"ReplayKeeper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        result.error = "Failed to open WinHTTP session";
        return result;
    }
    WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);

    HINTERNET connect = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        result.error = "Failed to connect";
        return result;
    }

    DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Failed to create request";
        return result;
    }

    std::wstring headers = L"User-Agent: ReplayKeeper\r\n";
    headers += extraHeaders;

    BOOL ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        result.error = LastWinHttpError("Network request");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    result.statusCode = statusCode;

    DWORD bytesAvailable = 0;
    bool readFailed = false;
    do {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(request, &bytesAvailable)) {
            result.error = LastWinHttpError("Network read");
            readFailed = true;
            break;
        }
        if (bytesAvailable == 0) break;

        size_t oldSize = result.data.size();
        result.data.resize(oldSize + bytesAvailable);
        DWORD bytesRead = 0;
        if (WinHttpReadData(request, result.data.data() + oldSize, bytesAvailable, &bytesRead)) {
            result.data.resize(oldSize + bytesRead);
        } else {
            result.data.resize(oldSize);
            result.error = LastWinHttpError("Network read");
            readFailed = true;
            break;
        }
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    result.success = !readFailed && statusCode >= 200 && statusCode < 300;
    if (!result.success && result.error.empty()) {
        result.error = "HTTP " + std::to_string(statusCode);
    }
    if (result.success && expectedSize > 0 && result.data.size() != expectedSize) {
        result.success = false;
        result.error = "Expected " + std::to_string(expectedSize) + " bytes, got "
            + std::to_string(result.data.size());
    }
    return result;
}

bool Cancelled(const Options& options) {
    return options.cancelRequested && options.cancelRequested->load();
}

void Report(const Options& options, const Progress& progress) {
    if (options.onProgress) options.onProgress(progress);
}

std::string EffectiveLanguage(const Options& options) {
    return options.language.empty() ? kDefaultLanguage : options.language;
}

std::string LanguageStatusLabel(const std::string& selectedLanguage) {
    return selectedLanguage.empty() ? "all languages" : selectedLanguage;
}

bool ShouldIncludeLanguageNames(const std::vector<std::string>& fileLanguages,
                                const std::string& selectedLanguage) {
    if (fileLanguages.empty()) return true;
    std::string effectiveLanguage = selectedLanguage.empty() ? kDefaultLanguage : selectedLanguage;
    if (std::find(fileLanguages.begin(), fileLanguages.end(), kWindowsPlatformLanguage) != fileLanguages.end()) {
        return true;
    }
    return std::find(fileLanguages.begin(), fileLanguages.end(), effectiveLanguage) != fileLanguages.end();
}

#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
bool ShouldIncludeFileForLanguage(const File& file, const std::string& selectedLanguage) {
    if (file.languages.length == 0) return true;
    std::string effectiveLanguage = selectedLanguage.empty() ? kDefaultLanguage : selectedLanguage;
    for (uint32_t i = 0; i < file.languages.length; ++i) {
        const char* languageName = file.languages.objects[i].name;
        if (languageName && std::strcmp(languageName, kWindowsPlatformLanguage) == 0) return true;
        if (languageName && effectiveLanguage == languageName) return true;
    }
    return false;
}
#endif

HttpBytes HttpGetBytesWithRetry(const Options& options, const std::string& url,
                                const std::wstring& extraHeaders = L"",
                                size_t expectedSize = 0, Progress* progress = nullptr,
                                const std::string& label = "Download") {
    HttpBytes lastResult;
    int failuresInWindow = 0;
    auto windowStart = std::chrono::steady_clock::now();
    while (true) {
        if (Cancelled(options)) {
            lastResult.error = "Download cancelled";
            return lastResult;
        }

        lastResult = HttpGetBytes(url, extraHeaders, expectedSize);
        if (lastResult.success) return lastResult;

        if (!ShouldRetryDownload(lastResult.statusCode, lastResult.error, 0, 0)) {
            return lastResult;
        }

        long long elapsedMs = 0;
        if (RecordRetryFailure(failuresInWindow, windowStart, elapsedMs)) {
            lastResult.error = "Too many retryable failures in 60 seconds: " + lastResult.error;
            return lastResult;
        }

        if (progress) {
            progress->message = label + " failed, retrying after transient failure "
                + std::to_string(failuresInWindow) + "/"
                + std::to_string(kMaxRetryFailuresPerMinute)
                + " in 60s"
                + ": " + lastResult.error;
            Report(options, *progress);
        }

        if (!SleepBeforeRetry(options, failuresInWindow)) {
            lastResult.error = "Download cancelled";
            return lastResult;
        }
    }
}

#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE

struct ChunkPayloadResult {
    bool success = false;
    bool cancelled = false;
    std::string error;
    std::vector<uint8_t> data;
};

struct FlatObject {
    const uint8_t* object = nullptr;
    const uint8_t* vtable = nullptr;
};

struct DirectoryInfo {
    uint64_t parentId = 0;
    std::string name;
};

FlatObject ReadFlatObject(const uint8_t* position) {
    const uint8_t* object = position + ReadLe32(position);
    int32_t vtableOffset = (int32_t)ReadLe32(object);
    return {object, object - vtableOffset};
}

int FlatFieldCount(const FlatObject& object) {
    uint16_t vtableSize = (uint16_t)object.vtable[0] | ((uint16_t)object.vtable[1] << 8);
    return (vtableSize - 4) / 2;
}

const uint8_t* FlatField(const FlatObject& object, int index) {
    if (FlatFieldCount(object) <= index) return nullptr;

    const uint8_t* offsetPtr = object.vtable + 4 + index * 2;
    uint16_t offset = (uint16_t)offsetPtr[0] | ((uint16_t)offsetPtr[1] << 8);
    return offset == 0 ? nullptr : object.object + offset;
}

const uint8_t* FlatVector(const uint8_t* field, uint32_t& length) {
    if (!field) {
        length = 0;
        return nullptr;
    }

    const uint8_t* vector = field + ReadLe32(field);
    length = ReadLe32(vector);
    return vector + 4;
}

std::string FlatString(const uint8_t* field) {
    if (!field) return "";
    const uint8_t* value = field + ReadLe32(field);
    uint32_t length = ReadLe32(value);
    return std::string(reinterpret_cast<const char*>(value + 4), length);
}

uint64_t FlatU64(const FlatObject& object, int index) {
    const uint8_t* field = FlatField(object, index);
    return field ? ReadLe64(field) : 0;
}

uint64_t FindDirectoryIdField(const FlatObject& fileEntry,
                              const std::map<uint64_t, DirectoryInfo>& directories) {
    uint64_t directoryId = FlatU64(fileEntry, 1);
    if (directoryId == 0 || directories.find(directoryId) != directories.end()) {
        return directoryId;
    }

    int fieldCount = FlatFieldCount(fileEntry);
    for (int fieldIndex = 2; fieldIndex < fieldCount; ++fieldIndex) {
        const uint8_t* field = FlatField(fileEntry, fieldIndex);
        if (!field) continue;

        uint64_t candidate = ReadLe64(field);
        if (candidate != 0 && directories.find(candidate) != directories.end()) {
            return candidate;
        }
    }

    return directoryId;
}

bool IsKnownManifestRoot(const std::string& value) {
    std::string lower = ToLowerAscii(value);
    return lower == "data" || lower == "config";
}

std::string JoinManifestPath(const std::vector<std::string>& parts, std::string fileName) {
    std::string path;
    for (const std::string& part : parts) {
        if (part.empty()) continue;
        if (!path.empty()) path += "/";
        path += part;
    }
    if (!path.empty()) path += "/";
    path += fileName;
    return path;
}

std::string BuildManifestPath(std::string fileName, uint64_t directoryId,
                              const std::map<uint64_t, DirectoryInfo>& directories) {
    std::vector<std::string> parts;
    for (size_t guard = 0; directoryId != 0 && guard < directories.size(); ++guard) {
        auto it = directories.find(directoryId);
        if (it == directories.end()) break;
        if (!it->second.name.empty()) parts.push_back(it->second.name);
        directoryId = it->second.parentId;
    }

    if (!parts.empty() && IsKnownManifestRoot(parts.front())) {
        return JoinManifestPath(parts, std::move(fileName));
    }

    if (!parts.empty() && IsKnownManifestRoot(parts.back())) {
        std::vector<std::string> rootToLeaf = parts;
        std::reverse(rootToLeaf.begin(), rootToLeaf.end());
        return JoinManifestPath(rootToLeaf, std::move(fileName));
    }

    std::reverse(parts.begin(), parts.end());
    return JoinManifestPath(parts, std::move(fileName));
}

std::vector<std::string> ExtractManifestFullPaths(const uint8_t* body) {
    std::vector<std::string> paths;

    FlatObject root = ReadFlatObject(body);
    uint32_t fileCount = 0;
    uint32_t directoryCount = 0;
    const uint8_t* fileEntries = FlatVector(FlatField(root, 2), fileCount);
    const uint8_t* directoryEntries = FlatVector(FlatField(root, 3), directoryCount);
    if (!fileEntries || !directoryEntries) return paths;

    std::map<uint64_t, DirectoryInfo> directories;
    std::map<uint64_t, FlatObject> rawDirectories;
    for (uint32_t i = 0; i < directoryCount; ++i) {
        FlatObject directory = ReadFlatObject(directoryEntries + i * 4);
        uint64_t directoryId = FlatU64(directory, 0);
        if (directoryId == 0) continue;

        DirectoryInfo info;
        info.parentId = FlatU64(directory, 1);
        info.name = FlatString(FlatField(directory, 2));
        directories[directoryId] = std::move(info);
        rawDirectories[directoryId] = directory;
    }

    for (auto& entry : directories) {
        if (entry.second.parentId == 0 || directories.find(entry.second.parentId) != directories.end()) {
            continue;
        }

        auto raw = rawDirectories.find(entry.first);
        if (raw == rawDirectories.end()) continue;
        int fieldCount = FlatFieldCount(raw->second);
        for (int fieldIndex = 3; fieldIndex < fieldCount; ++fieldIndex) {
            const uint8_t* field = FlatField(raw->second, fieldIndex);
            if (!field) continue;

            uint64_t candidate = ReadLe64(field);
            if (candidate != 0 && directories.find(candidate) != directories.end()) {
                entry.second.parentId = candidate;
                break;
            }
        }
    }

    paths.reserve(fileCount);
    for (uint32_t i = 0; i < fileCount; ++i) {
        FlatObject fileEntry = ReadFlatObject(fileEntries + i * 4);
        std::string name = FlatString(FlatField(fileEntry, 3));
        uint64_t directoryId = FindDirectoryIdField(fileEntry, directories);
        paths.push_back(BuildManifestPath(std::move(name), directoryId, directories));
    }

    return paths;
}

char* DuplicateForManifestFree(const std::string& value) {
    char* copy = static_cast<char*>(std::malloc(value.size() + 1));
    if (!copy) return nullptr;

    std::memcpy(copy, value.c_str(), value.size() + 1);
    return copy;
}

void ApplyManifestFullPaths(Manifest* manifest, const std::vector<std::string>& fullPaths) {
    if (!manifest || fullPaths.size() != manifest->files.length) return;

    for (uint32_t i = 0; i < manifest->files.length; ++i) {
        if (fullPaths[i].empty() || !HasFolderSeparator(fullPaths[i])) continue;
        if (manifest->files.objects[i].name && HasFolderSeparator(manifest->files.objects[i].name)) continue;

        char* replacement = DuplicateForManifestFree(fullPaths[i]);
        if (!replacement) continue;

        std::free(manifest->files.objects[i].name);
        manifest->files.objects[i].name = replacement;
    }
}

std::string BundleUrl(uint64_t bundleId) {
    std::ostringstream out;
    out << kBundleBase << "/" << std::uppercase << std::hex << std::setw(16)
        << std::setfill('0') << bundleId << ".bundle";
    return out.str();
}

bool LooksLikeUrl(const std::string& value) {
    return value.find("://") != std::string::npos;
}

bool HasFolderSeparator(const std::string& value) {
    return value.find('/') != std::string::npos || value.find('\\') != std::string::npos;
}

std::string ToLowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    }
    return value;
}

bool EndsWithAsciiIgnoreCase(const std::string& value, const std::string& suffix) {
    if (suffix.size() > value.size()) return false;
    return ToLowerAscii(value.substr(value.size() - suffix.size())) == ToLowerAscii(suffix);
}

bool PathStartsWithDataRoot(const fs::path& path) {
    std::string value = WideToUtf8(path.wstring());
    std::replace(value.begin(), value.end(), '\\', '/');
    value = ToLowerAscii(value);
    return value == "data" || value.rfind("data/", 0) == 0;
}

std::string TrimUrlDecorations(std::string value) {
    size_t fragment = value.find('#');
    if (fragment != std::string::npos) value.erase(fragment);
    size_t query = value.find('?');
    if (query != std::string::npos) value.erase(query);
    return value;
}

std::string LinkPathFromKnownRoot(const std::string& linkValue) {
    std::string value = TrimUrlDecorations(linkValue);
    std::replace(value.begin(), value.end(), '\\', '/');
    std::string lowerValue = ToLowerAscii(value);

    struct KnownRoot {
        const char* match;
        const char* canonical;
    };
    static constexpr KnownRoot kKnownRoots[] = {
        {"data/", "DATA/"},
        {"config/", "Config/"}
    };
    for (const KnownRoot& root : kKnownRoots) {
        size_t position = lowerValue.find(root.match);
        if (position != std::string::npos) {
            return std::string(root.canonical) + value.substr(position + std::strlen(root.match));
        }
    }

    if (LooksLikeUrl(linkValue)) return "";
    return value;
}

std::wstring ManifestOutputRelativePath(const char* name, const char* link) {
    std::string value = name ? name : "";
    std::string linkValue = link ? link : "";
    if (!HasFolderSeparator(value) && HasFolderSeparator(linkValue)) {
        std::string linkPath = LinkPathFromKnownRoot(linkValue);
        if (!linkPath.empty()) value = linkPath;
    }

    std::replace(value.begin(), value.end(), '\\', '/');

    std::wstring result;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('/', start);
        std::string part = value.substr(start,
            end == std::string::npos ? std::string::npos : end - start);
        if (!part.empty() && part != "." && part != "..") {
            if (!result.empty()) result += L"\\";
            result += Utf8ToWide(part);
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }

    if (result.empty()) result = L"unnamed";
    return result;
}

bool FileChunkValid(const fs::path& path, const Chunk& chunk) {
    if (!fs::exists(path)) return false;
    std::error_code ec;
    uint64_t size = fs::file_size(path, ec);
    if (ec || size < chunk.file_offset + chunk.uncompressed_size) return false;

    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;

    std::vector<uint8_t> data(chunk.uncompressed_size);
    input.seekg((std::streamoff)chunk.file_offset, std::ios::beg);
    input.read(reinterpret_cast<char*>(data.data()), (std::streamsize)data.size());
    if ((uint64_t)input.gcount() != data.size()) return false;

    BinaryData binary = {
        .length = data.size(),
        .data = data.data()
    };
    return chunk_valid(&binary, chunk.chunk_id, chunk.hashType);
}

ChunkPayloadResult DownloadVerifiedChunkWithRetry(const Options& options, const Chunk& chunk,
                                                  const std::wstring& rangeHeader,
                                                  Progress& progress) {
    ChunkPayloadResult result;
    int failuresInWindow = 0;
    auto windowStart = std::chrono::steady_clock::now();

    while (true) {
        if (Cancelled(options)) {
            result.cancelled = true;
            result.error = "Download cancelled";
            return result;
        }

        HttpBytes compressed = HttpGetBytesWithRetry(options, BundleUrl(chunk.bundle_id),
            rangeHeader, chunk.compressed_size);
        if (!compressed.success) {
            result.error = "Failed to download bundle range: " + compressed.error;
            return result;
        }

        std::vector<uint8_t> decompressed(chunk.uncompressed_size);
        size_t decompressedSize = ZSTD_decompress(decompressed.data(), decompressed.size(),
            compressed.data.data(), compressed.data.size());
        if (ZSTD_isError(decompressedSize) || decompressedSize != decompressed.size()) {
            result.error = "ZSTD decompression failed";
        } else {
            BinaryData binary = {
                .length = decompressed.size(),
                .data = decompressed.data()
            };
            if (chunk_valid(&binary, chunk.chunk_id, chunk.hashType)) {
                result.success = true;
                result.data = std::move(decompressed);
                return result;
            }
            result.error = "Chunk verification failed";
        }

        long long elapsedMs = 0;
        RecordRetryFailure(failuresInWindow, windowStart, elapsedMs);
        if (!ShouldRetryChunkPayloadFailure(result.error, failuresInWindow, 0, elapsedMs)) {
            result.error = "Too many chunk payload failures in 60 seconds: " + result.error;
            return result;
        }

        progress.message = result.error + ", retrying "
            + std::to_string(failuresInWindow) + "/"
            + std::to_string(kMaxRetryFailuresPerMinute)
            + " in 60s";
        Report(options, progress);

        if (!SleepBeforeRetry(options, failuresInWindow)) {
            result.cancelled = true;
            result.error = "Download cancelled";
            return result;
        }
    }
}

Manifest* ParseManifestSafely(const std::vector<uint8_t>& bytes, std::string& error) {
    ManifestHeader header;
    if (!ReadManifestHeader(bytes, header, &error)) {
        return nullptr;
    }

    constexpr uint32_t kMaxReasonableManifestBody = 512u * 1024u * 1024u;
    if (header.uncompressedSize > kMaxReasonableManifestBody) {
        error = "Manifest uncompressed body is unexpectedly large";
        return nullptr;
    }

    std::vector<uint8_t> uncompressed(header.uncompressedSize);
    size_t decompressedSize = ZSTD_decompress(uncompressed.data(), uncompressed.size(),
        bytes.data() + header.contentOffset, header.compressedSize);
    if (ZSTD_isError(decompressedSize) || decompressedSize != uncompressed.size()) {
        error = "Manifest ZSTD decompression failed";
        return nullptr;
    }

    Manifest* manifest = static_cast<Manifest*>(std::calloc(1, sizeof(Manifest)));
    if (!manifest) {
        error = "Failed to allocate manifest";
        return nullptr;
    }

    manifest->manifest_id = header.manifestId;
    if (parse_body(manifest, uncompressed.data()) != 0) {
        free_manifest(manifest);
        error = "Failed to parse manifest body";
        return nullptr;
    }
    ApplyManifestFullPaths(manifest, ExtractManifestFullPaths(uncompressed.data()));

    return manifest;
}

Result DownloadFileChunks(const Options& options, const File& file, const fs::path& filePath,
                          const std::string& displayPath, Progress& progress) {
    Result result;
    std::vector<Chunk> missing;
    missing.reserve(file.chunks.length);

    for (uint32_t i = 0; i < file.chunks.length; ++i) {
        if (Cancelled(options)) {
            result.cancelled = true;
            result.message = "Download cancelled";
            return result;
        }

        const Chunk& chunk = file.chunks.objects[i];
        if (!FileChunkValid(filePath, chunk)) {
            missing.push_back(chunk);
        } else {
            ++progress.chunksDone;
        }
    }

    if (missing.empty()) {
        ++progress.filesDone;
        progress.message = "Verified " + displayPath;
        Report(options, progress);
        result.success = true;
        return result;
    }

    fs::create_directories(filePath.parent_path());
    if (!fs::exists(filePath)) {
        std::ofstream create(filePath, std::ios::binary);
        create.close();
    }
    std::error_code resizeError;
    fs::resize_file(filePath, file.file_size, resizeError);
    if (resizeError) {
        result.message = "Failed to resize " + std::string(file.name);
        return result;
    }

    std::fstream output(filePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!output.is_open()) {
        result.message = "Failed to open " + std::string(file.name);
        return result;
    }

    std::atomic_size_t nextIndex{0};
    std::atomic_bool failed{false};
    std::mutex writeMutex;
    std::mutex errorMutex;
    std::string errorMessage;

    int threadCount = std::clamp(options.threads, 1, 4);
    threadCount = std::min<int>(threadCount, (int)missing.size());
    std::vector<std::thread> workers;
    workers.reserve(threadCount);

    auto worker = [&]() {
        while (!failed.load()) {
            if (Cancelled(options)) return;
            size_t index = nextIndex.fetch_add(1);
            if (index >= missing.size()) return;

            const Chunk& chunk = missing[index];
            uint64_t firstByte = chunk.bundle_offset;
            uint64_t lastByte = chunk.bundle_offset + chunk.compressed_size - 1;
            std::wstring rangeHeader = L"Range: bytes=" + std::to_wstring(firstByte)
                + L"-" + std::to_wstring(lastByte) + L"\r\n";

            ChunkPayloadResult payload = DownloadVerifiedChunkWithRetry(options, chunk,
                rangeHeader, progress);
            if (!payload.success) {
                if (payload.cancelled) return;
                std::lock_guard<std::mutex> lock(errorMutex);
                errorMessage = payload.error.empty() ? "Patch download failed" : payload.error;
                failed.store(true);
                return;
            }

            {
                std::lock_guard<std::mutex> lock(writeMutex);
                output.seekp((std::streamoff)chunk.file_offset, std::ios::beg);
                output.write(reinterpret_cast<const char*>(payload.data.data()),
                    (std::streamsize)payload.data.size());
                output.flush();
                ++progress.chunksDone;
                progress.message = "Downloading " + displayPath;
                Report(options, progress);
            }
        }
    };

    for (int i = 0; i < threadCount; ++i) workers.emplace_back(worker);
    for (std::thread& thread : workers) thread.join();
    output.close();

    if (Cancelled(options)) {
        result.cancelled = true;
        result.message = "Download cancelled";
        return result;
    }
    if (failed.load()) {
        result.message = errorMessage.empty() ? "Patch download failed" : errorMessage;
        return result;
    }

    ++progress.filesDone;
    result.success = true;
    return result;
}

#endif

}

bool CoreAvailable() {
    return REPLAYKEEPER_PATCH_DOWNLOAD_CORE != 0;
}

std::string CoreUnavailableReason() {
#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
    return "";
#else
    return "Patch downloader core is disabled because ManifestDownloader zstd/BLAKE3 submodules are missing.";
#endif
}

bool ShouldRetryDownloadForTest(unsigned long statusCode, const std::string& error,
                                int attempt, int maxAttempts) {
    return ShouldRetryDownload((DWORD)statusCode, error, attempt, maxAttempts);
}

bool RetryFailureWindowExceededForTest(int consecutiveFailures, long long windowStartMs,
                                       long long currentMs) {
    return RetryFailureWindowExceeded(consecutiveFailures, windowStartMs, currentMs);
}

bool ShouldRetryChunkPayloadFailureForTest(const std::string& error, int consecutiveFailures,
                                           long long windowStartMs, long long currentMs) {
    return ShouldRetryChunkPayloadFailure(error, consecutiveFailures, windowStartMs, currentMs);
}

bool ManifestBytesLookSafeForTest(const std::vector<unsigned char>& bytes) {
    std::vector<uint8_t> raw(bytes.begin(), bytes.end());
    ManifestHeader header;
    if (!ReadManifestHeader(raw, header)) return false;
#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
    std::vector<uint8_t> uncompressed(header.uncompressedSize);
    size_t decompressedSize = ZSTD_decompress(uncompressed.data(), uncompressed.size(),
        raw.data() + header.contentOffset, header.compressedSize);
    return !ZSTD_isError(decompressedSize) && decompressedSize == uncompressed.size();
#else
    return true;
#endif
}

std::string DefaultPatchLanguageForTest() {
    return kDefaultLanguage;
}

bool ShouldIncludeFileForSelectedLanguageForTest(const std::vector<std::string>& fileLanguages,
                                                 const std::string& selectedLanguage) {
    return ShouldIncludeLanguageNames(fileLanguages, selectedLanguage);
}

std::wstring OutputRelativePathForTest(const char* fileName, const char* fileLink) {
    return ManifestOutputRelativePath(fileName, fileLink);
}

std::wstring ParsedManifestOutputPathForTest(const std::vector<unsigned char>& bytes,
                                             const char* fileName) {
#if REPLAYKEEPER_PATCH_DOWNLOAD_CORE
    std::vector<uint8_t> raw(bytes.begin(), bytes.end());
    std::string error;
    Manifest* manifest = ParseManifestSafely(raw, error);
    if (!manifest) return L"";

    std::wstring targetName = Utf8ToWide(fileName ? fileName : "");
    std::wstring result;
    for (uint32_t i = 0; i < manifest->files.length; ++i) {
        const File& file = manifest->files.objects[i];
        fs::path relativePath(ManifestOutputRelativePath(file.name, file.link));
        if (relativePath.filename().wstring() == targetName) {
            result = relativePath.wstring();
            break;
        }
    }

    free_manifest(manifest);
    return result;
#else
    (void)bytes;
    (void)fileName;
    return L"";
#endif
}

Result DownloadPatch(const Options& options) {
    if (options.manifestUrl.empty()) {
        return {.success = false, .message = "Missing manifest URL"};
    }
    if (options.outputDirectory.empty()) {
        return {.success = false, .message = "Select an output folder first"};
    }
    if (Cancelled(options)) {
        return {.success = false, .cancelled = true, .message = "Download cancelled"};
    }

#if !REPLAYKEEPER_PATCH_DOWNLOAD_CORE
    (void)options;
    return {.success = false, .message = CoreUnavailableReason()};
#else
    Progress progress;
    progress.message = "Downloading manifest...";
    Report(options, progress);
    hasShaExtension = checkShaExtension();
    std::string selectedLanguage = EffectiveLanguage(options);

    HttpBytes manifestBytes = HttpGetBytesWithRetry(options, options.manifestUrl,
        L"", 0, &progress, "Manifest download");
    if (!manifestBytes.success) {
        return {.success = false, .message = "Failed to download manifest: " + manifestBytes.error};
    }
    if (Cancelled(options)) {
        return {.success = false, .cancelled = true, .message = "Download cancelled"};
    }

    std::string manifestError;
    Manifest* manifest = ParseManifestSafely(manifestBytes.data, manifestError);
    if (!manifest) {
        return {.success = false, .message = "Failed to parse manifest: " + manifestError};
    }

    for (uint32_t i = 0; i < manifest->files.length; ++i) {
        if (!ShouldIncludeFileForLanguage(manifest->files.objects[i], selectedLanguage)) {
            continue;
        }
        ++progress.filesTotal;
        progress.chunksTotal += manifest->files.objects[i].chunks.length;
    }
    std::string samplePath;
    for (uint32_t i = 0; i < manifest->files.length; ++i) {
        const File& file = manifest->files.objects[i];
        if (!ShouldIncludeFileForLanguage(file, selectedLanguage) || !file.name) {
            continue;
        }
        std::string name = file.name;
        if (name.find("Aatrox.wad.client") != std::string::npos
            || name.find("Companions.wad.client") != std::string::npos) {
            samplePath = name;
            break;
        }
    }
    progress.message = "Manifest parsed (" + LanguageStatusLabel(selectedLanguage) + ")";
    if (!samplePath.empty()) {
        progress.message += ", sample: " + samplePath;
    }
    Report(options, progress);

    Result finalResult;
    fs::path outputRoot(options.outputDirectory);
    for (uint32_t i = 0; i < manifest->files.length; ++i) {
        if (Cancelled(options)) {
            finalResult.cancelled = true;
            finalResult.message = "Download cancelled";
            break;
        }

        const File& file = manifest->files.objects[i];
        if (!ShouldIncludeFileForLanguage(file, selectedLanguage)) {
            continue;
        }
        fs::path relativePath = ManifestOutputRelativePath(file.name, file.link);
        std::string manifestName = file.name ? file.name : "";
        if (EndsWithAsciiIgnoreCase(manifestName, ".wad.client") && !PathStartsWithDataRoot(relativePath)) {
            finalResult.success = false;
            finalResult.message = "Manifest path resolved outside DATA for " + manifestName
                + " -> " + WideToUtf8(relativePath.wstring());
            break;
        }

        fs::path filePath = outputRoot / relativePath;
        Result fileResult = DownloadFileChunks(options, file, filePath,
            WideToUtf8(relativePath.wstring()), progress);
        if (!fileResult.success) {
            finalResult = fileResult;
            break;
        }
    }

    if (!finalResult.success && !finalResult.cancelled && finalResult.message.empty()) {
        finalResult.success = true;
        finalResult.message = "Patch download complete";
    }

    free_manifest(manifest);
    return finalResult;
#endif
}

}
