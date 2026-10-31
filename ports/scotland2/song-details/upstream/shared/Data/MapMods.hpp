#pragma once

#include "../_config.h"
#include <vector>
namespace SongDetailsCache {
    /// @brief Enum describing common mods that maps tend to use
    enum class SONGDETAILS_EXPORT MapMods {
        None =              0x0,
        NoodleExtensions =  0x1,
        NE =                0x1, // alias
        MappingExtensions = 0x2,
        ME =                0x2, // alias
        Chroma =            0x4,
        Cinema =            0x8,
        Vivify =            0x10,
        VI =                0x10, // alias

        ME_NE = ME | NE,
        Chroma_NE = Chroma | NE,
        Chroma_ME = Chroma | ME,
        Chroma_ME_NE = Chroma | ME | NE,
        Cinema_NE = Cinema | NE,
        Cinema_ME = Cinema | ME,
        Cinema_ME_NE = Cinema | ME | NE,
        Cinema_Chroma = Cinema | Chroma,
        Cinema_Chroma_NE = Cinema | Chroma | NE,
        Cinema_Chroma_ME = Cinema | Chroma | ME,
        Cinema_Chroma_ME_NE = Cinema | Chroma | ME | NE,
        Chroma_NE_VI = Chroma | NE | Vivify,
        Chroma_VI = Chroma | Vivify,
    };

    static constexpr MapMods operator |(const MapMods& lhs, const MapMods& rhs) {
        return static_cast<MapMods>(static_cast<int>(lhs) | static_cast<int>(rhs));
    }
    static constexpr MapMods& operator |=(MapMods& lhs, const MapMods& rhs) {
        lhs = lhs | rhs;
        return lhs;
    }

    static constexpr MapMods operator &(const MapMods& lhs, const MapMods& rhs) {
        return static_cast<MapMods>(static_cast<int>(lhs) & static_cast<int>(rhs));
    }
    static constexpr  MapMods& operator &=(MapMods& lhs, const MapMods& rhs) {
        lhs = lhs & rhs;
        return lhs;
    }

    static constexpr  bool hasFlags(const MapMods& lhs, const MapMods& rhs) {
        return (lhs & rhs) == rhs;
    }

    static std::vector<std::string> toVectorOfStrings(const MapMods& mods) {
        std::vector<std::string> result{};
        if (hasFlags(mods, MapMods::NoodleExtensions)) result.emplace_back("Noodle Extensions");
        if (hasFlags(mods, MapMods::MappingExtensions)) result.emplace_back("Mapping Extensions");
        if (hasFlags(mods, MapMods::Chroma)) result.emplace_back("Chroma");
        if (hasFlags(mods, MapMods::Cinema)) result.emplace_back("Cinema");
        if (hasFlags(mods, MapMods::Vivify)) result.emplace_back("Vivify");
        return result;
    }
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::MapMods> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::MapMods c, FormatContext& ctx) const {
        switch (c) {
            case SongDetailsCache::MapMods::None:
                return formatter<string_view>::format("None", ctx);;
            case SongDetailsCache::MapMods::NE:
                return formatter<string_view>::format("Noodle Extensions", ctx);
            case SongDetailsCache::MapMods::ME:
                return formatter<string_view>::format("Mapping Extensions", ctx);
            case SongDetailsCache::MapMods::ME_NE:
                return formatter<string_view>::format("Noodle Extensions, Mapping Extensions", ctx);
            case SongDetailsCache::MapMods::Chroma:
                return formatter<string_view>::format("Chroma", ctx);;
            case SongDetailsCache::MapMods::Chroma_NE:
                return formatter<string_view>::format("Noodle Extensions, Chroma", ctx);
            case SongDetailsCache::MapMods::Chroma_ME:
                return formatter<string_view>::format("Mapping Extensions, Chroma", ctx);
            case SongDetailsCache::MapMods::Chroma_ME_NE:
                return formatter<string_view>::format("Noodle Extensions, Mapping Extensions, Chroma", ctx);
            case SongDetailsCache::MapMods::Cinema:
                return formatter<string_view>::format("Cinema", ctx);;
            case SongDetailsCache::MapMods::Cinema_NE:
                return formatter<string_view>::format("Noodle Extensions, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_ME:
                return formatter<string_view>::format("Mapping Extensions, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_ME_NE:
                return formatter<string_view>::format("Noodle Extensions, Mapping Extensions, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_Chroma:
                return formatter<string_view>::format("Chroma, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_Chroma_NE:
                return formatter<string_view>::format("Noodle Extensions, Chroma, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_Chroma_ME:
                return formatter<string_view>::format("Mapping Extensions, Chroma, Cinema", ctx);
            case SongDetailsCache::MapMods::Cinema_Chroma_ME_NE:
                return formatter<string_view>::format("Noodle Extensions, Mapping Extensions, Chroma, Cinema", ctx);
            case SongDetailsCache::MapMods::Chroma_NE_VI:
                return formatter<string_view>::format("Noodle Extensions, Chroma, Vivify", ctx);
            case SongDetailsCache::MapMods::Vivify:
                return formatter<string_view>::format("Vivify", ctx);
            case SongDetailsCache::MapMods::Chroma_VI:
                return formatter<string_view>::format("Chroma, Vivify", ctx);
        }
    }
};
#endif
