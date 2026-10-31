#include "BSML/Components/HotReloadFileWatcher.hpp"
#include "logging.hpp"
#include "BSML.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Time.hpp"
#include <sys/stat.h>
#include <functional>

DEFINE_TYPE(BSML, HotReloadFileWatcher);

using namespace UnityEngine;

namespace BSML {
    void HotReloadFileWatcher::ctor() {
        runCheck = true;
        checkInterval = 10.0f;
        lastFileEdit = 0;
        // trigger a reload instantly, if you give a filepath in didactivate it immediately reloads
        timeSinceLastCheck = checkInterval;
    }

    void HotReloadFileWatcher::Update() {
        if (!runCheck || filePath.empty()) {
            return;
        }

        timeSinceLastCheck += Time::get_deltaTime();

        if (timeSinceLastCheck >= checkInterval) {
            timeSinceLastCheck = 0;
            if (!fileexists(filePath)) {
                return;
            }

            struct stat fileStat = {0};
            if (stat(filePath.c_str(), &fileStat) == 0) {
                int editTime = fileStat.st_mtime;
                bool doReload = editTime > lastFileEdit;
                if (doReload) {
                    lastFileEdit = editTime;
                    Reload();
                }
            }
        }
    }

    void HotReloadFileWatcher::Reload() {
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
    }
}