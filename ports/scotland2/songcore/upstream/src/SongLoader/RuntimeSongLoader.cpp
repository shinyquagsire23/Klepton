#include "SongLoader/RuntimeSongLoader.hpp"
#include "SongLoader/CustomBeatmapLevel.hpp"
#include "SongLoader/CustomBeatmapLevelsRepository.hpp"
#include "SongLoader/CustomLevelPack.hpp"
#include "CustomJSONData.hpp"
#include "SongCore.hpp"

#include "logging.hpp"
#include "config.hpp"
#include "assets.hpp"

#include "paper2_scotland2/shared/utfcpp/source/utf8.h"
#include "bsml/shared/BSML/MainThreadScheduler.hpp"
#include "bsml/shared/Helpers/utilities.hpp"

#include "Utils/Hashing.hpp"
#include "Utils/File.hpp"
#include "Utils/Cache.hpp"

#include "System/Collections/Generic/ICollection_1.hpp"
#include "System/Collections/Generic/IEnumerable_1.hpp"
#include "System/Collections/Generic/IEnumerator_1.hpp"
#include "System/Collections/IEnumerator.hpp"
#include "System/IDisposable.hpp"

#include "Utils/SaveDataVersion.hpp"

DEFINE_TYPE(SongCore::SongLoader, RuntimeSongLoader);

#define MAX_THREAD_COUNT 8

using namespace std::chrono;

namespace SongCore::SongLoader {
    RuntimeSongLoader* RuntimeSongLoader::_instance = nullptr;

    std::string lowerString(std::string_view str) {
        std::string result;
        result.resize(str.size());
        std::transform(str.begin(), str.end(), result.begin(), tolower);
        return result;
    }

    void RuntimeSongLoader::ctor(GlobalNamespace::CustomLevelLoader* customLevelLoader, GlobalNamespace::BeatmapLevelsModel* beatmapLevelsModel, LevelLoader* levelLoader) {
        INVOKE_CTOR();

        _beatmapLevelsModel = beatmapLevelsModel;
        _customLevelLoader = customLevelLoader;
        _levelLoader = levelLoader;

        auto t = time(nullptr);
        auto localTime = *localtime(&t);
        auto evilTime = (localTime.tm_mday == 1 && localTime.tm_mon == 3);
        bool doEvil = evilTime || ((rand() % 20) == 0);

        _customLevelPack = CustomLevelPack::New(fmt::format("{}CustomLevels", CUSTOM_LEVEL_PACK_PREFIX_ID), "Custom Levels", BSML::Utilities::LoadSpriteRaw(doEvil ? Assets::Resources::CustomLevelsCoverEvil_png : Assets::Resources::CustomLevelsCover_png));
        _customWIPLevelPack = CustomLevelPack::New(fmt::format("{}CustomWIPLevels", CUSTOM_LEVEL_PACK_PREFIX_ID), "Custom WIP Levels", BSML::Utilities::LoadSpriteRaw(Assets::Resources::CustomWIPLevelsCover_png));
        _customBeatmapLevelsRepository = CustomBeatmapLevelsRepository::New_ctor();

        _customLevels = SongDict::New_ctor();
        _customWIPLevels = SongDict::New_ctor();
    }

    void RuntimeSongLoader::Initialize() {
        DEBUG("RuntimeSongLoader Initialize");
        if (_instance) { return; }
        _instance = this;

        RefreshSongs(true);
    }

    void RuntimeSongLoader::Dispose() {
        if (_instance == this) _instance = nullptr;

        _customLevels->Clear();
        _customWIPLevels->Clear();
    }

