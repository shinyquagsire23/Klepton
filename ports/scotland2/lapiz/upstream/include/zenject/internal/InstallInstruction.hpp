#pragma once

#include "System/Type.hpp"
#include "zenject/Zenjector.hpp"

namespace Lapiz::Zenject::Internal {

    class InstallInstruction
    {
        public:
            InstallInstruction(Il2CppClass* baseInstaller, ZenjectorCallback installCallback) : _baseInstaller(baseInstaller), _installCallback(installCallback) {};

            __declspec(property(get=get_baseInstaller)) Il2CppClass* BaseInstaller;
            __declspec(property(get=get_installCallback)) ZenjectorCallback InstallCallback;

            auto get_baseInstaller() const { return _baseInstaller; }
            auto get_installCallback() const { return _installCallback; }

            void onInstall(::Zenject::DiContainer* container) const { if (_installCallback) _installCallback(container); }
        private:
            const Il2CppClass* _baseInstaller;
            const ZenjectorCallback _installCallback;
    };
}
