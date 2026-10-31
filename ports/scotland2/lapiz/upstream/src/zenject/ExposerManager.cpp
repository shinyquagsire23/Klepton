#include "zenject/internal/Exposers/ExposerManager.hpp"

#include "utilities/logging.hpp"
#include "utilities/typeutil.hpp"
#include "Zenject/SceneDecoratorContext.hpp"
#include "Zenject/DiContainer.hpp"
#include "Zenject/ConcreteIdBinderNonGeneric.hpp"

namespace Lapiz::Zenject::Internal::Exposers {
    void ExposerManager::Install(Internal::ExposeSet* exposeSet, ::Zenject::Context* ctx, std::set<UnityEngine::MonoBehaviour*> iterlist) {
        auto sceneDecoratorContext = il2cpp_utils::try_cast<::Zenject::SceneDecoratorContext>(ctx).value_or(nullptr);
        if (!sceneDecoratorContext || !sceneDecoratorContext->m_CachedPtr.m_value) {
            return;
        }

        if (exposeSet->get_locationContractName() != sceneDecoratorContext->_decoratedContractName ||
            System::String::IsNullOrEmpty(sceneDecoratorContext->_decoratedContractName) ||
            exposeSet->get_locationContractName().empty()) {
            return;
        }

        if (iterlist.size() == 0) {
            auto count = sceneDecoratorContext->_injectableMonoBehaviours->Count;
            for (auto i = 0; i < count; i++) {
                iterlist.insert(sceneDecoratorContext->_injectableMonoBehaviours->get_Item(i));
            }
        }

        UnityEngine::MonoBehaviour* toExpose = nullptr;
        for (auto il : iterlist) {
            if (il->klass == exposeSet->get_typeToExpose()) {
                toExpose = il;
                break;
            }
        }

        if (toExpose && toExpose->m_CachedPtr.m_value) {
            ArrayW<System::Type*> ts(il2cpp_array_size_t(1));
            ts[0] = reinterpret_cast<System::Type*>(il2cpp_utils::GetSystemType(exposeSet->get_typeToExpose()));
            sceneDecoratorContext->Container->Bind(ts)->FromInstance(toExpose)->AsSingle();
        }
        else {
            WARNING("Could not find {} in {}.", exposeSet->get_typeToExpose()->name, exposeSet->get_locationContractName());
        }
    };
}
