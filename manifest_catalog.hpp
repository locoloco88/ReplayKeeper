#pragma once

#include <string>
#include <vector>

namespace manifest_catalog {

struct PatchEntry {
    std::string version;
    std::string displayVersion;
    std::string fileName;
    std::string downloadUrl;
};

struct ServerListResult {
    bool success = false;
    std::string error;
    std::vector<std::string> servers;
};

struct PatchListResult {
    bool success = false;
    std::string error;
    std::vector<PatchEntry> patches;
};

struct ManifestLinkResult {
    bool success = false;
    std::string error;
    std::string manifestUrl;
};

ServerListResult FetchLeagueServers();
PatchListResult FetchLeaguePatches(const std::string& server);
ManifestLinkResult FetchManifestLink(const PatchEntry& patch);

std::vector<std::string> ParseServerDirectoryForTest(const std::string& json);
std::vector<PatchEntry> ParsePatchDirectoryForTest(const std::string& json);

} // namespace manifest_catalog
