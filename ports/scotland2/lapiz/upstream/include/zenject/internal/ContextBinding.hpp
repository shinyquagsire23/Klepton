#pragma once

#include "Zenject/Context.hpp"
#include "ZenjectInstallationAccessor.hpp"

namespace Lapiz::Zenject::Internal {
    struct ContextBinding {
            ContextBinding(::Zenject::Context* context, Il2CppClass* installerType, std::shared_ptr<ZenjectInstallationAccessor> accessor)
            : _context(context), _installerType(installerType), _accessor(accessor) {}

            __declspec(property(get=get_installerType)) Il2CppClass* InstallerType;
            __declspec(property(get=get_context)) ::Zenject::Context* Context;
            __declspec(property(get=get_accessor)) std::shared_ptr<ZenjectInstallationAccessor> Accessor;

            void AddInstaller(::Zenject::InstallerBase* installerBase) {
                _accessor->get_normalInstallers()->Add(installerBase);
            }
            void AddInstaller(::Zenject::MonoInstaller* monoInstaller) {
                _accessor->get_installers()->Add(monoInstaller);
            }
            void AddInstaller(const Il2CppClass* klass) {
                AddInstaller(reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(klass)));
            }
            void AddInstaller(System::Type* type) {
                // TODO: Assert derives from installer base?
                _accessor->get_normalInstallerTypes()->Add(type);
            }

            auto get_installerType() const { return _installerType; }
            auto get_context() const { return _context; }
            auto get_accesor() const { return _accessor; }

        private:
            ::Zenject::Context* _context;
            Il2CppClass* _installerType;
            std::shared_ptr<ZenjectInstallationAccessor> _accessor;
    };
    using ContextBindingSet = std::shared_ptr<std::set<std::shared_ptr<ContextBinding>>>;
}
