#pragma once

#include "IInstallFilter.hpp"
#include "UnityEngine/GameObject.hpp"

namespace Lapiz::Zenject::Internal::Filters {
    class ContextNamedSceneInstallFilter : public IInstallFilter {
        public:
            template<typename T>
            requires(std::is_convertible_v<T, ::Zenject::Context*>)
            ContextNamedSceneInstallFilter(std::string sceneName) : ContextNamedSceneInstallFilter(sceneName, classof(T)) {}
            ContextNamedSceneInstallFilter(std::string sceneName, Il2CppClass* contextType) : _sceneName(sceneName), _contextType(contextType) {}

            bool ShouldInstall(ContextBinding* binding) const override {
                return binding->get_context()->klass == _contextType && binding->get_context()->get_gameObject()->get_scene()->get_name() == _sceneName;
            }
        private:
            const std::string _sceneName;
            const Il2CppClass* _contextType;
    };
}