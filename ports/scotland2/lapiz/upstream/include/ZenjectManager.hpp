#pragma once

#include "Zenject/Context.hpp"
#include "zenject/internal/ContextBinding.hpp"
#include "zenject/internal/Instructors/InstructorManager.hpp"
#include "zenject/internal/Mutators/MutatorManager.hpp"
#include "zenject/internal/Exposers/ExposerManager.hpp"
#include "zenject/Zenjector.hpp"

namespace Lapiz::Zenject {

    class ZenjectManager {
        public:
            ZenjectManager();

            static ZenjectManager& get_instance();
            auto get_initialSceneConstructionRegistered() { return _initialSceneConstructionRegistered; }
        private:
            friend class Zenjector;
            static constexpr const char* _initialContextName = "AppCoreSceneContext";
            
            void Add(Zenjector* zenjector);
            void ContextDecorator_ContextInstalling(::Zenject::Context* mainContext, Internal::ContextBindingSet installerBindings);
            
            static bool _initialSceneConstructionRegistered;
            std::set<Zenjector*> _zenjectors;

            Internal::Instructors::InstructorManager _instructorManager;
            Internal::Mutators::MutatorManager _mutatorManager;
            Internal::Exposers::ExposerManager _exposerManager;
    };
}