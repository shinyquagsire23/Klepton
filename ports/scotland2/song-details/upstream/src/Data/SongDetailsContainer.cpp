#include "Data/SongDetailsContainer.hpp"
#include "logging.hpp"
#include "SongDetails.hpp"
#include "Data/DataGetter.hpp"
#include <sys/stat.h>
#include "StopWatch.hpp"

#include "SongProto.pb.h"

namespace SongDetailsCache {
    shared_ptr_vector<uint32_t> SongDetailsContainer::keys;
    shared_ptr_vector<SongHash> SongDetailsContainer::hashBytes;
    shared_ptr_vector<uint32_t> SongDetailsContainer::hashBytesLUT;
    shared_ptr_vector<std::string> SongDetailsContainer::songNames;
    shared_ptr_vector<std::string> SongDetailsContainer::songAuthorNames;
    shared_ptr_vector<std::string> SongDetailsContainer::levelAuthorNames;
    shared_ptr_vector<std::string> SongDetailsContainer::uploaderNames;
    shared_ptr_unordered_map<std::string, uint64_t> SongDetailsContainer::tags;

    std::chrono::sys_seconds SongDetailsContainer::scrapeEndedTimeUnix;
    std::chrono::seconds SongDetailsContainer::updateThrottle = std::chrono::seconds(0);

    shared_ptr_vector<Song> SongDetailsContainer::songs;
    shared_ptr_vector<SongDifficulty> SongDetailsContainer::difficulties;

    BSHookUtils::UnorderedEventCallback<> SongDetailsContainer::dataAvailableOrUpdatedInternal;
    BSHookUtils::UnorderedEventCallback<std::string> SongDetailsContainer::dataLoadFailedInternal;

    std::future<void> SongDetailsContainer::Load(bool reload, int acceptableAgeHours) {
        return std::async(std::launch::async, std::bind(&SongDetailsContainer::Load_internal, reload, acceptableAgeHours));
    }

    void SongDetailsContainer::Load_internal(bool reload, int acceptableAgeHours) {
        using namespace std::chrono;
        using namespace std::chrono_literals;
        struct stat fInfo = {0};

        bool shouldLoadFresh = false;
        if (stat(DataGetter::cachePath().c_str(), &fInfo) == 0) { // only succeeds if exists
            if (!get_isDataAvailable() || reload) {
                try {
                    auto cachedStreamOpt = DataGetter::ReadCachedDatabase();
                    if (cachedStreamOpt) {
                        DEBUG("Processing cached data!");
                        StopWatch sw; sw.Start();
                        Process(*cachedStreamOpt, false);
                        DEBUG("Processed cached data in {}ms", sw.EllapsedMilliseconds());
                        cachedStreamOpt->close();

                        if (!get_isDataAvailable()) {
                            INFO("Failed to load cached data, will try to load fresh data");
                            shouldLoadFresh = true;
                        }
                    }
                } catch (...) {
                    ERROR("Failed to read cached data, will try to load fresh data");
                    shouldLoadFresh = true;
                }
            }

            if (get_isDataAvailable() && std::chrono::system_clock::now().time_since_epoch() - scrapeEndedTimeUnix.time_since_epoch() > std::chrono::hours(std::max(acceptableAgeHours, 1))) {
                shouldLoadFresh = true;
            }
        } else { // didn't exist or otherwise failed
            shouldLoadFresh = true;
        }

        if (!shouldLoadFresh || (system_clock::now().time_since_epoch() - updateThrottle < 30min)) {
            SongDetails::isLoading = false;
            return;
        }

        for (const auto& [src, _] : DataGetter::dataSources) {
            try {
                auto db = DataGetter::UpdateAndReadDatabase(src).get();
                if (!db.has_value()) {
                    break;
                }

                StopWatch sw; sw.Start();
                Process(*db->data);
                DEBUG("Processed new data in {}ms", sw.EllapsedMilliseconds());
                sw.Restart();
                DataGetter::WriteCachedDatabase(*db).wait();
                DEBUG("Wrote data in {}ms", sw.EllapsedMilliseconds());

                if (get_isDataAvailable()) {
                    break;
                }

                ERROR("Data load failed for unknown reason");
            } catch (...) {
                ERROR("Failed to download from source {}", src);
            }
        }

        if (!get_isDataAvailable()) {
            // TODO: Collect the last error
            dataLoadFailedInternal.invoke("DB failed to download");
        }
        SongDetails::isLoading = false;
    }

    void SongDetailsContainer::Process(std::istream& istream, bool force) {
        if (!force && songs) return;
        StopWatch sw; sw.Start();
        Structs::SongDetailsV3 parsedContainer;
        if (!parsedContainer.ParseFromIstream(&istream)) {
            ERROR("Failed to parse Song container from istream!");
            return;
        }

        INFO("Parsed protobuf in {}ms", sw.EllapsedMilliseconds());
        Process(parsedContainer, force);
    }

    void SongDetailsContainer::Process(const std::vector<uint8_t>& data, bool force) {
        if (!force && songs) return;

        StopWatch sw; sw.Start();
        Structs::SongDetailsV3 parsedContainer;
        if (!parsedContainer.ParseFromArray(data.data(), data.size())) {
            ERROR("Failed to parse Song container from data!");
            return;
        }

        INFO("Parsed protobuf in {}ms", sw.EllapsedMilliseconds());
        Process(parsedContainer, force);
    }