    void RuntimeSongLoader::CollectLevels(std::filesystem::path const& root, bool isWip, std::set<LevelPathAndWip>& out) {
        // recursively find folders in this root folder to load songs from
        std::error_code error_code;
        auto iterator = std::filesystem::recursive_directory_iterator(root, error_code);
        if (error_code) {
            WARNING("Failed to get iterator for directory {}: {}", root.string(), error_code.message());
            return;
        }

        for (auto entry : iterator) {
            if (!entry.is_directory()) continue;
            auto songPath = entry.path();
            // if this is an autosaves dir, just skip silently
            if (songPath.string().ends_with("autosaves")) continue;

            auto dataPath = songPath / "info.dat";
            if (!std::filesystem::exists(dataPath)) {
                dataPath = songPath / "Info.dat";
                if (!std::filesystem::exists(dataPath)) {
                    WARNING("Possible song folder '{}' had no info.dat file! skipping...", songPath.string());
                    continue;
                }
            }

            out.emplace(songPath, isWip);
        }
    }

    void RuntimeSongLoader::CollectLevels(std::span<const std::filesystem::path> roots, bool isWip, std::set<LevelPathAndWip>& out) {
        for (auto& rootPath : roots) {
            if (!std::filesystem::exists(rootPath)) {
                WARNING("Attempted to load songs from folder '{}' but it did not exist! skipping...", rootPath.string());
                continue;
            }

            CollectLevels(rootPath, isWip, out);
        }
    }

    std::shared_future<void> RuntimeSongLoader::RefreshSongs(bool fullRefresh) {
        if (AreSongsRefreshing) {
            INFO("Refresh was requested while songs were refreshing, queueing up a new refresh for afterwards, or returning the already queued up refresh");
            // if double future isn't valid, that means we need to make a new one here
            if (!_doubleRefreshRequestedFuture.valid()) {
                std::unique_lock<std::shared_mutex> writingLock(_doubleRefreshMutex);
                // if it wasn't marked as a full refresh, mark it as such
                _doubleRefreshIsFull = fullRefresh;
                _doubleRefreshRequestedFuture = il2cpp_utils::il2cpp_async(std::launch::async, &RuntimeSongLoader::RefreshRequestedWhileRefreshing, this);
            } else {
                // if the double refresh isn't full, update it
                if (!_doubleRefreshIsFull) {
                    std::unique_lock<std::shared_mutex> writingLock(_doubleRefreshMutex);
                    _doubleRefreshIsFull = fullRefresh;
                }
            }

            std::shared_lock<std::shared_mutex> readingLock(_doubleRefreshMutex);
            return _doubleRefreshRequestedFuture;
        }

        std::unique_lock<std::shared_mutex> writingLock(_currentRefreshMutex);
        _currentlyLoadingFuture = il2cpp_utils::il2cpp_async(std::launch::async, &RuntimeSongLoader::RefreshSongs_internal, this, std::forward<bool>(fullRefresh));
        return _currentlyLoadingFuture;
    }

    void RuntimeSongLoader::RefreshRequestedWhileRefreshing() {
        // while old refresh still going, wait
        std::shared_lock<std::shared_mutex> currentRefreshReadLock(_currentRefreshMutex);
        auto currentRefreshFuture = _currentlyLoadingFuture;
        currentRefreshReadLock.unlock();

        currentRefreshFuture.wait();

        // queue up another refresh
        currentRefreshFuture = RefreshSongs(_doubleRefreshIsFull);

        // overwrite current loading future with this double request future since we want to
        // then allow a refresh requested during *this* one to be allowed to overwrite the double refresh
        {
            std::unique_lock<std::shared_mutex> currentRefreshWriteLock(_currentRefreshMutex);
            std::unique_lock<std::shared_mutex> doubleRefreshWriteLock(_doubleRefreshMutex);
            _currentlyLoadingFuture = std::move(_doubleRefreshRequestedFuture);
        }

        // wait for our queued refresh to finish
        currentRefreshFuture.wait();
    }

