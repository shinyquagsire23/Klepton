#pragma once

#include "./_config.h"

#include "SongLoader/CustomBeatmapLevel.hpp"
#include "SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"

#include "SongLoader/CustomLevelPack.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"

#include "CustomJSONData.hpp"

#include <span>
#include <future>

namespace SongCore::API {
    namespace Capabilities {
        enum SONGCORE_EXPORT CapabilityEventKind {
            Registered = 0,
            Unregistered = 1,
        };

        /// @brief Registers a capability to let songcore know what the game is capable of
        /// @param capability The capability to register
        SONGCORE_EXPORT void RegisterCapability(std::string_view capability);

        /// @brief Unregisters a capability to let songcore know what the game is capable of
        /// @param capability The capability to register
        SONGCORE_EXPORT void UnregisterCapability(std::string_view capability);

        /// @brief checks whether a capability is registered
        /// @param capability The checked capability
        /// @return true for registered, false for not
        SONGCORE_EXPORT bool IsCapabilityRegistered(std::string_view capability);

        /// @brief provides access to the registered capabilities without allowing edits
        SONGCORE_EXPORT std::span<const std::string> GetRegisteredCapabilities();

        /// @brief provides access to an event that gets invoked when the capabilities are updated. not guaranteed to run on main thread! not cleared on soft restart. Invoked after the particular capability is added to the list.
        SONGCORE_EXPORT UnorderedEventCallback<std::string_view, Capabilities::CapabilityEventKind>& GetCapabilitiesUpdatedEvent();
    }

    namespace PlayButton {
        struct SONGCORE_EXPORT PlayButtonDisablingModInfo {
            std::string modID;
            std::string reason;
        };

        #ifdef MOD_ID
        /// @brief disallows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        /// @param reason optional reason for why it was disabled
        SONGCORE_EXPORT void DisablePlayButton(std::string modID = MOD_ID, std::string reason = "");

        /// @brief allows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        SONGCORE_EXPORT void EnablePlayButton(std::string modID = MOD_ID);

        #else
        /// @brief disallows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        /// @param reason optional reason for why it was disabled
        SONGCORE_EXPORT void DisablePlayButton(std::string modID, std::string reason = "");

        /// @brief allows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        SONGCORE_EXPORT void EnablePlayButton(std::string modID);

        #endif

        /// @brief event ran when the disabling mod infos change, like when Disable or Enable Play button is called from any mod, provides a span of the disabling mod ids and reasons
        SONGCORE_EXPORT UnorderedEventCallback<std::span<PlayButtonDisablingModInfo const>>& GetPlayButtonDisablingModsChangedEvent();

        /// @brief provides a span of the disabling mod infos
        SONGCORE_EXPORT std::span<PlayButtonDisablingModInfo const> GetPlayButtonDisablingModInfos();
    }

    namespace Characteristics {
        enum SONGCORE_EXPORT CharacteristicEventKind {
            Registered = 0,
            Unregistered = 1,
        };

        /// @brief method to register a custom characteristic. This has to be ran at late_load at the latest to work correctly. Unregistering is not possible.
        /// @param characteristic the characteristic to register
        SONGCORE_EXPORT void RegisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief method to register a custom characteristic. This has to be ran at late_load at the latest to work correctly. Unregistering is not possible.
        /// @param characteristic the characteristic to register
        SONGCORE_EXPORT void UnregisterCustomCharacteristic(GlobalNamespace::BeatmapCharacteristicSO* characteristic);

        /// @brief gets a characteristic by serialized name. Only valid to be called after the first zenject install has happened
        /// @param serializedName the name to look for
        /// @return the found characteristic, or nullptr if not found. not guaranteed to still be a valid unity object!
        SONGCORE_EXPORT GlobalNamespace::BeatmapCharacteristicSO* GetCharacteristicBySerializedName(std::string_view serializedName);

        /// @brief provides access to the registered characteristics without allowing edits
        SONGCORE_EXPORT std::span<GlobalNamespace::BeatmapCharacteristicSO*> GetRegisteredCharacteristics();

        /// @brief provides access to an event that gets invoked when the custom characteristics are updated. not guaranteed to run on main thread! not cleared on soft restart. Invoked after the particular characteristic is added to the list.
        SONGCORE_EXPORT UnorderedEventCallback<GlobalNamespace::BeatmapCharacteristicSO*, Characteristics::CharacteristicEventKind>& GetCharacteristicsUpdatedEvent();

