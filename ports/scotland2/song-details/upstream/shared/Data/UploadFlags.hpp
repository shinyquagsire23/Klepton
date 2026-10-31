#pragma once

#include "../_config.h"
#include <vector>

namespace SongDetailsCache {

    /// @brief Enum describing what ranked states a map can be in
    enum class SONGDETAILS_EXPORT UploadFlags {
        None =                 0,
        Curated =              1 << 0,
        VerifiedUploader =     1 << 1,
        HasV3Environment =     1 << 2,
    };

    static constexpr UploadFlags operator |(const UploadFlags& lhs, const UploadFlags& rhs) {
        return static_cast<UploadFlags>(static_cast<int>(lhs) | static_cast<int>(rhs));
    }
    static constexpr UploadFlags& operator |=(UploadFlags& lhs, const UploadFlags& rhs) {
        lhs = lhs | rhs;
        return lhs;
    }

    static constexpr UploadFlags operator &(const UploadFlags& lhs, const UploadFlags& rhs) {
        return static_cast<UploadFlags>(static_cast<int>(lhs) & static_cast<int>(rhs));
    }
    static constexpr  UploadFlags& operator &=(UploadFlags& lhs, const UploadFlags& rhs) {
        lhs = lhs & rhs;
        return lhs;
    }

    static constexpr  bool hasFlags(const UploadFlags& lhs, const UploadFlags& rhs) {
        return (lhs & rhs) == rhs;
    }

    static std::vector<std::string> toVectorOfStrings(const UploadFlags& states) {
        std::vector<std::string> result{};
        if (hasFlags(states, UploadFlags::None)) result.emplace_back("None");
        if (hasFlags(states, UploadFlags::Curated)) result.emplace_back("Curated");
        if (hasFlags(states, UploadFlags::VerifiedUploader)) result.emplace_back("VerifiedUploader");
        if (hasFlags(states, UploadFlags::HasV3Environment)) result.emplace_back("HasV3Environment");
        return result;
    }
}

// if we have fmt, add formatting methods
#if __has_include("fmt/core.h")
#include <fmt/core.h>
#include <sstream>
template <> struct SONGDETAILS_EXPORT fmt::formatter<::SongDetailsCache::UploadFlags> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::SongDetailsCache::UploadFlags s, FormatContext& ctx) const {

        std::string result;
        if (hasFlags(s, SongDetailsCache::UploadFlags::Curated) ) {
            result += "Curated";
        }
        if (hasFlags(s, SongDetailsCache::UploadFlags::VerifiedUploader) ) {
            if (!result.empty()) {
                result += " | ";
            }
            result += "VerifiedUploader";
        }
        if (hasFlags(s, SongDetailsCache::UploadFlags::HasV3Environment) ) {
            if (!result.empty()) {
                result += " | ";
            }
            result += "HasV3Environment";
        }
        if (result.empty()) {
            result = "None";
        }

        return formatter<string_view>::format(result, ctx);
    }
};
#endif
