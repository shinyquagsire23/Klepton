#pragma once

#include "RankedStates.hpp"

#include "../_config.h"

namespace SongDetailsCache {
    enum class SONGDETAILS_EXPORT RankedStatus {
        Unranked = 0,
        Ranked = 1,
        Qualified = 2,
        Queued = 3,
    };

    static RankedStatus RankedStatusFromRankedStates(RankedStates states) {
        if (hasFlags(states, RankedStates::ScoresaberRanked)) return RankedStatus::Ranked;
        if (hasFlags(states, RankedStates::ScoresaberQualified)) return RankedStatus::Qualified;
        return RankedStatus::Unranked;
    }
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
#include <string_view>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::RankedStatus> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::RankedStatus s, FormatContext& ctx) const {
        // return the specific case if possible
        switch (s) {
            case SongDetailsCache::RankedStatus::Unranked:
                return formatter<string_view>::format("Unranked", ctx);
            case SongDetailsCache::RankedStatus::Ranked:
                return formatter<string_view>::format("Ranked", ctx);
            case SongDetailsCache::RankedStatus::Qualified:
                return formatter<string_view>::format("Qualified", ctx);
            case SongDetailsCache::RankedStatus::Queued:
                return formatter<string_view>::format("Queued", ctx);
            default:
                return formatter<string_view>::format("Unknown", ctx);
        }
    }
};
#endif
