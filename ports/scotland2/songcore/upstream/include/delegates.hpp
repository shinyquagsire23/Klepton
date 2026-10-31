#pragma once

#include "custom-types/shared/delegate.hpp"

template<typename... T> void CreateDelegate(const std::function<(void)>& func) {
    return custom_types::MakeDelegate<T>(func);
}
