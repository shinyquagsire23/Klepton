#pragma once

#include <string>
#include <vector>

#include "UnityEngine/MonoBehaviour.hpp"
#include "custom-types/shared/macros.hpp"

DECLARE_CLASS_CODEGEN(AirbudsSearch, CustomFileWatcher, UnityEngine::MonoBehaviour) {

    DECLARE_CTOR(ctor);

    DECLARE_INSTANCE_METHOD(void, Update);

    public:
    std::chrono::time_point<std::chrono::steady_clock> lastUpdateTime_;
    std::chrono::seconds pollingInterval_;
    std::vector<std::filesystem::path> filePaths_;
    std::function<void(const std::vector<std::filesystem::path> &paths)> onFilesChanged_;

    private:
    std::unique_ptr<std::unordered_map<std::filesystem::path, std::filesystem::file_time_type>> lastWriteTimes_;
};