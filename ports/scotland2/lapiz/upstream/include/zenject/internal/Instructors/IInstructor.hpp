#pragma once

#include "../InstallSet.hpp"
#include "../ContextBinding.hpp"

namespace Lapiz::Zenject::Internal::Instructors {
    class IInstructor {
        public:
            virtual void Install(InstallSet* set, ContextBinding* binding) = 0;
    };
}