    void RuntimeSongLoader::RefreshSongs_internal(bool fullRefresh) {
        // AreRefreshing is already false here, but areLoaded may be true depending on whether this is a new reload or not
        InvokeSongsWillRefresh();

        auto refreshStartTime = high_resolution_clock::now();
        std::set<LevelPathAndWip> levels;
        _areSongsLoaded = false;
        _loadedSongs = 0;

        // travel the given song paths to collect levels to load
        CollectLevels(config.RootCustomLevelPaths, false, levels);
        CollectLevels(config.RootCustomWIPLevelPaths, true, levels);

        if (fullRefresh) {
            CustomLevels->Clear();
            CustomWIPLevels->Clear();
        }

        // load songs on multiple threads
        std::mutex levelsItrMutex;
        std::set<LevelPathAndWip>::const_iterator levelsItr = levels.begin();
        std::set<LevelPathAndWip>::const_iterator levelsEnd = levels.end();

        using namespace std::chrono;
        auto loadStartTime = high_resolution_clock::now();

        auto workerThreadCount = std::clamp<size_t>(levels.size(), 1, MAX_THREAD_COUNT);
        std::vector<std::future<void>> songLoadFutures;
        songLoadFutures.reserve(workerThreadCount);
        _totalSongs = levels.size();

        INFO("Now going to load {} levels on {} threads", (int)_totalSongs, workerThreadCount);
        for (int i = 0; i < workerThreadCount; i++) {
            songLoadFutures.emplace_back(
                il2cpp_utils::il2cpp_async(
                    &RuntimeSongLoader::RefreshSongWorkerThread,
                    this,
                    &levelsItrMutex,
                    &levelsItr,
                    &levelsEnd
                )
            );
        }

        for (auto& t : songLoadFutures) {
            t.wait();
        }

        size_t actualCount = _customLevels->Count + _customWIPLevels->Count;
        auto time = high_resolution_clock::now() - loadStartTime;
        if (auto ms = duration_cast<milliseconds>(time).count(); ms > 0) {
            INFO("Loaded {} (actual: {}) songs in {}ms", levels.size(), actualCount, ms);
        } else {
            auto µs = (float)duration_cast<nanoseconds>(time).count() / 1000.0f;
            INFO("Loaded {} (actual: {}) songs in {}us", levels.size(), actualCount, µs);
        }

        // save cache to file after all songs are loaded
        Utils::SaveSongInfoCache();

        // anonymous function to get the values from a songdict into a vector
        static auto GetValues = [](SongDict* dict){
            std::vector<CustomBeatmapLevel*> vec;
            vec.reserve(dict->Count);

            auto enumerator = dict->GetEnumerator();
            while(enumerator->i___System__Collections__IEnumerator()->MoveNext()) {
                vec.emplace_back(enumerator->Current.Value);
            }
            enumerator->i___System__IDisposable()->Dispose();

            return vec;
        };

        auto collectionUpdateStartTime = high_resolution_clock::now();

        auto customLevelValues = GetValues(_customLevels);
        auto customWIPLevelValues = GetValues(_customWIPLevels);

        _customLevelPack->SetLevels(customLevelValues);
        _customLevelPack->SortLevels();

        _customWIPLevelPack->SetLevels(customWIPLevelValues);
        _customWIPLevelPack->SortLevels();

        {
            std::vector<CustomBeatmapLevel*> allLevels;
            allLevels.reserve(actualCount);

            // insert wip levels before other loaded levels
            allLevels.insert(allLevels.begin(), customWIPLevelValues.begin(), customWIPLevelValues.end());
            allLevels.insert(allLevels.begin(), customLevelValues.begin(), customLevelValues.end());

            std::unordered_map<std::string, CustomBeatmapLevel*> levelIdsToLevels;
            std::unordered_map<std::string, CustomBeatmapLevel*> hashesToLevels;
            levelIdsToLevels.reserve(actualCount);
            hashesToLevels.reserve(actualCount);

            for (auto const level : allLevels) {
                std::string levelID = lowerString(static_cast<std::string>(level->levelID));

                levelIdsToLevels[levelID] = level;
                hashesToLevels[std::string(GetHashFromLevelID(levelID))] = level;
            }

            // touch collections as short as possible by using move
            _allLoadedLevels = std::move(allLevels);
            _levelIdsToLevels = std::move(levelIdsToLevels);
            _hashesToLevels = std::move(hashesToLevels);
        }

        INFO("Updated collections after load in {}ms", duration_cast<milliseconds>(high_resolution_clock::now() - collectionUpdateStartTime).count());

        // events happen on main thread anyway so we don't have to queue up on main thread
        RefreshLevelPacks();

        // same goes here, it's already on main thread
        InvokeSongsLoaded(_allLoadedLevels);
        _areSongsLoaded = true;
        INFO("Refresh performed in {}ms", duration_cast<milliseconds>(high_resolution_clock::now() - refreshStartTime).count());
    }

