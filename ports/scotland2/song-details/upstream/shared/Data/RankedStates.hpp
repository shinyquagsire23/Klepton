#pragma once

#include "../_config.h"
#include <vector>

namespace SongDetailsCache {

    /// @brief Enum describing what ranked states a map can be in
    enum class SONGDETAILS_EXPORT RankedStates {
        Unranked =             0,
        ScoresaberRanked =     1 << 0,
        SR =                   1 << 0, // alias
        BeatleaderRanked =     1 << 1,
        BR =                   1 << 1, // alias
        ScoresaberQualified =  1 << 2,
        SQ =                   1 << 2, // alias
        BeatleaderQualified =  1 << 3,
        BQ =                   1 << 3, // alias

    };

    static constexpr RankedStates operator |(const RankedStates& lhs, const RankedStates& rhs) {
        return static_cast<RankedStates>(static_cast<int>(lhs) | static_cast<int>(rhs));
    }
    static constexpr RankedStates& operator |=(RankedStates& lhs, const RankedStates& rhs) {
        lhs = lhs | rhs;
        return lhs;
    }

    static constexpr RankedStates operator &(const RankedStates& lhs, const RankedStates& rhs) {
        return static_cast<RankedStates>(static_cast<int>(lhs) & static_cast<int>(rhs));
    }
    static constexpr  RankedStates& operator &=(RankedStates& lhs, const RankedStates& rhs) {
        lhs = lhs & rhs;
        return lhs;
    }

    static constexpr  bool hasFlags(const RankedStates& lhs, const RankedStates& rhs) {
        return (lhs & rhs) == rhs;
    }

    static std::vector<std::string> toVectorOfStrings(const RankedStates& states) {
        std::vector<std::string> result{};
        if (hasFlags(states, RankedStates::BeatleaderRanked)) result.emplace_back("Beatleader Ranked");
        if (hasFlags(states, RankedStates::BeatleaderQualified)) result.emplace_back("Beatleader Qualified");
        if (hasFlags(states, RankedStates::ScoresaberQualified)) result.emplace_back("Scoresaber Qualified");
        if (hasFlags(states, RankedStates::ScoresaberRanked)) result.emplace_back("Scoresaber Ranked");
        return result;
    }
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
#include <sstream>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::RankedStates> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::RankedStates s, FormatContext& ctx) const {

        std::string result = "";
        if (hasFlags(s, SongDetailsCache::RankedStates::BeatleaderRanked) ) {
            result += "BeatleaderRanked";
        }
        if (hasFlags(s, SongDetailsCache::RankedStates::ScoresaberRanked)) {
            if (!result.empty()) {
                result += " | ";
            }
            result += "ScoresaberRanked";
        }
        if (hasFlags(s, SongDetailsCache::RankedStates::ScoresaberQualified) ) {
            if (!result.empty()) {
                result += " | ";
            }
            result += "ScoresaberQualified";
        }
        if (hasFlags(s, SongDetailsCache::RankedStates::BeatleaderQualified)) {
            if (!result.empty()) {
                result += " | ";
            }
            result += "BeatleaderQualified";
        }
        if (result.empty()) {
            result = "Unranked";
        }

        return formatter<string_view>::format(result, ctx);
    }
};
#endif
