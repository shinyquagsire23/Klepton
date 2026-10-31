#include "CustomFileWatcher.hpp"
#include "Log.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Time.hpp"
#include <sys/stat.h>
#include <functional>

DEFINE_TYPE(AirbudsSearch, CustomFileWatcher);

using namespace UnityEngine;
using namespace AirbudsSearch;

void CustomFileWatcher::ctor() {
    lastUpdateTime_ = std::chrono::steady_clock::now();
    pollingInterval_ = std::chrono::seconds(5);
    lastWriteTimes_ = std::make_unique<std::unordered_map<std::filesystem::path, std::filesystem::file_time_type>>();
    onFilesChanged_ = [](const std::vector<std::filesystem::path>& paths) {
        // Empty
    };
}

int64_t file_time_to_int(std::filesystem::file_time_type ftime) {
    // Convert to system_clock time_point
    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - decltype(ftime)::clock::now() + std::chrono::system_clock::now()
    );

    // Get duration since epoch as integer (e.g., seconds)
    auto duration_since_epoch = sctp.time_since_epoch();

    // Convert to integer count of seconds (change to milliseconds if you want)
    return std::chrono::duration_cast<std::chrono::seconds>(duration_since_epoch).count();
}

void CustomFileWatcher::Update() {

    // Check if it's time to check the files
    const std::chrono::time_point<std::chrono::steady_clock> now = std::chrono::steady_clock::now();
    const auto elapsed = now - lastUpdateTime_;
    if (elapsed < pollingInterval_) {
        return;
    }
    lastUpdateTime_ = now;

    std::vector<std::filesystem::path> modifiedPaths;

    for (const std::filesystem::path& path : filePaths_) {

        // Get the last modified time of the file
        const auto lastWriteTime = std::filesystem::last_write_time(path);

        if (!lastWriteTimes_->contains(path)) {
            (*lastWriteTimes_)[path] = lastWriteTime;
            continue;
        }

        // Check if the file's last modified time is more recent than it was since the last time we checked it
        const std::filesystem::file_time_type previousLastWriteTime = lastWriteTimes_->at(path.string());
        if (lastWriteTime > previousLastWriteTime) {
            (*lastWriteTimes_)[path] = lastWriteTime;
            modifiedPaths.push_back(path);
            continue;
        }
    }

    // Execute callback
    if (!modifiedPaths.empty()) {
        onFilesChanged_(modifiedPaths);
    }
}

/*void CustomFileWatcher::Reload() {
    if (!host) {
        ERROR("Host object not set, can't hot reload!");
        return;
    }
    std::string content = readfile(filePath);
    int newHash = std::hash<std::string>()(content);
    if (newHash != fileHash) {
        fileHash = newHash;
        auto t = get_transform();
        int childCount = t->get_childCount();
        for (int i = 0; i < childCount; i++)
            Object::DestroyImmediate(t->GetChild(0)->get_gameObject());

        BSML::parse_and_construct(content, t, host);
    } else {
        INFO("Content hash was not different, not reloading UI");
    }
}*/
