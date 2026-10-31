#include "ZenjectManager.hpp"
#include "utilities/logging.hpp"

#include "Hooks/ContextDecorator.hpp"

#include "zenject/internal/InstallInstruction.hpp"
#include "zenject/internal/InstallSet.hpp"
#include "Zenject/SceneDecoratorContext.hpp"

#include "scotland2/shared/modloader.h"

namespace Lapiz::Zenject {
    static ZenjectManager instance;
    bool ZenjectManager::_initialSceneConstructionRegistered = false;

    ZenjectManager& ZenjectManager::get_instance() {
        return instance;
    }

    void ZenjectManager::Add(Zenjector* zenjector) {
        DEBUG("Registered zenjector for mod {} v{}", zenjector->modInfo.id, zenjector->modInfo.version);
        _zenjectors.emplace(zenjector);
    }

    ZenjectManager::ZenjectManager() {
        Internal::ContextDecorator::contextInstalling += {&ZenjectManager::ContextDecorator_ContextInstalling, this};
    }

    void ZenjectManager::ContextDecorator_ContextInstalling(::Zenject::Context* mainContext, Internal::ContextBindingSet installerBindings) {
        if (mainContext->get_name() == _initialContextName)
            _initialSceneConstructionRegistered = true;

        if (!_initialSceneConstructionRegistered) return;

        std::set<UnityEngine::MonoBehaviour*> injectableList;
        bool isDecorator = il2cpp_utils::IsConvertibleFrom(&classof(::Zenject::SceneDecoratorContext*)->byval_arg, &mainContext->klass->byval_arg, false);

        for (auto zenjector : _zenjectors) {
            if (isDecorator) {
                for (auto set : zenjector->_mutateSets) {
                    _mutatorManager.Install(set, mainContext, injectableList);
                }
                for (auto set : zenjector->_exposeSets) {
                    _exposerManager.Install(set, mainContext, injectableList);
                }
            }

            auto& bindings = *installerBindings.get();
            for (auto set : zenjector->_installSets) {
                for (auto binding : bindings) {
                    if (set->Filter->ShouldInstall(binding.get())) {
                        auto instructor = _instructorManager.InstructorForSet(set);
                        if (!instructor) {
                            auto t = set->InstallerType;
                            WARNING("Could not find instatiation instructor for type {}::{}, skipping this set!", t->namespaze, t->name);
                            continue;
                        }
                        instructor->Install(set, binding.get());
                    }
                }
            }

            for (auto instruction : zenjector->_installInstructions) {
                for (auto binding : bindings) {
                    if (instruction->BaseInstaller == binding->InstallerType) {
                        auto container = binding->Context->Container;
                        instruction->onInstall(container);
                    }
                }
            }
        }
    }
}
