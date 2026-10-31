#pragma once

#include "IInstructor.hpp"

namespace Lapiz::Zenject::Internal::Instructors {
    class InstructorManager;
    class TypedInstructor : public IInstructor {
        public:
            void Install(InstallSet* set, ContextBinding* binding) override {
                binding->AddInstaller(set->get_installerType());
            }
        private:
            TypedInstructor() = default;
            friend class InstructorManager;
    };
}