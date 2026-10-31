#pragma once

#include "./_config.h"
#include "bshook-utils.hpp"
#include "Data/Song.hpp"
#include "Data/SongDifficulty.hpp"
#include "SongArray.hpp"
#include "DiffArray.hpp"
#include "TagsMap.hpp"
#include <future>
#include <filesystem>
#include <functional>

namespace SongDetailsCache {
    class SONGDETAILS_EXPORT SongDetails {
        public:
            SongArray songs = SongArray();
            DiffArray difficulties = DiffArray();
            TagsMap tags = TagsMap();
            static std::future<SongDetails*> Init();
            static std::future<SongDetails*> Init(int refreshIfOlderThanHours);
            static void SetCacheDirectory(std::filesystem::path path);

            /// @brief Function used to filter with the different methods on this type
            using DifficultyFilterFunction = std::function<bool(const SongDifficulty&)>;
            /// @brief Method to find the song indexes that satisfy the difficulty find method
            /// @param check the callback ran to check if this difficulty satisfies the condition
            /// @return vector of indexes into the backing song array
            const std::vector<std::size_t> FindSongIndexes(const DifficultyFilterFunction& check) const;
            /// @brief Method to find the songs that satisfy the difficulty find method
            /// @param check the callback ran to check if this difficulty satisfies the condition
            /// @return vector of song pointers, I'd make them references but c++ doesn't allow that
            const std::vector<const Song*> FindSongs(const DifficultyFilterFunction& check) const;
            /// @brief Method to count the songs that satisfy the difficulty find method
            /// @param check the callback ran to check if this difficulty satisfies the condition
            /// @return amount of songs found that satisfy the condition
            std::size_t CountSongs(const DifficultyFilterFunction& check) const;
            /// @brief gets the time from when the last scrape was done
            /// @return unix timestamp of the last scrape end time
            std::chrono::sys_seconds get_scrapeEndedTimeUnix() const;

            /// @brief Invoked when data is available or was updated
            static BSHookUtils::UnorderedEventCallback<> dataAvailableOrUpdated;

            /// @brief Invoked when data failed to load
            static BSHookUtils::UnorderedEventCallback<std::string> dataLoadFailed;

        private:
            SongDetails() noexcept;
            static void DataAvailableOrUpdated();
            static void DataLoadFailed(std::string message);

            friend class SongDetailsContainer;
            static bool isLoading;
            static SongDetails instance;
    };
}
