#pragma once

#include "custom-types/shared/macros.hpp"
#include "Zenject/Installer.hpp"

DECLARE_CLASS_CODEGEN(Lapiz::Installers, LapizGameCoreInstaller, ::Zenject::Installer) {
    DECLARE_OVERRIDE_METHOD_MATCH(void, InstallBindings, &::Zenject::InstallerBase::InstallBindings);
    DECLARE_DEFAULT_CTOR();
};