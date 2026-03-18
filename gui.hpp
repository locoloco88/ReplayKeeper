#pragma once

#include <windows.h>
#include <d3d11.h>
#include <string>
#include <atomic>
#include <mutex>

struct GuiState {
    std::mutex mtx;
    std::atomic<bool> monitorEnabled{true};
    std::atomic<bool> running{true};
    std::atomic<bool> filesCopied{false};
    std::atomic<bool> leagueClientRunning{false};
    std::wstring leagueInstallPath;
    bool pathFromRegistry = false;
    std::wstring riotClientPath;
    bool riotClientPathFromRegistry = false;
    std::string consoleLogs;
    char gameIdInput[64] = "";

    std::string notificationMessage;
    float notificationEndTime = 0.0f;
    bool notificationIsSuccess = false;

    std::string downloadingGameId;
    float lastDownloadCheckTime = 0.0f;

    char patchVersionInput[32] = "";
    std::string currentPatchVersion;
    std::string gameFolderPatchVersion;
};

extern GuiState g_guiState;

bool InitializeGui(const wchar_t* title, int width, int height);
void RunGuiLoop();
void CleanupGui();
