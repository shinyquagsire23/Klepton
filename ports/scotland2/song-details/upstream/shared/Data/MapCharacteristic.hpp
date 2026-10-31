#pragma once

#include "../_config.h"
#include <string_view>

namespace SongDetailsCache {
    /// @brief Enum describing Map Characteristics
    enum class SONGDETAILS_EXPORT MapCharacteristic {
        Custom = 0,
        Standard = 1,
        OneSaber = 2,
        NoArrows = 3,
        NinetyDegree = 4,
        ThreeSixtyDegree = 5,
        LightShow = 6,
        Lawless = 7,
    };

    /// @brief parse a mapcharacteristic
    /// @return true if parse was successful
    bool parse(std::string_view str, MapCharacteristic& out);
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::MapCharacteristic> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::MapCharacteristic c, FormatContext& ctx) const {
        // return the specific case if possible
        switch (c) {
            case ::SongDetailsCache::MapCharacteristic::Custom:
                return formatter<string_view>::format("Custom", ctx);
            case ::SongDetailsCache::MapCharacteristic::Standard:
                return formatter<string_view>::format("Standard", ctx);
            case ::SongDetailsCache::MapCharacteristic::OneSaber:
                return formatter<string_view>::format("OneSaber", ctx);
            case ::SongDetailsCache::MapCharacteristic::NoArrows:
                return formatter<string_view>::format("NoArrows", ctx);
            case ::SongDetailsCache::MapCharacteristic::NinetyDegree:
                return formatter<string_view>::format("NinetyDegree", ctx);
            case ::SongDetailsCache::MapCharacteristic::ThreeSixtyDegree:
                return formatter<string_view>::format("ThreeSixtyDegree", ctx);
            case ::SongDetailsCache::MapCharacteristic::LightShow:
                return formatter<string_view>::format("LightShow", ctx);
            case ::SongDetailsCache::MapCharacteristic::Lawless:
                return formatter<string_view>::format("Lawless", ctx);
            default:
                return formatter<string_view>::format("Unknown", ctx);
        }
    }
};
#endif
