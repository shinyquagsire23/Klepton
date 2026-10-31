#include "UI/IconCache.hpp"
#include "assets.hpp"
#include "logging.hpp"
#include "bsml/shared/Helpers/utilities.hpp"
#include <fstream>
#include <ios>

DEFINE_TYPE(SongCore::UI, IconCache);

#define GetIconImpl(fieldname, assetname) do { \
        if (!fieldname) fieldname = BSML::Utilities::LoadSpriteRaw(Assets::Resources::assetname##_png); \
        return fieldname; \
} while(0)

#define DestroyIcon(fieldname) do { \
    if (fieldname && fieldname->m_CachedPtr.m_value) \
        UnityEngine::Object::Destroy(fieldname); \
    fieldname = nullptr; \
} while(0)
namespace SongCore::UI {
    IconCache* IconCache::_instance = nullptr;

    void IconCache::ctor() {
        INVOKE_CTOR();
        _pathIcons = IconDict::New_ctor();
        if (!_instance) _instance = this;
    }

    void IconCache::Dispose() {
        if (_instance == this) _instance = nullptr;

        auto enumerator = _pathIcons->GetEnumerator();
        while (enumerator.MoveNext()) {
            auto [path, icon] = enumerator.Current;
            if (icon && icon->m_CachedPtr.m_value) UnityEngine::Object::Destroy(icon);
        }
        enumerator.Dispose();

        _pathIcons->Clear();
        _lastUsedIcons.clear();

        DestroyIcon(_colorsIcon);
        DestroyIcon(_environmentIcon);
        DestroyIcon(_extraDiffsIcon);
        DestroyIcon(_folderIcon);
        DestroyIcon(_haveReqIcon);
        DestroyIcon(_infoIcon);
        DestroyIcon(_oneSaberIcon);
        DestroyIcon(_missingReqIcon);
        DestroyIcon(_standardIcon);
        DestroyIcon(_warningIcon);
        DestroyIcon(_haveSuggestionIcon);
        DestroyIcon(_missingSuggestionIcon);
        DestroyIcon(_deleteIcon);
    }

    UnityEngine::Sprite* IconCache::GetIconForPath(std::filesystem::path const& path) {
        // if path is empty
        if (path.empty()) return nullptr;

        UnityEngine::Sprite* sprite = nullptr;
        StringW csPath(path.string());
        if (_pathIcons->TryGetValue(csPath, byref(sprite))) {
            PathWasUsed(path);
            return sprite;
        }

        // wasn't loaded -> load
        if (std::filesystem::exists(path)) {
            std::ifstream in(path, std::ios::in | std::ios::binary | std::ios::ate);

            il2cpp_array_size_t len = in.tellg();
            in.seekg(0, std::ios::beg);
            ArrayW<uint8_t> data(len);
            in.read((char*)data.begin(), len);
            sprite = BSML::Utilities::LoadSpriteRaw(data);

            _pathIcons->Add(csPath, sprite);
            PathWasUsed(path);
        } else {
            WARNING("Requested icon file @ {} did not exist!", path.string());
        }

        return sprite;
    }

    UnityEngine::Sprite* IconCache::GetColorsIcon() { GetIconImpl(_colorsIcon, Colors); }
    UnityEngine::Sprite* IconCache::GetEnvironmentIcon() { GetIconImpl(_environmentIcon, Environment); }
    UnityEngine::Sprite* IconCache::GetExtraDiffsIcon() { GetIconImpl(_extraDiffsIcon, ExtraDiffsIcon); }
    UnityEngine::Sprite* IconCache::GetFolderIcon() { GetIconImpl(_folderIcon, FolderIcon); }
    UnityEngine::Sprite* IconCache::GetHaveReqIcon() { GetIconImpl(_haveReqIcon, GreenCheck); }
    UnityEngine::Sprite* IconCache::GetInfoIcon() { GetIconImpl(_infoIcon, Info); }
    UnityEngine::Sprite* IconCache::GetOneSaberIcon() { GetIconImpl(_oneSaberIcon, OneSaber); }
    UnityEngine::Sprite* IconCache::GetMissingReqIcon() { GetIconImpl(_missingReqIcon, RedX); }
    UnityEngine::Sprite* IconCache::GetStandardIcon() { GetIconImpl(_standardIcon, Standard); }
    UnityEngine::Sprite* IconCache::GetWarningIcon() { GetIconImpl(_warningIcon, Warning); }
    UnityEngine::Sprite* IconCache::GetHaveSuggestionIcon() { GetIconImpl(_haveSuggestionIcon, YellowCheck); }
    UnityEngine::Sprite* IconCache::GetMissingSuggestionIcon() { GetIconImpl(_missingSuggestionIcon, YellowX); }
    UnityEngine::Sprite* IconCache::GetDeleteIcon() { GetIconImpl(_deleteIcon, DeleteIcon); }

    void IconCache::PathWasUsed(std::filesystem::path const& path) {
        auto itr = std::find(_lastUsedIcons.begin(), _lastUsedIcons.end(), path);
        if (itr != _lastUsedIcons.end()) { // we found this path, so move it to the back
            _lastUsedIcons.erase(itr);
            _lastUsedIcons.emplace_back(path);
        } else { // path was not found, add it to the back
            _lastUsedIcons.emplace_back(path);
            if (_lastUsedIcons.size() > MAX_ICON_CACHE_COUNT) { // check if we have too many now that we added one, if so, remove and destroy it
                auto start = _lastUsedIcons.begin();
                StringW csPath(start->string());
                _lastUsedIcons.erase(start);

                if (_pathIcons->ContainsKey(csPath)) {
                    auto sprite = _pathIcons->Item[csPath];
                    _pathIcons->Remove(csPath);
                    UnityEngine::Object::Destroy(sprite);
                }

            }
        }
    }
}
