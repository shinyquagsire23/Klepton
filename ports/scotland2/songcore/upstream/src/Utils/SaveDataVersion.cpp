#include "Utils/SaveDataVersion.hpp"
#include "logging.hpp"
#include <regex>
#include <fstream>

namespace SongCore {
    Version Version::noVersion(0, 0, 0);

    Version GetVersion(std::string const& data) {
        if (data.empty()) return Version::noVersion;

        std::string truncatedText(data.substr(0, 50));
        static const std::regex versionRegex (R"("_?version"\s*:\s*"[0-9]+\.[0-9]+\.?[0-9]?")", std::regex_constants::optimize);
        std::smatch matches;
        if (std::regex_search(truncatedText, matches, versionRegex)) {
            if (!matches.empty()) {
                auto version = matches[0].str();
                version = version.substr(0, version.length()-1);
                version = version.substr(version.find_last_of('\"')+1, version.length());
                try {
                    // horrible I know, unless I find a way to split the version back into 3 nums there's not a lot to do
                    return Version(System::Version::New_ctor(version));
                } catch(const std::runtime_error& e) {
                    ERROR("BeatmapSaveDataHelpers_GetVersion Invalid version: '{}'!", version);
                }
            }
        }

        return Version::noVersion;
    }

    Version VersionFromFilePath(std::filesystem::path const& filePath) {
        if (!std::filesystem::exists(filePath)) return Version::noVersion;
        std::ifstream file(filePath, std::ios::ate);
        auto len = std::min<std::size_t>(file.tellg(), 50);
        file.seekg(0, std::ios::beg);
        std::string startOfFile(len, 0);
        file.read(startOfFile.data(), len);
        return GetVersion(startOfFile);
    }

    Version VersionFromFileData(std::string const& data) {
        return GetVersion(data);
    }
}
