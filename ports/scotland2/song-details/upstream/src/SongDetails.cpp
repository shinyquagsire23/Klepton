#include "SongDetails.hpp"
#include "Data/Song.hpp"
#include "Data/DataGetter.hpp"
#include "Data/SongDetailsContainer.hpp"

namespace SongDetailsCache {
    SongDetails SongDetails::instance{};
    BSHookUtils::UnorderedEventCallback<> SongDetails::dataAvailableOrUpdated;
    BSHookUtils::UnorderedEventCallback<std::string> SongDetails::dataLoadFailed;

    bool ::SongDetailsCache::SongDetails::isLoading = false;

    SongDetails::SongDetails() noexcept {
        SongDetailsContainer::dataAvailableOrUpdatedInternal += {&SongDetails::DataAvailableOrUpdated};
        SongDetailsContainer::dataLoadFailedInternal += {&SongDetails::DataLoadFailed};
    }

    std::future<SongDetails*> SongDetails::Init() { return Init(3); }
    std::future<SongDetails*> SongDetails::Init(int refreshIfOlderThanHours) {
        if (!isLoading) {
            isLoading = true;
            // essentially dispatches a load thread through std::async with launc::async
            SongDetailsContainer::Load(false, refreshIfOlderThanHours);
        }
        return std::async(std::launch::deferred, []{
            while(!SongDetailsContainer::get_isDataAvailable() && isLoading)
                std::this_thread::yield();
            return &instance;
        });
    }

    void SongDetails::DataAvailableOrUpdated() {
        instance.dataAvailableOrUpdated.invoke();
    }

    void SongDetails::DataLoadFailed(std::string message) {
        instance.dataLoadFailed.invoke(message);
    }

    void SongDetails::SetCacheDirectory(std::filesystem::path path) {
        std::filesystem::create_directories(path);
        DataGetter::basePath = path;
    }

    const std::vector<std::size_t> SongDetails::FindSongIndexes(const DifficultyFilterFunction& check) const {
        std::vector<std::size_t> l;
        auto& diffs = *SongDetailsContainer::difficulties;
        std::size_t sz = diffs.size();
        for (std::size_t i = 0, last = std::numeric_limits<uint32_t>::max(); i < sz; i++) {
            auto& cur = diffs[i];
            if (last == cur.songIndex || !check(cur))
                continue;
            last = l.emplace_back(cur.songIndex);
        }
        return l;
    }
    const std::vector<const Song*> SongDetails::FindSongs(const DifficultyFilterFunction& check) const {
        std::vector<const Song*> l;
        auto& diffs = *SongDetailsContainer::difficulties;
        auto& songs = *SongDetailsContainer::songs;
        std::size_t sz = diffs.size();
        for (std::size_t i = 0, last = std::numeric_limits<uint32_t>::max(); i < sz; i++) {
            auto& cur = diffs[i];
            if (last == cur.songIndex || !check(cur))
                continue;
            last = cur.songIndex;
            l.emplace_back(&songs[cur.songIndex]);
        }
        return l;
    }
    std::size_t SongDetails::CountSongs(const DifficultyFilterFunction& check) const {
        std::size_t count = 0;
        auto& diffs = *SongDetailsContainer::difficulties;
        std::size_t sz = diffs.size();
        for (std::size_t i = 0, last = std::numeric_limits<uint32_t>::max(); i < sz; i++) {
            auto& cur = diffs[i];
            if (last == cur.songIndex || !check(cur))
                continue;
            count++;
            last = cur.songIndex;
        }
        return count;
    }
    std::chrono::sys_seconds SongDetails::get_scrapeEndedTimeUnix() const {
        return SongDetailsContainer::scrapeEndedTimeUnix;
    }
}
