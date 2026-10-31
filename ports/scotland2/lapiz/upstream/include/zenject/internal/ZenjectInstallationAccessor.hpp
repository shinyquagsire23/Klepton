#pragma once

#include "Zenject/MonoInstaller.hpp"
#include "Zenject/InstallerBase.hpp"
#include "System/Type.hpp"

namespace Lapiz::Zenject::Internal {
    class ZenjectInstallationAccessor {
        public:
            ZenjectInstallationAccessor(ListW<::Zenject::InstallerBase*> normalInstallers, ListW<System::Type*> normalInstallerTypes, ListW<::Zenject::MonoInstaller*> installers) 
            : _installers(installers), _normalInstallers(normalInstallers), _normalInstallerTypes(normalInstallerTypes) {};
        
            auto get_installers() const { return _installers; }
            auto get_normalInstallers() const { return _normalInstallers; }
            auto get_normalInstallerTypes() const { return _normalInstallerTypes; }
        
        private:
            ListW<::Zenject::MonoInstaller*> _installers;
            ListW<::Zenject::InstallerBase*> _normalInstallers;
            ListW<System::Type*> _normalInstallerTypes;
    };
}
