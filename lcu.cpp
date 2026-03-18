#include "lcu.hpp"
#include <fstream>
#include <sstream>
#include <vector>
#include <winhttp.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <algorithm>
#include <winternl.h>

typedef NTSTATUS(NTAPI* NtQueryInformationProcessFn)(
    HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);

static const char base64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string Base64Encode(const std::string& input) {
    std::string output;
    int val = 0, valb = -6;
    for (unsigned char c : input) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            output.push_back(base64_chars[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) output.push_back(base64_chars[((val << 8) >> (valb + 8)) & 0x3F]);
    while (output.size() % 4) output.push_back('=');
    return output;
}

static std::wstring GetProcessCommandLine(DWORD pid) {
    std::wstring cmdLine;
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!hProcess) return cmdLine;

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) { CloseHandle(hProcess); return cmdLine; }

    auto NtQueryInformationProcess = (NtQueryInformationProcessFn)
        GetProcAddress(ntdll, "NtQueryInformationProcess");
    if (!NtQueryInformationProcess) { CloseHandle(hProcess); return cmdLine; }

    PROCESS_BASIC_INFORMATION pbi = {};
    NTSTATUS status = NtQueryInformationProcess(hProcess, ProcessBasicInformation,
        &pbi, sizeof(pbi), nullptr);
    if (status != 0 || !pbi.PebBaseAddress) { CloseHandle(hProcess); return cmdLine; }

    PEB peb = {};
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(hProcess, pbi.PebBaseAddress, &peb, sizeof(peb), &bytesRead)) {
        CloseHandle(hProcess);
        return cmdLine;
    }

    RTL_USER_PROCESS_PARAMETERS params = {};
    if (!ReadProcessMemory(hProcess, peb.ProcessParameters, &params, sizeof(params), &bytesRead)) {
        CloseHandle(hProcess);
        return cmdLine;
    }

    if (params.CommandLine.Length > 0 && params.CommandLine.Buffer) {
        cmdLine.resize(params.CommandLine.Length / sizeof(WCHAR));
        ReadProcessMemory(hProcess, params.CommandLine.Buffer,
            &cmdLine[0], params.CommandLine.Length, &bytesRead);
    }

    CloseHandle(hProcess);
    return cmdLine;
}

static std::string ExtractCmdLineArg(const std::wstring& cmdLine, const std::wstring& argName) {
    size_t pos = cmdLine.find(argName);
    if (pos == std::wstring::npos) return "";
    size_t valueStart = pos + argName.length();
    size_t valueEnd = valueStart;
    while (valueEnd < cmdLine.length() && 
           cmdLine[valueEnd] != L' ' && 
           cmdLine[valueEnd] != L'"' && 
           cmdLine[valueEnd] != L'\0') {
        valueEnd++;
    }
    if (valueEnd <= valueStart) return "";
    std::wstring valueW = cmdLine.substr(valueStart, valueEnd - valueStart);
    int size = WideCharToMultiByte(CP_UTF8, 0, valueW.c_str(), (int)valueW.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, valueW.c_str(), (int)valueW.size(), &result[0], size, nullptr, nullptr);
    return result;
}

LcuCredentials ConnectToLcu() {
    LcuCredentials creds;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return creds;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    DWORD lcuPid = 0;
    if (Process32FirstW(snapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, L"LeagueClientUx.exe") == 0) {
                lcuPid = pe32.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &pe32));
    }
    CloseHandle(snapshot);
    if (lcuPid == 0) return creds;

    std::wstring cmdLine = GetProcessCommandLine(lcuPid);
    if (!cmdLine.empty()) {
        std::string port = ExtractCmdLineArg(cmdLine, L"--app-port=");
        std::string token = ExtractCmdLineArg(cmdLine, L"--remoting-auth-token=");
        if (!port.empty() && !token.empty()) {
            creds.port = port;
            creds.authToken = token;
            creds.valid = true;
            return creds;
        }
    }

    std::wstring paths[] = {
        L"C:\\Riot Games\\League of Legends\\lockfile",
        L"D:\\Riot Games\\League of Legends\\lockfile",
        L"C:\\Program Files\\Riot Games\\League of Legends\\lockfile",
        L"C:\\Program Files (x86)\\Riot Games\\League of Legends\\lockfile"
    };

    for (const auto& path : paths) {
        std::ifstream file(path.c_str());
        if (file.is_open()) {
            std::string line;
            if (std::getline(file, line)) {
                std::vector<std::string> parts;
                std::stringstream ss(line);
                std::string part;
                while (std::getline(ss, part, ':')) {
                    parts.push_back(part);
                }
                if (parts.size() >= 5) {
                    creds.port = parts[2];
                    creds.authToken = parts[3];
                    creds.valid = true;
                    file.close();
                    return creds;
                }
            }
            file.close();
        }
    }
    return creds;
}

