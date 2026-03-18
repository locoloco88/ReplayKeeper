#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct LcuCredentials {
    std::string port;
    std::string authToken;
    bool valid = false;
};

struct ReplayMetadata {
    std::string state;
    std::string gameId;
    bool valid = false;
};

struct ApiResponse {
    bool success = false;
    int statusCode = 0;
    std::string body;
    std::string error;
};

LcuCredentials ConnectToLcu();
LcuCredentials ReadLockfile(const std::wstring& leaguePath);
ApiResponse MakeLcuGet(const LcuCredentials& creds, const std::string& endpoint);
ApiResponse MakeLcuPost(const LcuCredentials& creds, const std::string& endpoint, const std::string& jsonBody = "{}");
ReplayMetadata GetReplayMetadata(const LcuCredentials& creds, const std::string& gameId);
std::string DownloadReplay(const LcuCredentials& creds, const std::string& gameId);
std::string WatchReplay(const LcuCredentials& creds, const std::string& gameId);
std::wstring GetReplayDirectory();
std::vector<std::string> ListAvailableReplays();
