#pragma once

#include "../_config.h"
#include "MapCharacteristic.hpp"
#include "MapDifficulty.hpp"
#include "MapMods.hpp"
#include <stdint.h>

namespace SongDetailsCache {
    namespace Structs {
        struct SongDifficulty;
    };
    struct SONGDETAILS_EXPORT Song;
    struct SONGDETAILS_EXPORT SongDifficulty {
        public:
            static const SongDifficulty none;
            /// @brief Scoresaber difficulty rating of this difficulty
            const float starsSS;
            /// @brief BeatLeader difficulty rating of this difficulty
            const float starsBL;
            /// @brief NJS (Note Jump Speed) of this difficulty
            const float njs;
            /// @brief Amount of bombs in this Difficulty
            const uint32_t bombs;
            /// @brief Amount of notes in this Difficulty
            const uint32_t notes;
            /// @brief Amount of obstacles in this Difficulty
            const uint32_t obstacles;
            /// @brief Characteristic of this Difficulty
            const MapCharacteristic characteristic;
            /// @brief Map Difficulty of this Difficulty
            const MapDifficulty difficulty;
            /// @brief Mods required for this difficulty
            const MapMods mods;

            /// @brief The song this Difficulty belongs to
            const Song& song() const noexcept;

            /// @brief Returns if the Difficulty is ranked on Scoresaber or BeatLeader
            bool ranked() const noexcept;

            /// @brief Returns if the Difficulty is ranked on BeatLeader
            bool rankedBL() const noexcept;

            /// @brief Returns if the Difficulty is ranked on Scoresaber
            bool rankedSS() const noexcept;

            /// @brief shorthand to check the mods enum
            bool usesMods(const MapMods& usedMods) const noexcept;

            /// @brief check if this difficulty is the same as the other, by checking the pointer. Since we do not allow copy construction we can just compare pointers (test pls)
            /// @param other the SongDifficulty to check against
            /// @return equivalency
            bool operator ==(const SongDifficulty& other) const noexcept {
                return this == &other;
            }

            /// @brief checks if this difficulty is the same as none
            inline operator bool() {
                return this != &none;
            }

            /// @brief default move ctor
            SongDifficulty(SongDifficulty&&) = default;
            /// @brief delete copy ctor
            SongDifficulty(const SongDifficulty&) = delete;

            /// @brief This needs to be public for specific reasons, but it's not advised to make your own SongDifficulties
            SongDifficulty(std::size_t songIndex, const Structs::SongDifficulty* proto) noexcept;
        private:
            friend class SongDetailsContainer;
            friend class SongDetails;
            friend struct Song;
            const std::size_t songIndex;
    };
}