LcuCredentials ReadLockfile(const std::wstring& leaguePath) {
    LcuCredentials creds;
    std::wstring lockfilePath = leaguePath + L"\\lockfile";
    std::ifstream file(lockfilePath.c_str());
    if (!file.is_open()) return creds;

    std::string line;
    if (std::getline(file, line)) {
        std::vector<std::string> parts;
        std::stringstream ss(line);
        std::string part;
        while (std::getline(ss, part, ':')) {
            parts.push_back(part);
        }
        if (parts.size() >= 5) {
            creds.port = parts[2];
            creds.authToken = parts[3];
            creds.valid = true;
        }
    }
    file.close();
    return creds;
}

static ApiResponse MakeLcuRequestInternal(const LcuCredentials& creds, const std::string& method, 
                                           const std::string& endpoint, const std::string& body) {
    ApiResponse response;
    if (!creds.valid) {
        response.error = "Invalid credentials";
        return response;
    }

    std::string authString = "riot:" + creds.authToken;
    std::string authBase64 = Base64Encode(authString);
    std::wstring authHeader = L"Authorization: Basic " + std::wstring(authBase64.begin(), authBase64.end());

    HINTERNET hSession = WinHttpOpen(L"Sleeper/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        response.error = "Failed to open WinHTTP session";
        return response;
    }

    int portNum = 0;
    try { portNum = std::stoi(creds.port); } catch (...) {
        WinHttpCloseHandle(hSession);
        response.error = "Invalid port: " + creds.port;
        return response;
    }
    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", 
        static_cast<INTERNET_PORT>(portNum), 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        response.error = "Failed to connect to LCU";
        return response;
    }

    std::wstring methodW(method.begin(), method.end());
    std::wstring endpointW(endpoint.begin(), endpoint.end());
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, methodW.c_str(), endpointW.c_str(),
        nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "Failed to create request";
        return response;
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    WinHttpAddRequestHeaders(hRequest, authHeader.c_str(), -1, WINHTTP_ADDREQ_FLAG_ADD);
    WinHttpAddRequestHeaders(hRequest, L"Content-Type: application/json", -1, WINHTTP_ADDREQ_FLAG_ADD);

    BOOL result;
    if (method == "POST" || method == "PUT") {
        result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    } else {
        result = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "Failed to send request";
        return response;
    }

    result = WinHttpReceiveResponse(hRequest, nullptr);
    if (!result) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        response.error = "Failed to receive response";
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    do {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;
        std::vector<char> buffer(bytesAvailable + 1);
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
            response.body.append(buffer.data(), bytesRead);
        }
    } while (bytesAvailable > 0);

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    response.success = (statusCode >= 200 && statusCode < 300);
    return response;
}

ApiResponse MakeLcuGet(const LcuCredentials& creds, const std::string& endpoint) {
    return MakeLcuRequestInternal(creds, "GET", endpoint, "");
}

ApiResponse MakeLcuPost(const LcuCredentials& creds, const std::string& endpoint, const std::string& jsonBody) {
    return MakeLcuRequestInternal(creds, "POST", endpoint, jsonBody);
}

static std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t keyPos = json.find(searchKey);
    if (keyPos == std::string::npos) return "";
    size_t colonPos = json.find(':', keyPos);
    if (colonPos == std::string::npos) return "";
    size_t valueStart = colonPos + 1;
    while (valueStart < json.size() && (json[valueStart] == ' ' || json[valueStart] == '\t')) {
        valueStart++;
    }
    if (valueStart >= json.size()) return "";
    if (json[valueStart] == '"') {
        size_t quoteEnd = json.find('"', valueStart + 1);
        if (quoteEnd != std::string::npos) {
            return json.substr(valueStart + 1, quoteEnd - valueStart - 1);
        }
    } else {
        size_t valueEnd = json.find_first_of(",}\n", valueStart);
        if (valueEnd != std::string::npos) {
            std::string value = json.substr(valueStart, valueEnd - valueStart);
            while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
                value.pop_back();
            }
            return value;
        }
    }
    return "";
}

static std::string GetGameDetails(const LcuCredentials& creds, const std::string& gameId) {
    std::string endpoint = "/lol-match-history/v1/games/" + gameId;
    ApiResponse response = MakeLcuGet(creds, endpoint);
    if (!response.success || response.statusCode != 200) return "";

    std::string gameVersion = ExtractJsonString(response.body, "gameVersion");
    std::string gameType = ExtractJsonString(response.body, "gameType");
    std::string queueId = ExtractJsonString(response.body, "queueId");
    std::string gameCreation = ExtractJsonString(response.body, "gameCreation");
    std::string gameDuration = ExtractJsonString(response.body, "gameDuration");
    if (gameVersion.empty() || gameCreation.empty() || gameDuration.empty()) return "";

    long long creation = 0, duration = 0;
    try {
        creation = std::stoll(gameCreation);
        duration = std::stoll(gameDuration);
    } catch (...) { return ""; }
    long long gameEnd = creation + (duration * 1000);

    std::string json = "{";
    json += "\"gameVersion\":\"" + gameVersion + "\",";
    json += "\"gameType\":\"" + gameType + "\",";
    json += "\"queueId\":" + queueId + ",";
    json += "\"gameEnd\":" + std::to_string(gameEnd);
    json += "}";
    return json;
}

