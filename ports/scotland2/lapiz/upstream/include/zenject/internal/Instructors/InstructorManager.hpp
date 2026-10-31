#pragma once

#include "IInstructor.hpp"

#include "MonoInstructor.hpp"
#include "ParameterizedInstructor.hpp"
#include "TypedInstructor.hpp"

#include "UnityEngine/Component.hpp"
#include "Zenject/MonoInstallerBase.hpp"
#include "utilities/typeutil.hpp"

namespace Lapiz::Zenject {
    class ZenjectManager;
    namespace Internal::Instructors {
        class InstructorManager {
            public:
                IInstructor* InstructorForSet(InstallSet* set) {
                    auto installerType = set->get_installerType();

                    if (!TypeUtil::hasAncestor<UnityEngine::Component*>(installerType) && set->get_parameters()) return _parameterizedInstructor;
                    if (TypeUtil::hasAncestor<::Zenject::MonoInstallerBase*>(installerType)) return _monoInstructor;
                    if (!TypeUtil::hasAncestor<UnityEngine::Component*>(installerType)) return _typedInstructor;

                    return nullptr;
                }
            private:
                friend class ::Lapiz::Zenject::ZenjectManager;
                InstructorManager() {
                    _monoInstructor = new MonoInstructor();
                    _parameterizedInstructor = new ParameterizedInstructor();
                    _typedInstructor = new TypedInstructor();
                }

                IInstructor* _monoInstructor;
                IInstructor* _typedInstructor;
                IInstructor* _parameterizedInstructor;
        };
    }
}