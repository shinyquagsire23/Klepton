#pragma once

#include "../_config.h"
#include "SongDifficulty.hpp"
#include "RankedStatus.hpp"
#include "RankedStates.hpp"
#include "UploadFlags.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <vector>
#include <cmath>

namespace SongDetailsCache {
    namespace Structs {
        struct SongV3;
    }
    struct SONGDETAILS_EXPORT Song {
        public:
            /// @brief bpm of this map
            const float bpm;
            /// @brief upvotes of this map
            const uint32_t upvotes;
            /// @brief downvotes of this map
            const uint32_t downvotes;
            /// @brief Upload flags of this map
            const UploadFlags uploadFlags;
            /// @brief Raw tags of this map
            const uint64_t tags;

            /// @brief get the minimum of some value you want to know the minimum of for all the diffs of this song
            /// @param func the lambda that returns the value
            /// @return lowest value found
            float min(std::function<float(const SongDifficulty&)> func) const;
            /// @brief get the maximum of some value you want to know the maximum of for all the diffs of this song
            float max(std::function<float(const SongDifficulty&)> func) const;

            /// @brief calculates the rating for this map based on the downvotes and upvotes
            float rating() const noexcept;
            /// @brief Gets the minimum njs for all diffs of this song
            float minNJS() const noexcept;
            /// @brief Gets the maximum njs for all diffs of this song
            float maxNJS() const noexcept;
            /// @brief Gets the minimum star value in all diffs of this song for BeatLeader
            float minStarBL() const noexcept;
            /// @brief Gets the maximum star value in all diffs of this song for BeatLeader
            float maxStarBL() const noexcept;
            /// @brief Gets the minimum star value in all diffs of this song for Scoresaber
            float minStarSS() const noexcept;
            /// @brief Gets the maximum star value in all diffs of this song for Scoresaber
            float maxStarSS() const noexcept;
            /// @brief Gets the maximum pp value in all diffs of this song
            [[deprecated("This function is deprecated. It only works for scoresaber ranked difficulties and is not accurate.")]]
            float minPP() const noexcept;
            /// @brief Gets the maximum pp value in all diffs of this song
            [[deprecated("This function is deprecated. It only works for scoresaber ranked difficulties and is not accurate.")]]
            float maxPP() const noexcept;

            /// @brief Unix timestamp of when the map was uploaded
            const uint32_t uploadTimeUnix;
            /// @brief Unix timestamp of when any of the difficulties of this map changed its ranked status
            const uint32_t rankedChangeUnix;

            /// @brief upload time as system time
            std::chrono::sys_time<std::chrono::seconds> uploadTime() const noexcept;

            /// @brief Duration of this song in seconds
            const uint32_t songDurationSeconds;
            /// @brief song duration as chrono seconds
            std::chrono::seconds songDuration() const noexcept;
            /// @brief Index of the Song in the Songs array
            const std::size_t index;
            /// @brief Index of the first difficulty of this song in the difficulties array
            const std::size_t diffOffset;
            /// @brief Amount of difficulties this song has
            const uint8_t diffCount;

            /// @return Hexadecimal representation of the Map ID
            std::string key() const noexcept;
            /// @return Numeric representation of the Map ID
            uint32_t mapId() const noexcept;

            /// @brief Ranked status of the map on ScoreSaber
            const RankedStatus rankedStatus;
            /// @brief Ranked state of the map on both ScoreSaber and BeatLeader (bitwise mask)
            const RankedStates rankedStates;

            /// @return Hexadecimal representation of the Map Hash
            std::string hash() const noexcept;

            /// @return Song name of this song
            const std::string& songName() const noexcept;
            /// @return Song author name of this song
            const std::string& songAuthorName() const noexcept;
            /// @return Level author name of this song
            const std::string& levelAuthorName() const noexcept;
            /// @return Uploader name of this song
            const std::string& uploaderName() const noexcept;

            /// @return Cover url on beatsaver
            std::string coverURL() const noexcept;

            /// @brief check if this song is the same as the other, by checking the pointer. Since we do not allow copy construction we can just compare pointers (test pls)
            /// @param other the Song to check against
            /// @return equivalency
            bool operator ==(const Song& other) const noexcept {
                return this == &other;
            }

            /// @brief checks if this song is the same as none
            inline operator bool() const noexcept {
                return this != &none;
            }

            /// @brief Helper method to check if the Song has a tag set
            /// @param tag string representation of the BeatSaver Tag
            /// @return whether the tag is set
            bool HasTag(std::string_view tag) const noexcept;
            /// @brief Helper function to get a difficulty from this song
            /// @param outDiff a reference to the output pointer
            /// @param diff the difficulty to search for
            /// @param characteristic the characteristic to search for
            /// @return whether it was found
            bool GetDifficulty(const SongDifficulty*& outDiff, MapDifficulty diff, MapCharacteristic characteristic = MapCharacteristic::Standard) const noexcept;
            /// @brief Helper function to get a difficulty from this song
            /// @param outDiff a reference to the output pointer
            /// @param diff the difficulty to search for
            /// @param characteristic the characteristic to search for as a string
            /// @return whether it was found
            bool GetDifficulty(const SongDifficulty*& outDiff, MapDifficulty diff, std::string_view characteristic) const noexcept;
            /// @brief Helper function to get a difficulty from this song
            /// @param diff the difficulty to search for
            /// @param characteristic the characteristic to search for
            /// @return The found SongDifficulty, otherwise SongDifficulty::none
            const SongDifficulty& GetDifficulty(MapDifficulty diff, MapCharacteristic characteristic = MapCharacteristic::Standard) const noexcept;
            /// @brief Helper function to get a difficulty from this song
            /// @param diff the difficulty to search for
            /// @param characteristic the characteristic to search for as a string
            /// @return The found SongDifficulty, otherwise SongDifficulty::none
            const SongDifficulty& GetDifficulty(MapDifficulty diff, std::string_view characteristic) const noexcept;

            // allow iterating all difficulties of this song in a regular foreach loop
            using difficulty_const_iterator = std::vector<SongDifficulty>::const_iterator;
            difficulty_const_iterator begin() const noexcept;
            difficulty_const_iterator end() const noexcept;
            /// @brief none song, exists to provide an "invalid" song
            static const Song none;
            /// @brief default move ctor
            Song(Song&&) = default;
            /// @brief delete copy constructor
            Song(const Song&) = delete;
            /// @brief this needs to be public for specific reasons, but it's not advised to make your own SongDetail::Songs!
            Song(std::size_t index, std::size_t diffOffset, uint8_t diffCount, const Structs::SongV3* proto) noexcept;
        private:
            friend class SongDetailsContainer;
            friend class SongDetails;
    };
}
