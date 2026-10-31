#pragma once

#include "beatsaber-hook/shared/utils/typedefs.h"

namespace TypeUtil {

    /// @brief recursively check the instance for equality to ancestor, if not found check parent
    /// @param instance the type to check ancestors for
    /// @param ancestor the type to find
    /// @return true if found, false if not found
    static bool hasAncestor(const Il2CppClass* instance, const Il2CppClass* ancestor) {
        if (!instance) return false;
        if (instance == ancestor) return true;
        return hasAncestor(instance->parent, ancestor);
    }

    /// @brief checks the passed instance klass for any parents that are of klass tparam
    /// @tparam T the type to check for
    /// @param instance the type to check ancestors for
    /// @return true if found, false if not found
    template<typename T>
    requires(std::is_convertible_v<T, System::Object*>)
    static bool hasAncestor(const Il2CppClass* instance) {
        return hasAncestor(instance, classof(T));
    }

    template<typename T, typename Arr = ArrayW<T>>
    static Arr AppendArrayOrDefault(Arr orig, T item = T{}) {
    auto res = Arr(orig.size() + 1);
    memcpy(res.begin(), orig.begin(), sizeof(T) * orig.size());
    res[orig.size()] = item;
    return res;
}
}