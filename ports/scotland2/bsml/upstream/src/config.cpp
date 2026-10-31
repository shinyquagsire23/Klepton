#include "config.hpp"
#include "logging.hpp"
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "git_info.h"


config_t config;

Configuration& get_config() {
    static Configuration config({MOD_ID, VERSION, GIT_COMMIT});
    config.Load();
    return config;
}

#define Save(identifier) doc.AddMember(#identifier, config.identifier, allocator)

void SaveConfig() {
    INFO("Saving Configuration...");
    rapidjson::Document& doc = get_config().config;
    doc.RemoveAllMembers();
    doc.SetObject();

    rapidjson::Document::AllocatorType& allocator = doc.GetAllocator();
    rapidjson::Value hiddenTabs;
    hiddenTabs.SetArray();
    for (auto& tab : config.hiddenTabs) {
        hiddenTabs.PushBack(rapidjson::Value(tab.data(), tab.size(), allocator), allocator);
    }

    doc.AddMember("hiddenTabs", hiddenTabs, allocator);

    get_config().Write();
    INFO("Saved Configuration!");
}

bool LoadConfig() {
    bool foundEverything = true;
    rapidjson::Document& doc = get_config().config;

    auto hiddenTabsItr = doc.FindMember("hiddenTabs");
    if (hiddenTabsItr != doc.MemberEnd()) {
        for (auto& tab : hiddenTabsItr->value.GetArray()) {
            config.hiddenTabs.push_back(tab.GetString());
        }
    } else
        foundEverything = false;

    if (foundEverything)
        INFO("Loaded Configuration!");
    return foundEverything;
}