        /// @brief creates a characteristic to register, it's your responsibility to manage the lifetime of it
        SONGCORE_EXPORT GlobalNamespace::BeatmapCharacteristicSO* CreateCharacteristic(UnityEngine::Sprite* icon, StringW characteristicName, StringW hintText, StringW serializedName, StringW compoundIdPartName, bool requires360Movement, bool containsRotationEvents, int sortingOrder);
    }

    namespace Loading {
        /// @brief refresh the loaded songs in the songloader, if the songloader doesn't exist an invalid future is returned. the returned future can be ignored safely
        /// @return future you can use to check whether songs are done refreshing. if you want an onFinished see `GetSongsLoadedEvent`
        SONGCORE_EXPORT std::shared_future<void> RefreshSongs(bool fullRefresh = false);

        /// @brief refresh the level packs, since this does not take long, it is not async
        SONGCORE_EXPORT void RefreshLevelPacks();

        /// @brief delete a song by providing its path
        /// @return a future you can use to check whether the deletion is done. if the songloader didn't exist yet it will give you a future that's not valid
        SONGCORE_EXPORT std::future<void> DeleteSong(std::filesystem::path const& levelPath);

        /// @brief delete a song by providing its preview beatmap level
        /// @return a future you can use to check whether the deletion is done. if the songloader didn't exist yet it will give you a future that's not valid
        SONGCORE_EXPORT std::future<void> DeleteSong(::SongCore::SongLoader::CustomBeatmapLevel* beatmapLevel);

        /// @brief event ran when songs are done refreshing
        SONGCORE_EXPORT UnorderedEventCallback<std::span<::SongCore::SongLoader::CustomBeatmapLevel* const>>& GetSongsLoadedEvent();

        /// @brief event ran when song refreshing will start
        SONGCORE_EXPORT UnorderedEventCallback<>& GetSongsWillRefreshEvent();

        /// @brief event ran before the beatmap levels model is updated, ideal for adding your own packs to the collection ordering them differently, or removing packs you don't want in there
        SONGCORE_EXPORT UnorderedEventCallback<SongCore::SongLoader::CustomBeatmapLevelsRepository*>& GetCustomLevelPacksWillRefreshEvent();

        /// @brief event ran after the beatmap levels model got updated
        SONGCORE_EXPORT UnorderedEventCallback<SongCore::SongLoader::CustomBeatmapLevelsRepository*>& GetCustomLevelPacksRefreshedEvent();

        /// @brief event ran before a song is actually deleted in case you want to keep track of the deleted songs
        SONGCORE_EXPORT UnorderedEventCallback<::SongCore::SongLoader::CustomBeatmapLevel*>& GetSongWillBeDeletedEvent();

        /// @brief event ran after a song was deleted. At this point there's no way of knowing what song it was so it's advised to look at `GetSongWillBeDeletedEvent`
        SONGCORE_EXPORT UnorderedEventCallback<>& GetSongDeletedEvent();

        /// @brief returns the path where levels should be stored
        SONGCORE_EXPORT std::filesystem::path GetPreferredCustomLevelPath();

        /// @brief returns a span of the custom level paths songloader loads levels from
        SONGCORE_EXPORT std::span<std::filesystem::path const> GetRootCustomLevelPaths();

        /// @brief returns the path where WIP levels should be stored
        SONGCORE_EXPORT std::filesystem::path GetPreferredCustomWIPLevelPath();

        /// @brief returns a span of the custom WIP level paths songloader loads levels from
        SONGCORE_EXPORT std::span<std::filesystem::path const> GetRootCustomWIPLevelPaths();

        /// @brief adds a path to the songcore config to load songs from
        /// @param path the path to load from
        /// @param wipPath whether this path is a wip path
        SONGCORE_EXPORT void AddLevelPath(std::filesystem::path const& path, bool wipPath = false);

        /// @brief removes a path from the songcore config
        /// @param path the path to remove
        /// @param wipPath whether this path is a wipPath
        SONGCORE_EXPORT void RemoveLevelPath(std::filesystem::path const& path, bool wipPath = false);

        /// @brief whether the songloader is currently refreshing songs
        SONGCORE_EXPORT bool AreSongsRefreshing();

