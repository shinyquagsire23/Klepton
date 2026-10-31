#pragma once

#include <string>
#include <string_view>
#include <filesystem>
#include <vector>
#include <mutex>
#include <set>

#include "custom-types/shared/macros.hpp"

#include "LevelLoader.hpp"
#include "CustomLevelPack.hpp"
#include "CustomBeatmapLevel.hpp"
#include "CustomBeatmapLevelsRepository.hpp"

#include "System/Collections/Concurrent/ConcurrentDictionary_2.hpp"
#include "System/Collections/Generic/List_1.hpp"

#include "GlobalNamespace/StandardLevelInfoSaveData.hpp"
#include "GlobalNamespace/EnvironmentInfoSO.hpp"
#include "GlobalNamespace/BeatmapDataLoader.hpp"
#include "GlobalNamespace/CustomLevelLoader.hpp"
#include "GlobalNamespace/BeatmapLevelsModel.hpp"

#include "Zenject/IInitializable.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"

namespace SongCore::SongLoader {
    using SongDict = ::System::Collections::Concurrent::ConcurrentDictionary_2<StringW, CustomBeatmapLevel*>;
}

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::SongLoader, RuntimeSongLoader, System::Object, Zenject::IInitializable*, System::IDisposable*) {
    DECLARE_CTOR(ctor, GlobalNamespace::CustomLevelLoader* customLevelLoader, GlobalNamespace::BeatmapLevelsModel* _beatmapLevelsModel, LevelLoader* levelLoader);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);

    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::CustomLevelLoader*, _customLevelLoader);
    DECLARE_INSTANCE_FIELD_PRIVATE(GlobalNamespace::BeatmapLevelsModel*, _beatmapLevelsModel);
    DECLARE_INSTANCE_FIELD_PRIVATE(LevelLoader*, _levelLoader);

    DECLARE_INSTANCE_FIELD_PRIVATE(SongCore::SongLoader::CustomLevelPack*, _customLevelPack);
    DECLARE_INSTANCE_FIELD_PRIVATE(SongCore::SongLoader::CustomLevelPack*, _customWIPLevelPack);
    DECLARE_INSTANCE_FIELD_PRIVATE(SongCore::SongLoader::CustomBeatmapLevelsRepository*, _customBeatmapLevelsRepository);

    DECLARE_INSTANCE_FIELD_PRIVATE(SongDict*, _customLevels);
    DECLARE_INSTANCE_FIELD_PRIVATE(SongDict*, _customWIPLevels);

    public:
        static inline const std::string CUSTOM_LEVEL_PREFIX_ID = "custom_level_";
        static inline const std::string CUSTOM_LEVEL_PACK_PREFIX_ID = "custom_levelPack_";

        /// @brief refreshes the loaded songs, loading any new ones
        /// @param fullRefresh whether to reload every song or only new ones
        /// @return shared future which you may await for when the songs are finished refreshing. if you want an onFinished use the `LevelsLoaded` event
        std::shared_future<void> RefreshSongs(bool fullRefresh = false);

        /// @brief refreshes the level packs in the beatmaplevelsmodel
        void RefreshLevelPacks();

        /// @brief delete a level by providing its level path
        std::future<void> DeleteSong(std::filesystem::path const& levelPath);

        /// @brief delete a level by providing a preview beatmap level, which is used to get the level path
        std::future<void> DeleteSong(SongCore::SongLoader::CustomBeatmapLevel* beatmapLevel);

        /// @brief Returns the instance of the songloader for people not using zenject. only valid when the app context is valid (soft restarts & game start have times when its not valid)
        static RuntimeSongLoader* get_instance() { return _instance; };

        /// @brief Returns the preferred song path.
        std::filesystem::path get_SongPath() const;
        __declspec(property(get = get_SongPath)) std::filesystem::path SongPath;

        /// @brief Returns the current WIP song path.
        std::filesystem::path get_WIPSongPath() const;
        __declspec(property(get = get_WIPSongPath)) std::filesystem::path WIPSongPath;

        /// @brief Returns the custom level dictionary
        SongDict* get_customLevels() const { return _customLevels; };
        __declspec(property(get = get_customLevels)) SongLoader::SongDict* CustomLevels;

        /// @brief Returns the custom wip level dictionary
        SongDict* get_customWIPLevels() const { return _customWIPLevels; };
        __declspec(property(get = get_customWIPLevels)) SongLoader::SongDict* CustomWIPLevels;

        /// @brief returns the levelpack songcore makes
        CustomLevelPack* get_CustomLevelPack() const { return _customLevelPack; }
        __declspec(property(get = get_CustomLevelPack)) CustomLevelPack* CustomLevelPack;

        /// @brief returns the wip levelpack songcore makes
        SongCore::SongLoader::CustomLevelPack* get_CustomWIPLevelPack() const { return _customWIPLevelPack; }
        __declspec(property(get = get_CustomWIPLevelPack)) SongCore::SongLoader::CustomLevelPack* CustomWIPLevelPack;

        /// @brief returns the beatmaplevelsrepository songcore makes
        CustomBeatmapLevelsRepository* get_CustomBeatmapLevelsRepository() const { return _customBeatmapLevelsRepository; }
        __declspec(property(get=get_CustomBeatmapLevelsRepository)) CustomBeatmapLevelsRepository* CustomBeatmapLevelsRepository;

        /// @brief lets you check whether songs are currently being refreshed
        bool get_AreSongsRefreshing() {
            using namespace std::chrono_literals;
            std::shared_lock<std::shared_mutex> lock(_currentRefreshMutex);
            // if not valid, we are not refreshing
            if (!_currentlyLoadingFuture.valid()) return false;
            // if not ready, we are refreshing
            return _currentlyLoadingFuture.wait_for(0ns) != std::future_status::ready;
        }

        __declspec(property(get=get_AreSongsRefreshing)) bool AreSongsRefreshing;

        /// @brief lets you check whether songs loaded are valid right now
        bool get_AreSongsLoaded() const { return _areSongsLoaded; }
        __declspec(property(get=get_AreSongsLoaded)) bool AreSongsLoaded;

        /// @brief gets the current song loading progress
        float get_Progress() const { return (float)_loadedSongs / (float)_totalSongs; }
        __declspec(property(get=get_Progress)) float Progress;

        /// @brief gets the total amount of songs to be loaded, may decrease as songs throw errors during load
        size_t get_TotalSongs() const { return (size_t)_totalSongs; }
        __declspec(property(get=get_TotalSongs)) size_t TotalSongs;

        /// @brief gets the current amount of succesfully loaded songs
        size_t get_LoadedSongs() const { return (size_t)_loadedSongs; }
        __declspec(property(get=get_LoadedSongs)) size_t LoadedSongs;

        /// @brief provides access into a span of all loaded levels
        std::span<CustomBeatmapLevel* const> get_AllLevels() const { return _allLoadedLevels; };
        __declspec(property(get=get_AllLevels)) std::span<CustomBeatmapLevel* const> AllLevels;

        /// @brief event invoked when songs will be refreshed
        UnorderedEventCallback<> SongsWillRefresh;

        /// @brief event invoked after song loading has completed, ran on main thread. the provided span is a readonly reference to all levels
        UnorderedEventCallback<std::span<CustomBeatmapLevel* const>> SongsLoaded;

        /// @brief event invoked before the beatmaplevelsmodel is updated with the new collections
        UnorderedEventCallback<SongCore::SongLoader::CustomBeatmapLevelsRepository*> CustomLevelPacksWillRefresh;

        /// @brief event invoked after the beatmaplevelsmodel was updated with the new collection
        UnorderedEventCallback<SongCore::SongLoader::CustomBeatmapLevelsRepository*> CustomLevelPacksRefreshed;

        /// @brief event invoked before a song is deleted so you can do last minute things you want to do with the deleted level
        UnorderedEventCallback<CustomBeatmapLevel*> SongWillBeDeleted;

        /// @brief event invoked after a song got deleted, so you may redo certain operations
        UnorderedEventCallback<> SongDeleted;

        /// @brief gets a level by the levelpath
        /// @return nullptr if level not found
        CustomBeatmapLevel* GetLevelByPath(std::filesystem::path const& levelPath);

        /// @brief gets a level by the levelId
        /// @return nullptr if level not found
        CustomBeatmapLevel* GetLevelByLevelID(std::string_view levelID);

        /// @brief gets a level by the hash
        /// @return nullptr if level not found
        CustomBeatmapLevel* GetLevelByHash(std::string_view hash);

        /// @brief gets a level by a search function
        /// @return nullptr if level not found
        CustomBeatmapLevel* GetLevelByFunction(std::function<bool(CustomBeatmapLevel*)> searchFunction);

        /// @brief gets the environment info with environmentName, or default if not found
        /// @param environmentName the name to look for
        /// @param allDirections if the env was not found, use the default for alldirections or not
        /// @return environmentinfo
        GlobalNamespace::EnvironmentInfoSO* GetEnvironmentInfo(StringW environmentName, bool allDirections);

        /// @brief gets all the environment infos with environmentNames
        /// @param environmentsNames the names to look for
        /// @return environmentinfo array
        ArrayW<GlobalNamespace::EnvironmentInfoSO*> GetEnvironmentInfos(std::span<StringW const> environmentsNames);

        /// @brief helper method to get the hash from the level id
        /// @return string view to the hash, or the entire levelid if not a custom level
        static std::string_view GetHashFromLevelID(std::string_view levelid);

        /// @brief helper method to get the hash from the level id
        /// @return string view to the hash, or the entire levelid if not a custom level
        static std::u16string_view GetHashFromLevelID(std::u16string_view levelid);
    private:
        /// @brief internal struct to keep track of levelpath and wip status of a song before it got loaded
        struct LevelPathAndWip {
            std::filesystem::path levelPath;
            bool isWip;

            auto operator<=>(LevelPathAndWip const& other) const {
                return levelPath <=> other.levelPath;
            }
        };

        /// @brief constructs the color schemes from the savedata
        /// @param colorSchemeDatas the save data color schemes
        /// @return constructed color schemes
        ArrayW<GlobalNamespace::ColorScheme*> GetColorSchemes(std::span<GlobalNamespace::BeatmapLevelColorSchemeSaveData* const> colorSchemeDatas);

        /// @brief collects levels from the root into the given set, and keeps the wip status
        static void CollectLevels(std::filesystem::path const& root, bool isWip, std::set<LevelPathAndWip>& out);

        /// @brief collects levels from the roots into the given set, and keeps the wip status
        static void CollectLevels(std::span<const std::filesystem::path> roots, bool isWip, std::set<LevelPathAndWip>& out);

        /// @brief method used when a double (or triple, quadruple...) refresh is requested
        void RefreshRequestedWhileRefreshing();

        /// @brief method kicked of by RefreshSongs on an il2cpp async
        void RefreshSongs_internal(bool fullRefresh);

        /// @brief worker thread for loading songs from a set
        void RefreshSongWorkerThread(std::mutex* levelsItrMutex, std::set<LevelPathAndWip>::const_iterator* levelsItr, std::set<LevelPathAndWip>::const_iterator* levelsEnd);

        /// @brief internal method for deleting a song, ran through il2cpp async
        void DeleteSong_internal(std::filesystem::path levelPath);

        /// @brief mutex for accesing the current refresh
        std::shared_mutex _currentRefreshMutex;
        /// @brief while songs are refreshing this future holds what is currently happening
        std::shared_future<void> _currentlyLoadingFuture;

        /// @brief mutex for accesing the double refresh
        std::shared_mutex _doubleRefreshMutex;
        /// @brief if a request is sent for a double refresh (while refreshing) this stores the future of that request
        std::shared_future<void> _doubleRefreshRequestedFuture;
        /// @brief whether the double refresh should be a full refresh
        bool _doubleRefreshIsFull;

        /// @brief how many songs have already been loaded
        std::atomic<size_t> _loadedSongs;
        /// @brief how many songs there are
        std::atomic<size_t> _totalSongs;
        /// @brief are songs done loading
        std::atomic<bool> _areSongsLoaded;
        /// @brief all loaded levels
        std::vector<CustomBeatmapLevel*> _allLoadedLevels;
        /// @brief collection holding the level ids to levels
        std::unordered_map<std::string, CustomBeatmapLevel*> _levelIdsToLevels;
        /// @brief collection holding the hashes to levels
        std::unordered_map<std::string, CustomBeatmapLevel*> _hashesToLevels;

        static RuntimeSongLoader* _instance;

        /// @brief invoker method for SongsWillRefresh event
        void InvokeSongsWillRefresh() const;
        /// @brief invoker method for SongsLoaded event
        void InvokeSongsLoaded(std::span<CustomBeatmapLevel* const> levels) const;
        /// @brief invoker method for CustomLevelPacksWillRefresh event
        void InvokeCustomLevelPacksWillRefresh(SongCore::SongLoader::CustomBeatmapLevelsRepository* beatmapLevelsRepository) const;
        /// @brief invoker method for CustomLevelPacksRefreshed event
        void InvokeCustomLevelPacksRefreshed(SongCore::SongLoader::CustomBeatmapLevelsRepository* beatmapLevelsRepository) const;
        /// @brief invoker method for SongWillBeDeleted event
        void InvokeSongWillBeDeleted(CustomBeatmapLevel* level) const;
        /// @brief invoker method for SongDeleted event
        void InvokeSongDeleted() const;
};