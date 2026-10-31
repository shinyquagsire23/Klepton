#pragma once

#include "Zenject/IInstaller.hpp"
#include <type_traits>


namespace Lapiz::concepts {
    template<typename T>
    concept IInstaller = requires(T t) {
        { t->operator ::Zenject::IInstaller*() } -> std::same_as<::Zenject::IInstaller*>;
    };
}