    void RuntimeSongLoader::RefreshSongWorkerThread(std::mutex* levelsItrMutex, std::set<LevelPathAndWip>::const_iterator* levelsItr, std::set<LevelPathAndWip>::const_iterator* levelsEnd) {
        auto NextLevel = [](std::mutex& levelsItrMutex, std::set<LevelPathAndWip>::const_iterator& levelsItr, std::set<LevelPathAndWip>::const_iterator& levelsEnd) -> LevelPathAndWip {
            std::lock_guard<std::mutex> lock(levelsItrMutex);
            if (levelsItr != levelsEnd) {
                auto v = *levelsItr;
                levelsItr++;
                return v;
            } else {
                return {};
            }
        };

        while (*levelsItr != *levelsEnd) {
            auto [levelPath, isWip] = NextLevel(*levelsItrMutex, *levelsItr, *levelsEnd);

            // we got an invalid levelPath
            if (levelPath.empty()) {
                _loadedSongs++;
                continue;
            }

            try {
                auto startTime = high_resolution_clock::now();
                CustomBeatmapLevel* level = nullptr;
                StringW csLevelPath(levelPath.string());

                // pick the dictionary we need to add / check from based on whether this song is WIP
                auto targetDict = isWip ? _customWIPLevels : _customLevels;

                // preliminary check to see whether the song we are looking for already is in our dictionary
                bool containsKey = targetDict->ContainsKey(csLevelPath);
                if (containsKey) {
                    level = targetDict->get_Item(levelPath.string());
                }

                // if the level is not yet set, attempt loading levelinfosavedata from the song path, then load custom preview beatmap level from that
                if (!level) {
                    static Version v4(4);
                    static auto GetSaveDataVersion = [](std::filesystem::path const& levelPath) {
                        std::filesystem::path infoPath = levelPath / "info.dat";
                        if (!std::filesystem::exists(infoPath)) infoPath = levelPath / "Info.dat";
                        return VersionFromFilePath(infoPath);
                    };

                    auto v = GetSaveDataVersion(levelPath);
                    if (v < v4) { // v3
                        auto saveData = _levelLoader->GetSaveDataFromV3(levelPath);
                        if (saveData) {
                            std::string hash;
                            level = _levelLoader->LoadCustomBeatmapLevel(levelPath, isWip, saveData, hash);
                        }
                    } else { // v4
                        auto saveData = _levelLoader->GetSaveDataFromV4(levelPath);
                        if (saveData) {
                            std::string hash;
                            level = _levelLoader->LoadCustomBeatmapLevel(levelPath, isWip, saveData, hash);
                        }
                    }
                }

                // if we now have a level, add it to the target dictionary, else log a failure
                if (level) {
                    targetDict->TryAdd(levelPath.string(), level);
                } else {
                    WARNING("Somehow failed to load song at path {}", levelPath.string());
                }

                auto time = high_resolution_clock::now() - startTime;
                if (auto ms = duration_cast<milliseconds>(time).count(); ms > 0) {
                    INFO("Loaded song in {}ms", ms);
                } else {
                    auto µs = (float)duration_cast<nanoseconds>(time).count() / 1000.0f;
                    INFO("Loaded song in {}us", µs);
                }

                // update progress
                _loadedSongs++;
            } catch (std::exception const& e) {
                ERROR("Caught exception of type {} while loading song @ path '{}', song will be skipped! what: {}", typeid(e).name(), levelPath.string(), e.what());
                // if an error was caught, a song failed to load so we decrease total song count
                _totalSongs--;
            } catch (...) {
                ERROR("Caught exception of unknown type (current_exception typeid: {}) while loading song @ path '{}', song will be skipped!", typeid(std::current_exception()).name(), levelPath.string());
                // if an error was caught, a song failed to load so we decrease total song count
                _totalSongs--;
            }
        }
    }

