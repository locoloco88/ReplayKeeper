#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <thread>
#include <filesystem>
#include <fstream>

#include "gui.hpp"

namespace fs = std::filesystem;

std::wstring GetExeDirectoryW() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exePathStr(exePath);
    size_t lastSlash = exePathStr.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return exePathStr.substr(0, lastSlash);
    }
    return exePathStr;
}

std::pair<std::wstring, bool> GetLeagueInstallPath() {

    struct RegLocation {
        HKEY root;
        const wchar_t* subkey;
        const wchar_t* value;
    };

    RegLocation locations[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game league_of_legends.live", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Riot Games, Inc\\League of Legends", L"Location" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Riot Games, Inc\\League of Legends", L"Location" },
        { HKEY_CURRENT_USER, L"Software\\Riot Games\\RiotClientInstalls", L"league_of_legends.live" },
    };

    for (const auto& loc : locations) {
        HKEY hKey;
        if (RegOpenKeyExW(loc.root, loc.subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buffer[MAX_PATH];
            DWORD bufferSize = sizeof(buffer);
            DWORD type;

            if (RegQueryValueExW(hKey, loc.value, nullptr, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                std::wstring path = buffer;

                if (!path.empty() && path.back() == L'\\') {
                    path.pop_back();
                }
                return {path, true};
            }
            RegCloseKey(hKey);
        }
    }

    return {L"", false};
}

std::pair<std::wstring, bool> GetRiotClientPath() {
    struct RegLocation {
        HKEY root;
        const wchar_t* subkey;
        const wchar_t* value;
    };

    RegLocation locations[] = {
        { HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game riot.client", L"InstallLocation" },
        { HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Riot Game riot.client", L"InstallLocation" },
        { HKEY_CURRENT_USER, L"Software\\Riot Games\\RiotClientInstalls", L"rc_default" },
        { HKEY_CURRENT_USER, L"Software\\Riot Games\\RiotClientInstalls", L"rc_live" },
    };

    for (const auto& loc : locations) {
        HKEY hKey;
        if (RegOpenKeyExW(loc.root, loc.subkey, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
            wchar_t buffer[MAX_PATH];
            DWORD bufferSize = sizeof(buffer);
            DWORD type;

            if (RegQueryValueExW(hKey, loc.value, nullptr, &type, (LPBYTE)buffer, &bufferSize) == ERROR_SUCCESS) {
                RegCloseKey(hKey);
                std::wstring path = buffer;

                if (!path.empty() && path.back() == L'\\') {
                    path.pop_back();
                }

                if (path.size() > 4 && path.substr(path.size() - 4) == L".exe") {
                    size_t lastSlash = path.find_last_of(L"\\/");
                    if (lastSlash != std::wstring::npos) {
                        path = path.substr(0, lastSlash);
                    }
                }

                if (fs::exists(path + L"\\RiotClientServices.exe")) {
                    return {path, true};
                }
            }
            RegCloseKey(hKey);
        }
    }

    if (!g_guiState.leagueInstallPath.empty()) {
        fs::path leaguePath(g_guiState.leagueInstallPath);
        fs::path parentDir = leaguePath.parent_path();
        std::wstring candidate = parentDir.wstring() + L"\\Riot Client";
        if (fs::exists(candidate + L"\\RiotClientServices.exe")) {
            return {candidate, true};
        }
    }

    return {L"", false};
}

int KillAllRiotProcesses() {
    const std::vector<std::wstring> targets = {
        L"LeagueClient.exe",
        L"LeagueClientUx.exe",
        L"LeagueClientUxRender.exe",
        L"League of Legends.exe",
        L"RiotClientServices.exe",
        L"RiotClientUx.exe",
        L"RiotClientCrashHandler.exe",
    };

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    int killed = 0;
    if (Process32FirstW(snapshot, &pe32)) {
        do {
            for (const auto& target : targets) {
                if (_wcsicmp(pe32.szExeFile, target.c_str()) == 0) {
                    HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pe32.th32ProcessID);
                    if (hProcess) {
                        if (TerminateProcess(hProcess, 0)) {
                            killed++;
                        }
                        CloseHandle(hProcess);
                    }
                    break;
                }
            }
        } while (Process32NextW(snapshot, &pe32));
    }
    CloseHandle(snapshot);
    return killed;
}

bool LaunchRiotClient(const std::wstring& riotClientDir) {
    std::wstring exePath = riotClientDir + L"\\RiotClientServices.exe";
    if (!fs::exists(exePath)) return false;

    std::wstring cmdLine = L"\"" + exePath + L"\" --allow-multiple-clients";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL result = CreateProcessW(
        exePath.c_str(),
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        0,
        nullptr,
        riotClientDir.c_str(),
        &si,
        &pi
    );

    if (result) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        return true;
    }
    return false;
}

bool CopyFileWithOverwrite(const std::wstring& source, const std::wstring& destination) {
    if (!fs::exists(source)) {
        return false;
    }

    fs::path destDir = fs::path(destination).parent_path();
    if (!fs::exists(destDir)) {
        try {
            fs::create_directories(destDir);
        } catch (...) {}
    }

    return CopyFileW(source.c_str(), destination.c_str(), FALSE) != 0;
}

static std::string ExtractPatchFromFile(const std::wstring& filePath) {
    if (!fs::exists(filePath)) return "";

    std::ifstream inFile(filePath.c_str(), std::ios::binary);
    if (!inFile.is_open()) return "";

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    size_t branchPos = content.find("+branch.releases-");
    if (branchPos == std::string::npos) return "";

    size_t versionStart = branchPos;
    while (versionStart > 0) {
        versionStart--;
        if (!isdigit(content[versionStart]) && content[versionStart] != '.') {
            versionStart++;
            break;
        }
    }

    std::string versionPrefix = content.substr(versionStart, branchPos - versionStart);
    size_t firstDot = versionPrefix.find('.');
    if (firstDot == std::string::npos) return "";
    size_t secondDot = versionPrefix.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return "";

    return versionPrefix.substr(0, secondDot);
}

std::string ReadCurrentPatchVersion() {
    std::wstring exeDir = GetExeDirectoryW();
    return ExtractPatchFromFile(exeDir + L"\\compat-version-metadata.json");
}

std::string ReadGameFolderPatchVersion() {
    if (g_guiState.leagueInstallPath.empty()) return "";
    return ExtractPatchFromFile(g_guiState.leagueInstallPath + L"\\Game\\compat-version-metadata.json");
}

static bool PatchMetadataFile(const std::wstring& filePath, const std::string& major, const std::string& minor) {
    if (!fs::exists(filePath)) return false;

    std::ifstream inFile(filePath.c_str(), std::ios::binary);
    if (!inFile.is_open()) return false;

    std::string content((std::istreambuf_iterator<char>(inFile)),
                         std::istreambuf_iterator<char>());
    inFile.close();

    size_t branchPos = content.find("+branch.releases-");
    if (branchPos == std::string::npos) return false;

    size_t versionStart = branchPos;
    int dotsFound = 0;
    while (versionStart > 0) {
        versionStart--;
        if (content[versionStart] == '.') dotsFound++;
        if (!isdigit(content[versionStart]) && content[versionStart] != '.') {
            versionStart++;
            break;
        }
        if (dotsFound > 2) { versionStart++; break; }
    }

    std::string oldVersionPrefix = content.substr(versionStart, branchPos - versionStart);
    size_t firstDot = oldVersionPrefix.find('.');
    if (firstDot == std::string::npos) return false;
    size_t secondDot = oldVersionPrefix.find('.', firstDot + 1);
    if (secondDot == std::string::npos) return false;
    std::string buildNumber = oldVersionPrefix.substr(secondDot + 1);

    std::string newVersionPrefix = major + "." + minor + "." + buildNumber;

    size_t dashStart = branchPos + std::string("+branch.releases-").length();
    size_t dashEnd = content.find('.', dashStart);
    if (dashEnd == std::string::npos) return false;

    std::string newDashPart = major + "-" + minor;

    std::string patched = content.substr(0, versionStart)
                        + newVersionPrefix
                        + "+branch.releases-"
                        + newDashPart
                        + content.substr(dashEnd);

    if (patched == content) return false;

    std::ofstream outFile(filePath.c_str(), std::ios::binary | std::ios::trunc);
    if (!outFile.is_open()) return false;

    outFile.write(patched.data(), patched.size());
    outFile.close();
    return true;
}

bool ApplyPatchToLocalFile(const char* patchInput) {
    std::string patch = patchInput;
    if (patch.empty()) return false;

    size_t dotPos = patch.find('.');
    if (dotPos == std::string::npos) return false;

    std::string major = patch.substr(0, dotPos);
    std::string minor = patch.substr(dotPos + 1);
    if (major.empty() || minor.empty()) return false;

    std::wstring exeDir = GetExeDirectoryW();
    return PatchMetadataFile(exeDir + L"\\compat-version-metadata.json", major, minor);
}

void CopyAllFiles() {
    if (g_guiState.filesCopied.load()) return;

    std::wstring leaguePath;
    {
        std::lock_guard<std::mutex> lock(g_guiState.mtx);
        leaguePath = g_guiState.leagueInstallPath;
    }

    std::wstring exeDir = GetExeDirectoryW();
    std::wstring gamePath = leaguePath + L"\\Game";

    std::wstring systemYamlSource = exeDir + L"\\system.yaml";
    std::wstring systemYamlDest = leaguePath + L"\\system.yaml";

    std::wstring compatMetadataSource = exeDir + L"\\compat-version-metadata.json";
    std::wstring compatMetadataDest = gamePath + L"\\compat-version-metadata.json";

    bool success = true;
    success &= CopyFileWithOverwrite(systemYamlSource, systemYamlDest);
    success &= CopyFileWithOverwrite(compatMetadataSource, compatMetadataDest);

    if (success) {
        g_guiState.filesCopied.store(true);
        g_guiState.monitorEnabled.store(false);
    }
}

bool IsLeagueClientRunning(const std::vector<std::wstring>& targetProcesses) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;

    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);

    bool found = false;
    if (Process32FirstW(snapshot, &pe32)) {
        do {
            for (const auto& targetName : targetProcesses) {
                if (_wcsicmp(pe32.szExeFile, targetName.c_str()) == 0) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        } while (Process32NextW(snapshot, &pe32));
    }
    CloseHandle(snapshot);
    return found;
}

void MonitorForLeagueClient(const std::vector<std::wstring>& targetProcesses) {
    while (g_guiState.running.load()) {
        bool clientRunning = IsLeagueClientRunning(targetProcesses);
        g_guiState.leagueClientRunning.store(clientRunning);
        
        if (g_guiState.monitorEnabled.load() && !g_guiState.filesCopied.load() && clientRunning) {
            CopyAllFiles();
        }
        Sleep(100);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hInstance;
    (void)hPrevInstance;
    (void)lpCmdLine;
    (void)nCmdShow;

    std::vector<std::wstring> targetProcesses = {
        L"LeagueClient.exe",
        L"LeagueClientUx.exe",
        L"LeagueClientUxRender.exe"
    };

    auto [path, fromRegistry] = GetLeagueInstallPath();
    g_guiState.leagueInstallPath = path;
    g_guiState.pathFromRegistry = fromRegistry;

    auto [riotPath, riotFromRegistry] = GetRiotClientPath();
    g_guiState.riotClientPath = riotPath;
    g_guiState.riotClientPathFromRegistry = riotFromRegistry;

    {
        bool canMonitor = true;
        std::wstring exeDir = GetExeDirectoryW();

        if (g_guiState.leagueInstallPath.empty()) {
            canMonitor = false;
        }

        if (!g_guiState.leagueInstallPath.empty()) {
            std::wstring localSystemYaml = exeDir + L"\\system.yaml";
            std::wstring leagueSystemYaml = g_guiState.leagueInstallPath + L"\\system.yaml";

            if (!fs::exists(localSystemYaml)) {
                if (fs::exists(leagueSystemYaml)) {
                    try {
                        fs::copy_file(leagueSystemYaml, localSystemYaml, fs::copy_options::overwrite_existing);
                    } catch (const std::exception&) {}
                }
            }

            if (fs::exists(localSystemYaml)) {
                std::ifstream inFile(localSystemYaml.c_str(), std::ios::binary);
                if (inFile.is_open()) {
                    std::vector<char> buffer((std::istreambuf_iterator<char>(inFile)),
                                              std::istreambuf_iterator<char>());
                    inFile.close();

                    std::string content(buffer.begin(), buffer.end());

                    const std::string targetUrl = "https://sieve.services.riotcdn.net";
                    const std::string replacement = "EXPIREDREPLAY";

                    size_t pos = content.find(targetUrl);
                    if (pos != std::string::npos) {
                        content.replace(pos, targetUrl.length(), replacement);

                        std::ofstream outFile(localSystemYaml.c_str(), std::ios::binary | std::ios::trunc);
                        if (outFile.is_open()) {
                            outFile.write(content.data(), content.size());
                            outFile.close();
                        }
                    }
                }
            }

            std::wstring localCompatMetadata = exeDir + L"\\compat-version-metadata.json";
            std::wstring leagueCompatMetadata = g_guiState.leagueInstallPath + L"\\Game\\compat-version-metadata.json";

            if (!fs::exists(localCompatMetadata)) {
                if (fs::exists(leagueCompatMetadata)) {
                    try {
                        fs::copy_file(leagueCompatMetadata, localCompatMetadata, fs::copy_options::overwrite_existing);
                    } catch (const std::exception&) {}
                }
            }
        }
        
        g_guiState.gameFolderPatchVersion = ReadGameFolderPatchVersion();
        g_guiState.currentPatchVersion = ReadCurrentPatchVersion();

        if (canMonitor) {
            if (IsLeagueClientRunning(targetProcesses)) {
                canMonitor = false;
            }
        }

        g_guiState.monitorEnabled.store(canMonitor);
    }

    if (!InitializeGui(L"Replay Keeper", 900, 730)) {
        MessageBoxW(nullptr, L"Failed to initialize GUI", L"Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    std::thread monitorThread(MonitorForLeagueClient, std::ref(targetProcesses));

    RunGuiLoop();

    g_guiState.running.store(false);
    monitorThread.join();
    CleanupGui();

    return 0;
}