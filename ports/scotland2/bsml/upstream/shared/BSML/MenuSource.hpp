#pragma once

#include "../_config.h"
#include "beatsaber-hook/shared/utils/il2cpp-type-check.hpp"

namespace BSML {
    enum class BSML_EXPORT MenuSource {
        BSMLContent,
        FlowCoordinator,
        ViewController,
        Method,
        Component,
    };
}

template<>
struct ::il2cpp_utils::il2cpp_type_check::il2cpp_no_arg_class<BSML::MenuSource> {
    static inline Il2CppClass* get() {
        return classof(int);
    }
};
static_assert(sizeof(BSML::MenuSource) == sizeof(int));