    void SongDetailsContainer::Process(const Structs::SongDetailsV3& parsedContainer, bool force) {
        if (!force && songs) return;

        auto& parsedField = parsedContainer.songs();

        if (parsedField.size() == 0) {
            ERROR("Parsing data yielded no songs!");
            return;
        }
        const auto len = parsedField.size();
        if(len>1000000 || parsedContainer.taglist_size()>64 ||
           parsedContainer.songhashes().size()!=static_cast<size_t>(len)*HASH_SIZE_BYTES) {
            ERROR("Rejected invalid SongDetails counts/hash payload");return;
        }
        size_t totalDifficulties = 0;
        for (const auto& song : parsedField) {
            totalDifficulties += song.difficulties_size();
            if (song.difficulties_size() > 255 || totalDifficulties > 8000000) {
                ERROR("Rejected excessive SongDetails difficulty count"); return;
            }
        }
        StopWatch sw; sw.Start();

        std::vector<const Structs::SongV3*> parsed;
        parsed.resize(len);
        INFO("Got {} songs in data", len);
        for (std::size_t idx = 0; const auto& s : parsedField) parsed[idx++] = &s;

        // we run everything with resize so everything is already valid memory
        auto newSongs = make_shared_vec<Song>();
        newSongs->reserve(len);

		auto newKeys = make_shared_vec<uint32_t>();
        newKeys->reserve(len);
		auto newHashes = make_shared_vec<SongHash>();
        newHashes->reserve(len);
		auto newHashesLUT = make_shared_vec<uint32_t>();
        newHashesLUT->reserve(len);

		auto newSongNames = make_shared_vec<std::string>();
        newSongNames->reserve(len);
		auto newSongAuthorNames = make_shared_vec<std::string>();
        newSongAuthorNames->reserve(len);
		auto newLevelAuthorNames = make_shared_vec<std::string>();
        newLevelAuthorNames->reserve(len);
		auto newUploaderNames = make_shared_vec<std::string>();
        newUploaderNames->reserve(len);
        auto newTags = make_shared_unordered_map<std::string, uint64_t>();

        auto newDiffs = make_shared_vec<SongDifficulty>();
        std::size_t diffLen = 0;
        for (auto s : parsed) diffLen += s->difficulties().size();
        newDiffs->reserve(diffLen);
        sw.Restart();
        std::size_t diffIndex = 0;

        // Fill the tags
        auto& parsedTagList = parsedContainer.taglist();
        for (int i = 0; i < parsedTagList.size(); i++) {
            newTags->emplace(parsedTagList[i], 1UL << i);
        }

        // Cast it to a vector of SongHashes
        const auto& songHashesRaw = parsedContainer.songhashes();
        for (std::size_t i = 0; i < len; i++) {
            const auto& parsedSong = parsed[i];
            uint8_t diffCount = std::min(255, parsedSong->difficulties_size());
            const auto& builtSong = newSongs->emplace_back(i, diffIndex, diffCount, parsedSong);
            newKeys->emplace_back(parsedSong->mapid());
            newHashes->emplace_back(std::string(songHashesRaw.data() + i * HASH_SIZE_BYTES, HASH_SIZE_BYTES));

            newSongNames->emplace_back(parsedSong->songname());
            newSongAuthorNames->emplace_back(parsedSong->songauthorname());
            newLevelAuthorNames->emplace_back(parsedSong->levelauthorname());
            // If they're equal, uploaderName is omitted from the dump
            newUploaderNames->emplace_back(parsedSong->has_uploadername() ? parsedSong->uploadername() : parsedSong->levelauthorname());
            if (parsedSong->difficulties().empty()) continue;

            for (const auto& diff : parsedSong->difficulties()) {
                newDiffs->emplace_back(i, &diff);
                diffIndex++;
            }
        }
        INFO("Processed songs in {}ms", sw.EllapsedMilliseconds());
        sw.Restart();
        // we need to sort the hashes, but not the original vector, let's just use pointers because that's more efficient than by copy
        std::vector<const Song*> sortedByHashes{};
        sortedByHashes.resize(len);
        for (std::size_t idx = 0; const auto& s : *newSongs) sortedByHashes[idx++] = &s;
        std::stable_sort(sortedByHashes.begin(), sortedByHashes.end(), [&](const Song* lhs, const Song* rhs){
            return newHashes->operator[](lhs->index).c1 < newHashes->operator[](rhs->index).c1;
        });

        for (const auto& sorted : sortedByHashes) {
            newHashesLUT->emplace_back(sorted->index);
        }

        INFO("Made hashes LUT in {}ms", sw.EllapsedMilliseconds());

        INFO("Finished Processing");
        if (!force && songs) return;

        INFO("Assigning new values");
        scrapeEndedTimeUnix = std::chrono::sys_seconds(std::chrono::seconds(parsedContainer.scrapeendedunix()));
        songs = newSongs;
        keys = newKeys;
        hashBytes = newHashes;
        hashBytesLUT = newHashesLUT;

        songNames = newSongNames;
        songAuthorNames = newSongAuthorNames;
        levelAuthorNames = newLevelAuthorNames;
        uploaderNames = newUploaderNames;
        tags = newTags;

        difficulties = newDiffs;

        if (get_isDataAvailable()) {
            try {
                dataAvailableOrUpdatedInternal.invoke();
            } catch (const std::exception& _) {}
        }
    }

    template<class T> static T HashPiece(const std::string& str, size_t offset) {
        T value = 0;
        if (str.size() == 20) std::memcpy(&value, str.data() + offset, sizeof value);
        return value;
    }
    SongHash::SongHash(const std::string& str) : c1(HashPiece<uint32_t>(str,0)), c2(HashPiece<uint64_t>(str,4)), c3(HashPiece<uint64_t>(str,12)) {}
    SongHash::SongHash() : c1(0), c2(0), c3(0) {}
}