    void RuntimeSongLoader::RefreshLevelPacks() {
        auto allLoaded = il2cpp_utils::cast<SongLoader::CustomBeatmapLevelsRepository>(_beatmapLevelsModel->_allLoadedBeatmapLevelsRepository);
        for (auto pack : _customBeatmapLevelsRepository->BeatmapLevelPacks) {
            allLoaded->RemoveLevelPack(pack);
        }
        _customBeatmapLevelsRepository->ClearLevelPacks();

        _customBeatmapLevelsRepository->AddLevelPack(_customLevelPack);
        _customBeatmapLevelsRepository->AddLevelPack(_customWIPLevelPack);

        InvokeCustomLevelPacksWillRefresh(_customBeatmapLevelsRepository);

        _customBeatmapLevelsRepository->FixBackingDictionaries();

        for (auto pack : _customBeatmapLevelsRepository->BeatmapLevelPacks) {
            allLoaded->AddLevelPack(pack);
        }

        allLoaded->FixBackingDictionaries();

        InvokeCustomLevelPacksRefreshed(_customBeatmapLevelsRepository);
    }

    void RuntimeSongLoader::DeleteSong_internal(std::filesystem::path levelPath) {
        INFO("Deleting song @ path {}", levelPath.string());
        auto csPath = StringW(levelPath.string());
        SongDict* targetDict = nullptr;

        CustomBeatmapLevel* level = nullptr;

        if (CustomLevels->ContainsKey(csPath)) {
            targetDict = CustomLevels;
            level = CustomLevels->get_Item(csPath);
        } else if (CustomWIPLevels->ContainsKey(csPath)) {
            targetDict = CustomWIPLevels;
            level = CustomWIPLevels->get_Item(csPath);
        }

        if (!targetDict) {
            WARNING("Level with path {} was attempted to be deleted, but it couldn't be found in the songloader dictionaries! returning...", levelPath.string());
            return;
        } else if (!level) {
            WARNING("Somehow the stored beatmap level was null, just removing from the dictionary and nothing else...");
            targetDict->System_Collections_Generic_IDictionary_TKey_TValue__Remove(csPath);
            return;
        }

        // let consumers of our api know a song will be deleted
        InvokeSongWillBeDeleted(level);

        std::error_code error_code;
        std::filesystem::remove_all(levelPath, error_code);

        if (error_code) WARNING("Error occurred during removal of {}: {}", levelPath.string(), error_code.message());
        if (!targetDict->System_Collections_Generic_IDictionary_TKey_TValue__Remove(csPath)) WARNING("Failed to remove beatmap for {} from dictionary!", levelPath.string());

        // since a (soft) refresh is required after a reload, there's no need to remove from the c++ collections
        // like _allLoadedLevels, _levelIdsToLevels, _hashesToLevels

        bool deletedInvoked = false;
        // let consumers of our api know a song was deleted
        InvokeSongDeleted();
    }

    std::future<void> RuntimeSongLoader::DeleteSong(std::filesystem::path const& levelPath) {
        return il2cpp_utils::il2cpp_async(&RuntimeSongLoader::DeleteSong_internal, this, levelPath);
    }

    std::future<void> RuntimeSongLoader::DeleteSong(CustomBeatmapLevel* beatmapLevel) {
        if (!beatmapLevel) return std::future<void>();
        return DeleteSong(static_cast<std::string>(beatmapLevel->customLevelPath));
    }

