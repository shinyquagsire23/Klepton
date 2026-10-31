#include "Configuration.hpp"

namespace AirbudsSearch {

// Loads the config from disk using our modInfo, then returns it for use
// other config tools such as config-utils don't use this config, so it can be
// removed if those are in use
Configuration& getConfig() {
    static Configuration config(modInfo);
    return config;
}

std::filesystem::path getDataDirectory() {
    static const std::string dataDirectoryString = getDataDir(modInfo);
    static const std::filesystem::path dataDirectory {dataDirectoryString};
    if (!std::filesystem::exists(dataDirectory)) {
        std::filesystem::create_directories(dataDirectory);
    }
    return dataDirectory;
}

bool hasAirbudsRefreshToken() {
    return !getAirbudsRefreshToken().empty();
}

std::string getAirbudsRefreshToken() {
    const auto& config = getConfig().config;
    if (!config.HasMember("airbuds")) {
        return "";
    }
    const auto& airbuds = config["airbuds"];
    if (!airbuds.IsObject() || !airbuds.HasMember("refreshToken") || !airbuds["refreshToken"].IsString()) {
        return "";
    }
    return airbuds["refreshToken"].GetString();
}

void setAirbudsRefreshToken(std::string_view token) {
    auto& config = getConfig().config;
    if (!config.HasMember("airbuds") || !config["airbuds"].IsObject()) {
        rapidjson::Value airbudsJson;
        airbudsJson.SetObject();
        config.AddMember("airbuds", airbudsJson, config.GetAllocator());
    }
    auto& airbuds = config["airbuds"];
    const std::string tokenString(token);
    if (airbuds.HasMember("refreshToken")) {
        airbuds["refreshToken"].SetString(tokenString.c_str(), config.GetAllocator());
    } else {
        airbuds.AddMember("refreshToken", rapidjson::Value(tokenString.c_str(), config.GetAllocator()), config.GetAllocator());
    }
    getConfig().Write();
}

void clearAirbudsRefreshToken() {
    setAirbudsRefreshToken("");
}

}
