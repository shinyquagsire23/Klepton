#pragma once

#include "../../_config.h"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"

namespace BSML {
    enum class BSML_EXPORT Side {
        Left,
        Right,
        Bottom,
        Top,
        Full
    };
}

// il2cpp class definition to use the enum as an int to C# stuff
template<>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<BSML::Side> {
    static inline Il2CppClass* get() {
        return classof(int);
    }
};
static_assert(sizeof(BSML::Side) == sizeof(int));

// if we have fmt, define the formatting methods for the handle enum
#if __has_include("fmt/core.h")
#include <fmt/core.h>
#include <string_view>

template <> struct fmt::formatter<::BSML::Side> : formatter<string_view> {
    // parse is inherited from formatter<string_view>.
    template <typename FormatContext>
    auto format(::BSML::Side s, FormatContext& ctx) {
        // return the specific case if possible
        switch (s) {
            case ::BSML::Side::Left:
                return formatter<string_view>::format("Left", ctx);
            case ::BSML::Side::Right:
                return formatter<string_view>::format("Right", ctx);
            case ::BSML::Side::Bottom:
                return formatter<string_view>::format("Bottom", ctx);
            case ::BSML::Side::Top:
                return formatter<string_view>::format("Top", ctx);
            case ::BSML::Side::Full:
                return formatter<string_view>::format("Full", ctx);
            default:
                return formatter<string_view>::format("Unknown", ctx);
        }
    }
};
#endif
