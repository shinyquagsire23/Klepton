#pragma once

#include "IInstallFilter.hpp"

namespace Lapiz::Zenject::Internal::Filters {
    class MultiTypedInstallFilter : public IInstallFilter {
        public:
            MultiTypedInstallFilter(const std::unordered_set<Il2CppClass*>& baseInstallerTypes) : _baseInstallerTypes(baseInstallerTypes) {}
            bool ShouldInstall(ContextBinding* binding) const override {
                auto itr = std::find(_baseInstallerTypes.begin(), _baseInstallerTypes.end(), binding->get_installerType());
                return itr != _baseInstallerTypes.end();
            }
        private:
            const std::unordered_set<Il2CppClass*> _baseInstallerTypes;
    };
}