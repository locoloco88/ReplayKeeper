#include "gui.hpp"
#include "lcu.hpp"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <filesystem>
#include <fstream>
#include <vector>
#include <algorithm>
#include <shlobj.h>

namespace fs = std::filesystem;

static std::string WideToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return "";
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &result[0], size, nullptr, nullptr);
    return result;
}

std::wstring GetExeDirectoryW();
std::string ReadCurrentPatchVersion();
std::string ReadGameFolderPatchVersion();
bool ApplyPatchToLocalFile(const char* patchInput);
int KillAllRiotProcesses();
bool LaunchRiotClient(const std::wstring& riotClientDir);

static void TryCopyStartupFiles(const std::wstring& leaguePath) {
    if (leaguePath.empty()) return;
    std::wstring exeDir = GetExeDirectoryW();

    std::wstring localSystemYaml = exeDir + L"\\system.yaml";
    std::wstring leagueSystemYaml = leaguePath + L"\\system.yaml";
    if (!fs::exists(localSystemYaml) && fs::exists(leagueSystemYaml)) {
        try { fs::copy_file(leagueSystemYaml, localSystemYaml, fs::copy_options::overwrite_existing); } catch (...) {}
    }
    if (fs::exists(localSystemYaml)) {
        std::ifstream inFile(localSystemYaml.c_str(), std::ios::binary);
        if (inFile.is_open()) {
            std::string content((std::istreambuf_iterator<char>(inFile)), std::istreambuf_iterator<char>());
            inFile.close();
            const std::string targetUrl = "https://sieve.services.riotcdn.net";
            size_t pos = content.find(targetUrl);
            if (pos != std::string::npos) {
                content.replace(pos, targetUrl.length(), "EXPIREDREPLAY");
                std::ofstream outFile(localSystemYaml.c_str(), std::ios::binary | std::ios::trunc);
                if (outFile.is_open()) { outFile.write(content.data(), content.size()); outFile.close(); }
            }
        }
    }

    std::wstring localCompat = exeDir + L"\\compat-version-metadata.json";
    std::wstring leagueCompat = leaguePath + L"\\Game\\compat-version-metadata.json";
    if (!fs::exists(localCompat) && fs::exists(leagueCompat)) {
        try { fs::copy_file(leagueCompat, localCompat, fs::copy_options::overwrite_existing); } catch (...) {}
    }

    g_guiState.currentPatchVersion = ReadCurrentPatchVersion();
    g_guiState.gameFolderPatchVersion = ReadGameFolderPatchVersion();
}

static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
static HWND g_hwnd = nullptr;

GuiState g_guiState;

static bool CreateDeviceD3D(HWND hWnd);
static void CleanupDeviceD3D();
static void CreateRenderTarget();
static void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