ReplayMetadata GetReplayMetadata(const LcuCredentials& creds, const std::string& gameId) {
    ReplayMetadata metadata;
    metadata.gameId = gameId;
    std::string endpoint = "/lol-replays/v1/metadata/" + gameId;
    ApiResponse response = MakeLcuGet(creds, endpoint);
    if (response.success) {
        metadata.state = ExtractJsonString(response.body, "state");
        metadata.valid = !metadata.state.empty();
        return metadata;
    }
    if (response.statusCode == 404) {
        std::string gameDetails = GetGameDetails(creds, gameId);
        if (gameDetails.empty()) return metadata;
        std::string createEndpoint = "/lol-replays/v2/metadata/" + gameId + "/create";
        ApiResponse createResponse = MakeLcuPost(creds, createEndpoint, gameDetails);
        if (createResponse.statusCode == 200 || createResponse.statusCode == 201 || createResponse.statusCode == 204) {
            ApiResponse retryResponse = MakeLcuGet(creds, endpoint);
            if (retryResponse.success) {
                metadata.state = ExtractJsonString(retryResponse.body, "state");
                metadata.valid = !metadata.state.empty();
                return metadata;
            }
        }
    }
    return metadata;
}

std::string DownloadReplay(const LcuCredentials& creds, const std::string& gameId) {
    if (!creds.valid) return "Error: LeagueClient not found. Ensure it's running.";
    ReplayMetadata metadata = GetReplayMetadata(creds, gameId);
    if (!metadata.valid) return "GameID " + gameId + " not found";
    std::string state = metadata.state;
    std::transform(state.begin(), state.end(), state.begin(), ::tolower);
    if (state == "watch") return "Already downloaded";
    if (state == "incompatible") return "Replay is from a different patch";
    if (state != "download") return "Unknown state: " + metadata.state;

    std::string endpoint = "/lol-replays/v1/rofls/" + gameId + "/download";
    ApiResponse response = MakeLcuPost(creds, endpoint, "{}");
    if (response.statusCode == 200 || response.statusCode == 201 || response.statusCode == 204) {
        return "Downloading " + gameId + "...";
    } else if (response.statusCode == 404) {
        return "Replay " + gameId + " not found";
    } else if (response.statusCode == 409) {
        return "Download conflict";
    } else {
        return "Failed (" + std::to_string(response.statusCode) + ")";
    }
}

std::string WatchReplay(const LcuCredentials& creds, const std::string& gameId) {
    if (!creds.valid) return "League Client not running";
    ReplayMetadata metadata = GetReplayMetadata(creds, gameId);
    if (!metadata.valid) return "GameID " + gameId + " not found";
    std::string state = metadata.state;
    std::transform(state.begin(), state.end(), state.begin(), ::tolower);
    if (state == "download") return "Download the replay first";
    if (state == "incompatible") return "Replay is from a different patch";
    if (state != "watch") return "Unknown state: " + metadata.state;

    std::string endpoint = "/lol-replays/v1/rofls/" + gameId + "/watch";
    ApiResponse response = MakeLcuPost(creds, endpoint, "{}");
    if (response.statusCode == 200 || response.statusCode == 204) {
        return "Launching replay...";
    } else if (response.statusCode == 404) {
        return "GameID " + gameId + " not found";
    } else {
        return "Failed (" + std::to_string(response.statusCode) + ")";
    }
}

std::wstring GetReplayDirectory() {
    wchar_t documentsPath[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_MYDOCUMENTS, nullptr, 0, documentsPath))) {
        return std::wstring(documentsPath) + L"\\League of Legends\\Replays";
    }
    return L"";
}

std::vector<std::string> ListAvailableReplays() {
    std::vector<std::string> gameIds;
    std::wstring replayDir = GetReplayDirectory();
    if (replayDir.empty()) return gameIds;
    WIN32_FIND_DATAW findData;
    std::wstring searchPath = replayDir + L"\\*.rofl";
    HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
    if (hFind == INVALID_HANDLE_VALUE) return gameIds;
    do {
        std::wstring filename = findData.cFileName;
        if (filename.size() > 5) {
            filename = filename.substr(0, filename.size() - 5);
        }
        size_t dashPos = filename.find(L'-');
        if (dashPos != std::wstring::npos) {
            std::wstring gameIdW = filename.substr(dashPos + 1);
            std::string gameId(gameIdW.begin(), gameIdW.end());
            bool isNumeric = true;
            for (char c : gameId) {
                if (!isdigit(c)) { isNumeric = false; break; }
            }
            if (isNumeric && !gameId.empty()) {
                gameIds.push_back(gameId);
            }
        }
    } while (FindNextFileW(hFind, &findData));
    FindClose(hFind);
    return gameIds;
}
