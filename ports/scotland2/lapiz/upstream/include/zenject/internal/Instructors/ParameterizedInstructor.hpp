#pragma once

#include "IInstructor.hpp"
#include "Zenject/DiContainer.hpp"

namespace Lapiz::Zenject::Internal::Instructors {
    class InstructorManager;
    class ParameterizedInstructor : public IInstructor {
        public:
            void Install(InstallSet* set, ContextBinding* binding) override {
                auto container = binding->get_context()->get_Container();
                auto args = reinterpret_cast<::System::Collections::Generic::IEnumerable_1<::System::Object*>*>(set->get_parameters().convert());
                auto type = reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(set->get_installerType()));
                auto installerBase = reinterpret_cast<::Zenject::InstallerBase*>(container->Instantiate(type, args));
                if (installerBase)
                    binding->AddInstaller(installerBase);
            }
        private:
            ParameterizedInstructor() = default;
            friend class InstructorManager;
    };
}
