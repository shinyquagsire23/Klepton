#pragma once

#include "../_config.h"

namespace SongDetailsCache {
    /// @brief Enum describing Map Difficulties
    enum class SONGDETAILS_EXPORT MapDifficulty {
        Easy = 0,
        Normal = 1,
        Hard = 2,
        Expert = 3,
        ExpertPlus = 4,
    };
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
#include <string_view>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::MapDifficulty> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::MapDifficulty c, FormatContext& ctx) const {
        // return the specific case if possible
        switch (c) {
            case SongDetailsCache::MapDifficulty::Easy:
                return formatter<string_view>::format("Easy", ctx);
            case SongDetailsCache::MapDifficulty::Normal:
                return formatter<string_view>::format("Normal", ctx);
            case SongDetailsCache::MapDifficulty::Hard:
                return formatter<string_view>::format("Hard", ctx);
            case SongDetailsCache::MapDifficulty::Expert:
                return formatter<string_view>::format("Expert", ctx);
            case SongDetailsCache::MapDifficulty::ExpertPlus:
                return formatter<string_view>::format("ExpertPlus", ctx);
            default:
                return formatter<string_view>::format("Unknown", ctx);
        }
    }
};
#endif
