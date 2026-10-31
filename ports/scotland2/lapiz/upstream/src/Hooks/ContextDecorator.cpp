#include "utilities/hooking.hpp"
#include "utilities/logging.hpp"

#include "Hooks/ContextDecorator.hpp"

#include "zenject/internal/ContextBinding.hpp"
#include "Zenject/SceneDecoratorContext.hpp"
#include "Zenject/Context.hpp"


namespace Lapiz::Zenject::Internal {
    std::set<::Zenject::Context*> ContextDecorator::recentlyInstalledDecorators;
    UnorderedEventCallback<::Zenject::Context*, ContextBindingSet> ContextDecorator::contextInstalling;

    void ContextDecorator::Context_InstallInstallers_Prefix(::Zenject::Context* self, ListW<::Zenject::InstallerBase*> normalInstallers, ListW<::System::Type*> normalInstallerTypes, ListW<::Zenject::ScriptableObjectInstaller*> scriptableObjectInstallers, ListW<::Zenject::MonoInstaller*> installers, ListW<::Zenject::MonoInstaller*> installerPrefabs) {
        if (recentlyInstalledDecorators.find(self) != recentlyInstalledDecorators.end()) {
            recentlyInstalledDecorators.erase(self);
            return;
        }

        auto accessor = std::make_shared<ZenjectInstallationAccessor>(normalInstallers, normalInstallerTypes, installers);
        ContextBindingSet bindings = std::make_shared<std::set<std::shared_ptr<ContextBinding>>>();
        for (auto normalInstaller : normalInstallers)
            bindings->emplace(std::make_shared<ContextBinding>(self, normalInstaller->klass, accessor));

        for (auto normalInstallerType : normalInstallerTypes) {
            bindings->emplace(std::make_shared<ContextBinding>(self, il2cpp_functions::class_from_system_type((Il2CppReflectionType*)normalInstallerType), accessor));
        }

        for (auto scriptableObjectInstaller : scriptableObjectInstallers)
            bindings->emplace(std::make_shared<ContextBinding>(self, scriptableObjectInstaller->klass, accessor));

        for (auto installer : installers)
            bindings->emplace(std::make_shared<ContextBinding>(self, installer->klass, accessor));

        for (auto installerPrefab : installerPrefabs)
            bindings->emplace(std::make_shared<ContextBinding>(self, installerPrefab->klass, accessor));

        if (il2cpp_utils::IsConvertibleFrom(&classof(::Zenject::SceneDecoratorContext*)->byval_arg, &self->klass->byval_arg, false))
            recentlyInstalledDecorators.emplace(self);

        if (contextInstalling.size() > 0)
            contextInstalling.invoke(self, bindings);
    }
}

using namespace Lapiz::Zenject::Internal;
using namespace System::Collections::Generic;

MAKE_AUTO_HOOK_MATCH(Context_InstallInstallers, static_cast<void (Zenject::Context::*)(List_1<::Zenject::InstallerBase*>*, List_1<::System::Type*>*, List_1<UnityW<::Zenject::ScriptableObjectInstaller>>*, List_1<UnityW<::Zenject::MonoInstaller>>*, List_1<UnityW<::Zenject::MonoInstaller>>*)>(&Zenject::Context::InstallInstallers),
    void,
    Zenject::Context* self,
    List_1<::Zenject::InstallerBase*>* normalInstallers,
    List_1<::System::Type*>* normalInstallerTypes,
    List_1<UnityW<::Zenject::ScriptableObjectInstaller>>* scriptableObjectInstallers,
    List_1<UnityW<::Zenject::MonoInstaller>>* installers,
    List_1<UnityW<::Zenject::MonoInstaller>>* installerPrefabs) {

    ContextDecorator::Context_InstallInstallers_Prefix(self, normalInstallers, normalInstallerTypes, scriptableObjectInstallers, installers, installerPrefabs);
    Context_InstallInstallers(self, normalInstallers, normalInstallerTypes, scriptableObjectInstallers, installers, installerPrefabs);
}
