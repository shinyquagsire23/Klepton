#pragma once

#include "IInstructor.hpp"
#include "UnityEngine/GameObject.hpp"
#include "Zenject/MonoInstaller.hpp"

namespace Lapiz::Zenject::Internal::Instructors {
    class InstructorManager;
    class MonoInstructor : public IInstructor {
        public:
            void Install(InstallSet* set, ContextBinding* binding) override {
                auto type = reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(set->InstallerType));
                auto installer = binding->get_context()->get_gameObject()->AddComponent(type).cast<::Zenject::MonoInstaller>();
                if (installer)
                    binding->AddInstaller(installer);
            }
        private:
            MonoInstructor() = default;
            friend class InstructorManager;
    };
}
