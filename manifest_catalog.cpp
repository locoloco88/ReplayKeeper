#include "manifest_catalog.hpp"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <sstream>
#include <stdexcept>
#include <windows.h>
#include <winhttp.h>

namespace manifest_catalog {
namespace {

struct HttpResult {
    bool success = false;
    DWORD statusCode = 0;
    std::string body;
    std::string error;
};

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return L"";
    int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(), result.data(), size);
    return result;
}

HttpResult HttpGet(const std::string& url, const std::wstring& extraHeaders = L"") {
    HttpResult result;
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
    if (parts.dwExtraInfoLength > 0) {
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }

    HINTERNET session = WinHttpOpen(L"ReplayKeeper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!session) {
        result.error = "Failed to open WinHTTP session";
        return result;
    }

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
    headers += L"Accept: application/vnd.github+json\r\n";
    headers += extraHeaders;

    BOOL ok = WinHttpSendRequest(request, headers.c_str(), (DWORD)-1,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(request, nullptr);
    if (!ok) {
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.error = "Network request failed";
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    result.statusCode = statusCode;

    DWORD bytesAvailable = 0;
    do {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(request, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;

        std::string buffer(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (WinHttpReadData(request, buffer.data(), bytesAvailable, &bytesRead)) {
            result.body.append(buffer.data(), bytesRead);
        }
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    result.success = statusCode >= 200 && statusCode < 300;
    if (!result.success) {
        result.error = "HTTP " + std::to_string(statusCode);
    }
    return result;
}

std::vector<std::string> ExtractJsonObjects(const std::string& json) {
    std::vector<std::string> objects;
    bool inString = false;
    bool escape = false;
    int depth = 0;
    size_t objectStart = std::string::npos;

    for (size_t i = 0; i < json.size(); ++i) {
        char ch = json[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\' && inString) {
            escape = true;
            continue;
        }
        if (ch == '"') {
            inString = !inString;
            continue;
        }
        if (inString) continue;

        if (ch == '{') {
            if (depth == 0) objectStart = i;
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(json.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}

std::string UnescapeJsonString(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    bool escape = false;
    for (char ch : value) {
        if (escape) {
            switch (ch) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: result.push_back(ch); break;
            }
            escape = false;
        } else if (ch == '\\') {
            escape = true;
        } else {
            result.push_back(ch);
        }
    }
    return result;
}

std::string ExtractJsonString(const std::string& object, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t keyPos = object.find(needle);
    if (keyPos == std::string::npos) return "";
    size_t colon = object.find(':', keyPos + needle.size());
    if (colon == std::string::npos) return "";
    size_t quote = object.find('"', colon + 1);
    if (quote == std::string::npos) return "";

    std::string raw;
    bool escape = false;
    for (size_t i = quote + 1; i < object.size(); ++i) {
        char ch = object[i];
        if (escape) {
            raw.push_back('\\');
            raw.push_back(ch);
            escape = false;
            continue;
        }
        if (ch == '\\') {
            escape = true;
            continue;
        }
        if (ch == '"') return UnescapeJsonString(raw);
        raw.push_back(ch);
    }
    return "";
}

std::vector<int> VersionParts(const std::string& version) {
    std::vector<int> parts;
    std::stringstream ss(version);
    std::string part;
    while (std::getline(ss, part, '.')) {
        try {
            parts.push_back(std::stoi(part));
        } catch (...) {
            parts.push_back(0);
        }
    }
    return parts;
}

bool PatchVersionNewer(const PatchEntry& a, const PatchEntry& b) {
    std::vector<int> left = VersionParts(a.version);
    std::vector<int> right = VersionParts(b.version);
    size_t count = std::max(left.size(), right.size());
    left.resize(count);
    right.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (left[i] != right[i]) return left[i] > right[i];
    }
    return a.version > b.version;
}

std::string DisplayPatchVersion(const std::string& version) {
    size_t firstDot = version.find('.');
    if (firstDot == std::string::npos) return version;
    size_t secondDot = version.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return version;
    return version.substr(0, secondDot);
}

bool LooksLikePatchTextFile(const std::string& name) {
    if (name.size() <= 4 || name.substr(name.size() - 4) != ".txt") return false;
    for (size_t i = 0; i + 4 < name.size(); ++i) {
        char ch = name[i];
        if (!std::isdigit((unsigned char)ch) && ch != '.') return false;
    }
    return true;
}

std::string Trim(std::string value) {
    while (!value.empty() && std::isspace((unsigned char)value.front())) value.erase(value.begin());
    while (!value.empty() && std::isspace((unsigned char)value.back())) value.pop_back();
    return value;
}

} // namespace

std::vector<std::string> ParseServerDirectoryForTest(const std::string& json) {
    std::vector<std::string> servers;
    for (const std::string& object : ExtractJsonObjects(json)) {
        if (ExtractJsonString(object, "type") == "dir") {
            std::string name = ExtractJsonString(object, "name");
            if (!name.empty()) servers.push_back(name);
        }
    }
    std::sort(servers.begin(), servers.end());
    return servers;
}

std::vector<PatchEntry> ParsePatchDirectoryForTest(const std::string& json) {
    std::vector<PatchEntry> patches;
    for (const std::string& object : ExtractJsonObjects(json)) {
        if (ExtractJsonString(object, "type") != "file") continue;
        std::string name = ExtractJsonString(object, "name");
        if (!LooksLikePatchTextFile(name)) continue;

        PatchEntry patch;
        patch.fileName = name;
        patch.version = name.substr(0, name.size() - 4);
        patch.displayVersion = DisplayPatchVersion(patch.version);
        patch.downloadUrl = ExtractJsonString(object, "download_url");
        if (patch.downloadUrl.empty()) continue;

        auto existing = std::find_if(patches.begin(), patches.end(), [&patch](const PatchEntry& entry) {
            return entry.displayVersion == patch.displayVersion;
        });
        if (existing == patches.end()) {
            patches.push_back(patch);
        } else if (PatchVersionNewer(patch, *existing)) {
            *existing = patch;
        }
    }
    std::sort(patches.begin(), patches.end(), PatchVersionNewer);
    return patches;
}

ServerListResult FetchLeagueServers() {
    ServerListResult result;
    HttpResult http = HttpGet("https://api.github.com/repos/Morilli/riot-manifests/contents/LoL?ref=master");
    if (!http.success) {
        result.error = "Failed to load servers: " + http.error;
        return result;
    }
    result.servers = ParseServerDirectoryForTest(http.body);
    result.success = !result.servers.empty();
    if (!result.success) result.error = "No League servers found in manifest repository";
    return result;
}

PatchListResult FetchLeaguePatches(const std::string& server) {
    PatchListResult result;
    if (server.empty()) {
        result.error = "Select a server first";
        return result;
    }

    std::string url = "https://api.github.com/repos/Morilli/riot-manifests/contents/LoL/"
        + server + "/windows/lol-game-client?ref=master";
    HttpResult http = HttpGet(url);
    if (!http.success) {
        result.error = "Failed to load patches: " + http.error;
        return result;
    }

    result.patches = ParsePatchDirectoryForTest(http.body);
    result.success = !result.patches.empty();
    if (!result.success) result.error = "No patches found for " + server;
    return result;
}

ManifestLinkResult FetchManifestLink(const PatchEntry& patch) {
    ManifestLinkResult result;
    if (patch.downloadUrl.empty()) {
        result.error = "Selected patch has no download URL";
        return result;
    }

    HttpResult http = HttpGet(patch.downloadUrl, L"Accept: text/plain\r\n");
    if (!http.success) {
        result.error = "Failed to load manifest link: " + http.error;
        return result;
    }

    result.manifestUrl = Trim(http.body);
    result.success = result.manifestUrl.rfind("https://", 0) == 0
        || result.manifestUrl.rfind("http://", 0) == 0;
    if (!result.success) result.error = "Patch text file did not contain a valid manifest URL";
    return result;
}

} // namespace manifest_catalog