        /// @brief whether the songloader has loaded songs
        SONGCORE_EXPORT bool AreSongsLoaded();

        /// @brief current loading progress
        SONGCORE_EXPORT float LoadProgress();

        /// @brief provides a span of all levels the songloader currently knows about
        /// @return if songloader not setup returns an empty span
        SONGCORE_EXPORT std::span<::SongCore::SongLoader::CustomBeatmapLevel* const> GetAllLevels();

        /// @brief Getter for the custom level pack songcore creates
        /// @return created pack, or nullptr if the songloader didn't exist
        SONGCORE_EXPORT SongLoader::CustomLevelPack* GetCustomLevelPack();

        /// @brief Getter for the custom WIP level pack songcore creates
        /// @return created pack, or nullptr if the songloader didn't exist
        SONGCORE_EXPORT SongLoader::CustomLevelPack* GetCustomWIPLevelPack();

        /// @brief getter for the custom level repository songcore creates
        /// @return created repository, or nullptr if the songloader didn't exist
        SONGCORE_EXPORT SongLoader::CustomBeatmapLevelsRepository* GetCustomBeatmapLevelsRepository();

        /// @brief gets a level by the levelpath
        /// @return nullptr if level not found
        SONGCORE_EXPORT ::SongCore::SongLoader::CustomBeatmapLevel* GetLevelByPath(std::filesystem::path const& levelPath);

        /// @brief gets a level by the levelId
        /// @return nullptr if level not found
        SONGCORE_EXPORT ::SongCore::SongLoader::CustomBeatmapLevel* GetLevelByLevelID(std::string_view levelID);

        /// @brief gets a level by the hash
        /// @return nullptr if level not found
        SONGCORE_EXPORT ::SongCore::SongLoader::CustomBeatmapLevel* GetLevelByHash(std::string_view hash);

        /// @brief gets a level by a search function
        /// @return nullptr if level not found
        SONGCORE_EXPORT ::SongCore::SongLoader::CustomBeatmapLevel* GetLevelByFunction(std::function<bool(::SongCore::SongLoader::CustomBeatmapLevel*)> searchFunction);
    }

    namespace LevelSelect {
        struct SONGCORE_EXPORT LevelWasSelectedEventArgs {
            /// @brief whether this is a custom level. if true, using the custom** in the unions should be valid, if not, you should be using the interfaces
            bool isCustom = false;
            /// @brief whether this is a WIP level
            bool isWIP = false;

            /// @brief levelID for the level
            std::string levelID;

            /// @brief optional level hash
            std::optional<std::string> hash = std::nullopt;

            struct BasicCustomLevelDetailsGroup {
                CustomJSONData::CustomSaveDataInfo::BasicCustomLevelDetails const& levelDetails;
                CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetailsSet const& characteristicDetails;
                CustomJSONData::CustomSaveDataInfo::BasicCustomDifficultyBeatmapDetails const& difficultyDetails;
            };

            /// @brief if this is a V2-V3 custom level, this should be set
            std::optional<CustomJSONData::CustomLevelInfoSaveDataV2*> customLevelInfoSaveDataV2 = std::nullopt;

            /// @brief if this is a V4 custom level, this should be set
            std::optional<CustomJSONData::CustomBeatmapLevelSaveDataV4*> customBeatmapLevelSaveDataV4 = std::nullopt;

            /// @brief if this is a custom level, this should be set
            std::optional<BasicCustomLevelDetailsGroup> customLevelDetails = std::nullopt;

            /// @brief the selected levelpack, may or may not be a custom pack, can be null in favorites & search views!
            union {
                SongCore::SongLoader::CustomLevelPack* customLevelPack = nullptr;
                GlobalNamespace::BeatmapLevelPack* levelPack;
            };

            /// @brief selected beatmap level
            union {
                ::SongCore::SongLoader::CustomBeatmapLevel* customBeatmapLevel = nullptr;
                GlobalNamespace::BeatmapLevel* beatmapLevel;
            };

            /// @brief selected beatmap key
            GlobalNamespace::BeatmapKey beatmapKey;
        };

        SONGCORE_EXPORT UnorderedEventCallback<LevelWasSelectedEventArgs const&>& GetLevelWasSelectedEvent();
    }
}
