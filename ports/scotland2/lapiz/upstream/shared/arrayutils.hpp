#pragma once

#include "System/Object.hpp"
#include "System/Type.hpp"

namespace Lapiz::ArrayUtils {
    template<typename T>
    requires(std::is_convertible<T, System::Object*>::value || std::is_arithmetic<T>::value)
    static inline void box_array(ArrayW<System::Object*> arr, int index, T val)  {
        if constexpr (std::is_pointer_v<T>) arr[index] = val;
        else arr[index] = il2cpp_functions::value_box(classof(T), &val);
    }

    template <typename First, typename... Rest>
    requires(std::is_convertible<First, System::Object*>::value || std::is_arithmetic<First>::value)
    static inline void box_array(ArrayW<System::Object*> arr, int index, First& first, Rest&... rest) {
        if constexpr (std::is_pointer_v<First>) arr[index] = first;
        else arr[index] = il2cpp_functions::value_box(classof(First), &first);

        box_array(arr, ++index, rest...);
    }

    template <typename First, typename... Rest>
    requires(std::is_convertible<First, System::Object*>::value || std::is_arithmetic<First>::value)
    static inline void box_array(ArrayW<System::Object*> arr, First& first, Rest&... rest) {
        box_array(arr, 0, first, rest...);
    }

    template<typename... Targs>
    static inline ArrayW<System::Object*> box_array(Targs&... rest) {
        auto res = ArrayW<System::Object*>(sizeof...(rest));
        box_array(res, rest...);
        return res;
    }

    template<typename T, typename... Targs>
    static void inline SetIndexType(ArrayW<System::Type*> arr, int index) {
        arr[index] = csTypeOf(T);
        if constexpr (sizeof...(Targs) > 0) SetIndexType<Targs...>(arr, ++index);
    }

    template<typename... Targs>
    static inline ArrayW<System::Type*> TypeArray() {
        auto res = ArrayW<System::Type*>(sizeof...(Targs));
        if constexpr (sizeof...(Targs) > 0) SetIndexType<Targs...>(res, 0);
        return res;
    }
}