bool InitializeGui(const wchar_t* title, int width, int height) {
    WNDCLASSEXW wc = {
        sizeof(wc),
        CS_CLASSDC,
        WndProc,
        0L, 0L,
        GetModuleHandle(nullptr),
        nullptr, nullptr, nullptr, nullptr,
        L"SleeperGuiClass",
        nullptr
    };
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(
        0,
        wc.lpszClassName, 
        title,
        WS_OVERLAPPEDWINDOW,
        100, 100, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!CreateDeviceD3D(g_hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return false;
    }

    ShowWindow(g_hwnd, SW_SHOWDEFAULT);
    UpdateWindow(g_hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    ImGui::StyleColorsDark();

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.WindowPadding = ImVec2(15, 15);
    style.FramePadding = ImVec2(8, 4);
    style.ItemSpacing = ImVec2(10, 8);

    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    return true;
}

void RunGuiLoop() {
    ImVec4 clearColor = ImVec4(0.1f, 0.1f, 0.12f, 1.0f);
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));

    while (g_guiState.running && msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        float scaleX = displaySize.x / 900.0f;
        float scaleY = displaySize.y / 550.0f;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        if (scale < 0.5f) scale = 0.5f;
        if (scale > 2.0f) scale = 2.0f;

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 8.0f * scale;
        style.FrameRounding = 4.0f * scale;
        style.GrabRounding = 4.0f * scale;
        style.WindowPadding = ImVec2(15 * scale, 15 * scale);
        style.FramePadding = ImVec2(8 * scale, 4 * scale);
        style.ItemSpacing = ImVec2(10 * scale, 8 * scale);
        style.IndentSpacing = 21.0f * scale;

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Replay Keeper", nullptr, 
            ImGuiWindowFlags_NoResize | 
            ImGuiWindowFlags_NoMove | 
            ImGuiWindowFlags_NoCollapse |
            ImGuiWindowFlags_NoTitleBar);

        static float programTime = 0.0f;
        programTime += ImGui::GetIO().DeltaTime;

        float windowWidth = ImGui::GetContentRegionAvail().x;
        float leftColumnWidth = windowWidth * 0.55f;
        float rightColumnWidth = windowWidth * 0.42f;

        ImGui::BeginChild("LeftColumn", ImVec2(leftColumnWidth, 0), false);

        ImGui::PushFont(nullptr);
        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Replay Keeper");
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();

        if (!g_guiState.leagueInstallPath.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "League Detected");

            std::string pathStr = WideToUtf8(g_guiState.leagueInstallPath);
            ImGui::Text("Path: %s", pathStr.c_str());
            
            if (g_guiState.pathFromRegistry) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "(Auto-detected)");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "(Manually selected)");
            }

            if (ImGui::Button("Change Path...", ImVec2(120 * scale, 0))) {
                BROWSEINFOW bi = {0};
                bi.lpszTitle = L"Select League of Legends folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t path[MAX_PATH];
                    if (SHGetPathFromIDListW(pidl, path)) {
                        std::lock_guard<std::mutex> lock(g_guiState.mtx);
                        g_guiState.leagueInstallPath = path;
                        g_guiState.pathFromRegistry = false;
                    }
                    CoTaskMemFree(pidl);
                    TryCopyStartupFiles(g_guiState.leagueInstallPath);
                }
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "League Not Found");
            ImGui::TextWrapped("Select your League folder manually.");
            
            if (ImGui::Button("Browse...", ImVec2(100 * scale, 0))) {
                BROWSEINFOW bi = {0};
                bi.lpszTitle = L"Select League of Legends folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t path[MAX_PATH];
                    if (SHGetPathFromIDListW(pidl, path)) {
                        std::lock_guard<std::mutex> lock(g_guiState.mtx);
                        g_guiState.leagueInstallPath = path;
                        g_guiState.pathFromRegistry = false;
                    }
                    CoTaskMemFree(pidl);
                    TryCopyStartupFiles(g_guiState.leagueInstallPath);
                }
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Riot Client");
        ImGui::Separator();
        ImGui::Spacing();

        if (!g_guiState.riotClientPath.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Riot Client Detected");
            std::string riotPathStr = WideToUtf8(g_guiState.riotClientPath);
            ImGui::Text("Path: %s", riotPathStr.c_str());

            if (g_guiState.riotClientPathFromRegistry) {
                ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "(Auto-detected)");
            } else {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.4f, 1.0f), "(Manually selected)");
            }

            if (ImGui::Button("Change Riot Client Path", ImVec2(-1, 0))) {
                BROWSEINFOW bi = {0};
                bi.lpszTitle = L"Select Riot Client folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t selPath[MAX_PATH];
                    if (SHGetPathFromIDListW(pidl, selPath)) {
                        g_guiState.riotClientPath = selPath;
                        g_guiState.riotClientPathFromRegistry = false;
                    }
                    CoTaskMemFree(pidl);
                }
            }
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Riot Client Not Found");
            if (ImGui::Button("Browse...", ImVec2(100 * scale, 0))) {
                BROWSEINFOW bi = {0};
                bi.lpszTitle = L"Select Riot Client folder";
                bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
                LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
                if (pidl) {
                    wchar_t selPath[MAX_PATH];
                    if (SHGetPathFromIDListW(pidl, selPath)) {
                        g_guiState.riotClientPath = selPath;
                        g_guiState.riotClientPathFromRegistry = false;
                    }
                    CoTaskMemFree(pidl);
                }
            }
        }

        ImGui::Spacing();

        if (ImGui::Button("Close Riot Processes", ImVec2(-1, 28 * scale))) {
            int killed = KillAllRiotProcesses();
            if (killed > 0) {
                g_guiState.notificationMessage = "Killed " + std::to_string(killed) + " process(es)";
                g_guiState.notificationEndTime = programTime + 3.0f;
                g_guiState.notificationIsSuccess = true;
                g_guiState.filesCopied.store(false);
            } else {
                g_guiState.notificationMessage = "No processes found";
                g_guiState.notificationEndTime = programTime + 3.0f;
                g_guiState.notificationIsSuccess = false;
            }
        }

        if (ImGui::Button("Start Riot Client", ImVec2(-1, 28 * scale))) {
            if (g_guiState.riotClientPath.empty()) {
                g_guiState.notificationMessage = "Select Riot Client folder first";
                g_guiState.notificationEndTime = programTime + 3.0f;
                g_guiState.notificationIsSuccess = false;
            } else if (LaunchRiotClient(g_guiState.riotClientPath)) {
                g_guiState.notificationMessage = "Riot Client started";
                g_guiState.notificationEndTime = programTime + 3.0f;
                g_guiState.notificationIsSuccess = true;
            } else {
                g_guiState.notificationMessage = "Failed to start Riot Client";
                g_guiState.notificationEndTime = programTime + 3.0f;
                g_guiState.notificationIsSuccess = false;
            }
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        bool monitorEnabled = g_guiState.monitorEnabled.load();
        if (ImGui::Checkbox("Auto-Replace Files", &monitorEnabled)) {
            if (monitorEnabled) {
                if (g_guiState.leagueInstallPath.empty()) {
                    monitorEnabled = false;
                    g_guiState.monitorEnabled.store(false);
                    g_guiState.notificationMessage = "Select League folder first";
                    g_guiState.notificationEndTime = programTime + 3.0f;
                    g_guiState.notificationIsSuccess = false;
                } else {
                    std::wstring exeDir = GetExeDirectoryW();

                    if (g_guiState.leagueClientRunning.load()) {
                        monitorEnabled = false;
                        g_guiState.monitorEnabled.store(false);
                        g_guiState.notificationMessage = "Close League first";
                        g_guiState.notificationEndTime = programTime + 3.0f;
                        g_guiState.notificationIsSuccess = false;
                    } else {
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

                        {
                            wchar_t exePath[MAX_PATH];
                            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                            std::wstring exeDir(exePath);
                            size_t lastSlash = exeDir.find_last_of(L"\\/");
                            if (lastSlash != std::wstring::npos) exeDir = exeDir.substr(0, lastSlash);

                            std::wstring localCompat = exeDir + L"\\compat-version-metadata.json";
                            std::wstring leagueCompat = g_guiState.leagueInstallPath + L"\\Game\\compat-version-metadata.json";
                            if (!fs::exists(localCompat)) {
                                if (fs::exists(leagueCompat)) {
                                    try {
                                        fs::copy_file(leagueCompat, localCompat, fs::copy_options::overwrite_existing);
                                    } catch (const std::exception&) {}
                                }
                            }

                            g_guiState.currentPatchVersion = ReadCurrentPatchVersion();
                            g_guiState.gameFolderPatchVersion = ReadGameFolderPatchVersion();
                        }
                        
                        g_guiState.monitorEnabled.store(true);
                    }
                }
            } else {
                g_guiState.monitorEnabled.store(false);
            }
        }
        
        ImGui::Spacing();

        ImGui::Text("Status:");
        ImGui::Indent();
        
        if (g_guiState.leagueClientRunning.load()) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "* League Client: Running");
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "* League Client: Not Running");
        }

        if (g_guiState.filesCopied.load()) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "* Files: Copied Successfully");
        } else if (monitorEnabled) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "* Files: Waiting for League Client...");
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "* Files: Monitoring Disabled");
        }
        
        ImGui::Unindent();

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Files replaced on game launch:");
        ImGui::Indent();
        ImGui::BulletText("system.yaml");
        ImGui::BulletText("compat-version-metadata.json");
        ImGui::Unindent();

        ImGui::Spacing();

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Patch Version Override");
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::Text("Enter patch (example 16.1):");
        ImGui::SetNextItemWidth(120 * scale);
        ImGui::InputText("##PatchVersion", g_guiState.patchVersionInput, sizeof(g_guiState.patchVersionInput));
        ImGui::SameLine();
        if (ImGui::Button("Apply", ImVec2(60 * scale, 0))) {
            if (strlen(g_guiState.patchVersionInput) > 0) {
                if (ApplyPatchToLocalFile(g_guiState.patchVersionInput)) {
                    g_guiState.currentPatchVersion = ReadCurrentPatchVersion();
                    g_guiState.notificationMessage = "Patch applied: " + g_guiState.currentPatchVersion;
                    g_guiState.notificationEndTime = programTime + 3.0f;
                    g_guiState.notificationIsSuccess = true;
                } else {
                    g_guiState.notificationMessage = "Failed to apply patch (check format)";
                    g_guiState.notificationEndTime = programTime + 3.0f;
                    g_guiState.notificationIsSuccess = false;
                }
            }
        }
        if (!g_guiState.gameFolderPatchVersion.empty()) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "Current: %s", g_guiState.gameFolderPatchVersion.c_str());
            if (!g_guiState.currentPatchVersion.empty() && g_guiState.currentPatchVersion != g_guiState.gameFolderPatchVersion) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.4f, 1.0f), "-> Override: %s", g_guiState.currentPatchVersion.c_str());
            }
        } else {
            ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f), "No compat-version-metadata.json found");
        }

        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("RightColumn", ImVec2(rightColumnWidth, 0), true);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Replay Downloader");
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("GameID:");
        ImGui::SetNextItemWidth(-1);
        ImGui::InputText("##GameID", g_guiState.gameIdInput, sizeof(g_guiState.gameIdInput));
        
        ImGui::Spacing();

        static LcuCredentials cachedCreds;

        if (ImGui::Button("Download Replay", ImVec2(-1, 28 * scale))) {
            std::string gameId = g_guiState.gameIdInput;
            if (gameId.empty()) {
                g_guiState.consoleLogs += "> Enter a GameID first\n";
            } else {
                cachedCreds = ConnectToLcu();
                std::string result = DownloadReplay(cachedCreds, gameId);
                g_guiState.consoleLogs += "> " + result + "\n";
                
                if (result.find("Downloading") != std::string::npos) {
                    g_guiState.downloadingGameId = gameId;
                    g_guiState.lastDownloadCheckTime = programTime;
                }
            }
        }

        if (ImGui::Button("Watch Replay", ImVec2(-1, 28 * scale))) {
            std::string gameId = g_guiState.gameIdInput;
            if (gameId.empty()) {
                g_guiState.consoleLogs += "> Enter a GameID first\n";
            } else {
                cachedCreds = ConnectToLcu();
                std::string result = WatchReplay(cachedCreds, gameId);
                g_guiState.consoleLogs += "> " + result + "\n";
            }
        }

        if (!g_guiState.downloadingGameId.empty() && programTime - g_guiState.lastDownloadCheckTime >= 2.0f) {
            g_guiState.lastDownloadCheckTime = programTime;
            if (!cachedCreds.valid) {
                cachedCreds = ConnectToLcu();
            }
            if (cachedCreds.valid) {
                ReplayMetadata metadata = GetReplayMetadata(cachedCreds, g_guiState.downloadingGameId);
                if (metadata.valid) {
                    std::string state = metadata.state;
                    std::transform(state.begin(), state.end(), state.begin(), ::tolower);
                    if (state == "watch") {
                        g_guiState.consoleLogs += "> Download complete: " + g_guiState.downloadingGameId + "\n";
                        g_guiState.notificationMessage = "Download complete!";
                        g_guiState.notificationEndTime = programTime + 3.0f;
                        g_guiState.notificationIsSuccess = true;
                        g_guiState.downloadingGameId.clear();
                    } else if (state != "downloading" && state != "download") {
                        g_guiState.consoleLogs += "> Download status: " + metadata.state + "\n";
                        g_guiState.downloadingGameId.clear();
                    }
                }
            }
        }

        ImGui::Spacing();

        static bool showReplayBrowser = false;
        static std::vector<std::string> availableReplays;
        static char searchFilter[64] = "";
        static int selectedReplayIdx = -1;

        if (ImGui::Button("Browse Replays...", ImVec2(-1, 28 * scale))) {
            availableReplays = ListAvailableReplays();
            searchFilter[0] = '\0';
            selectedReplayIdx = -1;
            showReplayBrowser = true;
            ImGui::OpenPopup("Replay Browser");
        }

        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(350, 400), ImGuiCond_Appearing);
        
        if (ImGui::BeginPopupModal("Replay Browser", &showReplayBrowser, ImGuiWindowFlags_NoResize)) {
            ImGui::Text("Search:");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##Search", searchFilter, sizeof(searchFilter));
            
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::Text("Available Replays (%d):", (int)availableReplays.size());
            
            float listHeight = ImGui::GetContentRegionAvail().y - 40;
            ImGui::BeginChild("ReplayList", ImVec2(-1, listHeight), true);
            
            std::string filterStr = searchFilter;
            for (int i = 0; i < (int)availableReplays.size(); i++) {
                if (!filterStr.empty()) {
                    if (availableReplays[i].find(filterStr) == std::string::npos) {
                        continue;
                    }
                }
                
                bool isSelected = (selectedReplayIdx == i);
                if (ImGui::Selectable(availableReplays[i].c_str(), isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
                    selectedReplayIdx = i;
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        strncpy(g_guiState.gameIdInput, availableReplays[i].c_str(), sizeof(g_guiState.gameIdInput) - 1);
                        g_guiState.gameIdInput[sizeof(g_guiState.gameIdInput) - 1] = '\0';
                        showReplayBrowser = false;
                        ImGui::CloseCurrentPopup();
                    }
                }
            }
            ImGui::EndChild();
            
            ImGui::Spacing();

            if (ImGui::Button("Select", ImVec2(100, 0))) {
                if (selectedReplayIdx >= 0 && selectedReplayIdx < (int)availableReplays.size()) {
                    strncpy(g_guiState.gameIdInput, availableReplays[selectedReplayIdx].c_str(), sizeof(g_guiState.gameIdInput) - 1);
                    g_guiState.gameIdInput[sizeof(g_guiState.gameIdInput) - 1] = '\0';
                }
                showReplayBrowser = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                showReplayBrowser = false;
                ImGui::CloseCurrentPopup();
            }
            
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::Text("Console:");
        
        float consoleHeight = ImGui::GetContentRegionAvail().y - 10 * scale;
        ImGui::BeginChild("ConsoleLog", ImVec2(-1, consoleHeight), true, 
            ImGuiWindowFlags_HorizontalScrollbar);
        
        ImGui::TextUnformatted(g_guiState.consoleLogs.c_str());

        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 20) {
            ImGui::SetScrollHereY(1.0f);
        }
        
        ImGui::EndChild();
        ImGui::EndChild();
        ImGui::End();

        if (!g_guiState.notificationMessage.empty() && programTime < g_guiState.notificationEndTime) {
            ImVec2 displaySize = ImGui::GetIO().DisplaySize;
            float padding = 10.0f;
            float textPadding = 12.0f;

            ImVec2 textSize = ImGui::CalcTextSize(g_guiState.notificationMessage.c_str());
            float notifyWidth = textSize.x + textPadding * 2;
            float notifyHeight = textSize.y + textPadding * 2;
            
            ImVec2 boxMin(displaySize.x - notifyWidth - padding, padding);
            ImVec2 boxMax(displaySize.x - padding, padding + notifyHeight);
            
            ImDrawList* drawList = ImGui::GetForegroundDrawList();

            ImU32 bgColor, borderColor, textColor;
            if (g_guiState.notificationIsSuccess) {
                bgColor = IM_COL32(30, 50, 30, 245);
                borderColor = IM_COL32(100, 255, 100, 255);
                textColor = IM_COL32(120, 255, 120, 255);
            } else {
                bgColor = IM_COL32(50, 30, 30, 245);
                borderColor = IM_COL32(255, 100, 100, 255);
                textColor = IM_COL32(255, 120, 120, 255);
            }

            drawList->AddRectFilled(boxMin, boxMax, bgColor, 6.0f);
            drawList->AddRect(boxMin, boxMax, borderColor, 6.0f, 0, 2.0f);

            ImVec2 textPos(boxMin.x + textPadding, boxMin.y + textPadding);
            drawList->AddText(textPos, textColor, g_guiState.notificationMessage.c_str());
        } else if (programTime >= g_guiState.notificationEndTime) {
            g_guiState.notificationMessage.clear();
        }

        ImGui::Render();
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, (float*)&clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }
}

void CleanupGui() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(g_hwnd);
    UnregisterClassW(L"SleeperGuiClass", GetModuleHandle(nullptr));
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    
    HRESULT res = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags,
        featureLevelArray, 2, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext
    );
    
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

static void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        g_guiState.running.store(false);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
