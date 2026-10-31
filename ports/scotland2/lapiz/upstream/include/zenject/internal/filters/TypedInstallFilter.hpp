#pragma once

#include "IInstallFilter.hpp"

namespace Lapiz::Zenject::Internal::Filters {
    class TypedInstallFilter : public IInstallFilter {
        public:
            TypedInstallFilter(Il2CppClass* installerType) : _installerType(installerType) {}
            bool ShouldInstall(ContextBinding* binding) const override {
                return _installerType == binding->get_installerType();
            }
        private:
            const Il2CppClass* _installerType;
    };
}