    CustomBeatmapLevel* RuntimeSongLoader::GetLevelByPath(std::filesystem::path const& levelPath) {
        auto csPath = StringW(levelPath.string());

        CustomBeatmapLevel* level = nullptr;
        if (CustomLevels->TryGetValue(csPath, byref(level))) return level;
        else if (CustomWIPLevels->TryGetValue(csPath, byref(level))) return level;

        return GetLevelByFunction([path = levelPath.string()](auto level){ return level->customLevelPath == path; });
    }

    CustomBeatmapLevel* RuntimeSongLoader::GetLevelByLevelID(std::string_view levelID) {
        // check levelids map first, if not found iterate all levels
        std::string idString(lowerString(levelID));
        auto itr = _levelIdsToLevels.find(idString);
        if (itr != _levelIdsToLevels.end()) return itr->second;
        return nullptr;
    }

    CustomBeatmapLevel* RuntimeSongLoader::GetLevelByHash(std::string_view hash_view) {
        std::string hashString = lowerString(hash_view);
        auto itr = _hashesToLevels.find(hashString);
        if (itr != _hashesToLevels.end()) return itr->second;
        return nullptr;
    }

    CustomBeatmapLevel* RuntimeSongLoader::GetLevelByFunction(std::function<bool(CustomBeatmapLevel*)> searchFunction) {
        auto levelItr = std::find_if(AllLevels.begin(), AllLevels.end(), searchFunction);
        if (levelItr == AllLevels.end()) return nullptr;
        return *levelItr;
    }

    std::filesystem::path RuntimeSongLoader::get_SongPath() const {
        return SongCore::API::Loading::GetPreferredCustomLevelPath();
    }

    std::filesystem::path RuntimeSongLoader::get_WIPSongPath() const {
        return SongCore::API::Loading::GetPreferredCustomWIPLevelPath();
    }

    std::string_view RuntimeSongLoader::GetHashFromLevelID(std::string_view levelid) {
        if (!levelid.starts_with("custom_level_")) return levelid;
        levelid = levelid.substr(13);
        return levelid.substr(0, levelid.find(' '));
    }

    std::u16string_view RuntimeSongLoader::GetHashFromLevelID(std::u16string_view levelid) {
        if (!levelid.starts_with(u"custom_level_")) return levelid;
        levelid = levelid.substr(13);
        return levelid.substr(0, levelid.find(u' '));
    }

// macro to wrap an event invoke into something that always executes on main thread. could we just check whether we are on main thread and invoke in place? sure, but where's the fun in that!
#define EVENT_MAIN_THREAD_INVOKE_WRAPPER(event, ...) do { \
    bool eventInvoked = false; \
    BSML::MainThreadScheduler::Schedule([this, &eventInvoked __VA_OPT__(, __VA_ARGS__)](){ \
        event.invoke(__VA_ARGS__); \
        SongCore::API::Loading::Get##event##Event().invoke(__VA_ARGS__); \
        eventInvoked = true; \
    }); \
    while (!eventInvoked) std::this_thread::sleep_for(std::chrono::milliseconds(10)); \
} while (0)

    void RuntimeSongLoader::InvokeSongsWillRefresh() const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(SongsWillRefresh);
    }

    void RuntimeSongLoader::InvokeSongsLoaded(std::span<CustomBeatmapLevel* const> levels) const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(SongsLoaded, levels);
    }

    void RuntimeSongLoader::InvokeCustomLevelPacksWillRefresh(SongCore::SongLoader::CustomBeatmapLevelsRepository* customLevelsRepository) const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(CustomLevelPacksWillRefresh, customLevelsRepository);
    }

    void RuntimeSongLoader::InvokeCustomLevelPacksRefreshed(SongCore::SongLoader::CustomBeatmapLevelsRepository* customLevelsRepository) const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(CustomLevelPacksRefreshed, customLevelsRepository);
    }

    void RuntimeSongLoader::InvokeSongWillBeDeleted(CustomBeatmapLevel* level) const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(SongWillBeDeleted, level);
    }

    void RuntimeSongLoader::InvokeSongDeleted() const {
        EVENT_MAIN_THREAD_INVOKE_WRAPPER(SongDeleted);
    }
}
