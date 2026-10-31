#pragma once

#include "custom-types/shared/macros.hpp"
#include "UnityEngine/Sprite.hpp"
#include "System/Object.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "System/Collections/Generic/Dictionary_2.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::UI, IconCache, System::Object, System::IDisposable*) {
    DECLARE_CTOR(ctor);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);

    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _colorsIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _environmentIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _extraDiffsIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _folderIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _haveReqIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _infoIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _oneSaberIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _missingReqIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _standardIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _warningIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _haveSuggestionIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _missingSuggestionIcon);
    DECLARE_INSTANCE_FIELD_PRIVATE(UnityEngine::Sprite*, _deleteIcon);

    using IconDict = System::Collections::Generic::Dictionary_2<StringW, UnityEngine::Sprite*>;
    DECLARE_INSTANCE_FIELD_PRIVATE(IconDict*, _pathIcons);
    public:
        UnityEngine::Sprite* GetIconForPath(std::filesystem::path const& path);

        UnityEngine::Sprite* GetColorsIcon();
        __declspec(property(get=GetColorsIcon)) UnityEngine::Sprite* ColorsIcon;

        UnityEngine::Sprite* GetEnvironmentIcon();
        __declspec(property(get=GetEnvironmentIcon)) UnityEngine::Sprite* EnvironmentIcon;

        UnityEngine::Sprite* GetExtraDiffsIcon();
        __declspec(property(get=GetExtraDiffsIcon)) UnityEngine::Sprite* ExtraDiffsIcon;

        UnityEngine::Sprite* GetFolderIcon();
        __declspec(property(get=GetFolderIcon)) UnityEngine::Sprite* FolderIcon;

        UnityEngine::Sprite* GetHaveReqIcon();
        __declspec(property(get=GetHaveReqIcon)) UnityEngine::Sprite* HaveReqIcon;

        UnityEngine::Sprite* GetInfoIcon();
        __declspec(property(get=GetInfoIcon)) UnityEngine::Sprite* InfoIcon;

        UnityEngine::Sprite* GetOneSaberIcon();
        __declspec(property(get=GetOneSaberIcon)) UnityEngine::Sprite* OneSaberIcon;

        UnityEngine::Sprite* GetMissingReqIcon();
        __declspec(property(get=GetMissingReqIcon)) UnityEngine::Sprite* MissingReqIcon;

        UnityEngine::Sprite* GetStandardIcon();
        __declspec(property(get=GetStandardIcon)) UnityEngine::Sprite* StandardIcon;

        UnityEngine::Sprite* GetWarningIcon();
        __declspec(property(get=GetWarningIcon)) UnityEngine::Sprite* WarningIcon;

        UnityEngine::Sprite* GetHaveSuggestionIcon();
        __declspec(property(get=GetHaveSuggestionIcon)) UnityEngine::Sprite* HaveSuggestionIcon;

        UnityEngine::Sprite* GetMissingSuggestionIcon();
        __declspec(property(get=GetMissingSuggestionIcon)) UnityEngine::Sprite* MissingSuggestionIcon;

        UnityEngine::Sprite* GetDeleteIcon();
        __declspec(property(get=GetDeleteIcon)) UnityEngine::Sprite* DeleteIcon;

        static IconCache* get_instance() { return _instance; }
    private:
        static IconCache* _instance;
        static constexpr size_t MAX_ICON_CACHE_COUNT = 50;
        /// @brief keeps track of the last used icons, and if the cache has too many used icons, the least used will be removed
        std::list<std::filesystem::path> _lastUsedIcons;

        void PathWasUsed(std::filesystem::path const& path);
};