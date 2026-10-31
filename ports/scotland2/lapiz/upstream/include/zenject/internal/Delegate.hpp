#pragma once

#include "System/Action.hpp"
#include "System/Object.hpp"

#include "Zenject/SceneDecoratorContext.hpp"

namespace Lapiz::Zenject::Internal {
    class ZenjectDelegate {
        public:
            template<typename T, typename U>
            requires(std::is_convertible_v<T, ::Zenject::SceneDecoratorContext*> && std::is_convertible_v<U, System::Object*>)
            ZenjectDelegate* Wrap(std::function<void(T, U)> callback) {
                // if the callback is already of the right type, skip the callback wrapping for casting
                if constexpr (std::is_same_v<T, ::Zenject::SceneDecoratorContext*> && std::is_same_v<U, System::Object*>) {
                    actionObj = callback;
                } else {
                    actionObj = [callback](::Zenject::SceneDecoratorContext* ctx, System::Object* obj) {
                        callback(reinterpret_cast<T>(ctx), reinterpret_cast<U>(obj));
                    };
                }
                return this;
            }

            void Invoke(::Zenject::SceneDecoratorContext* ctx, System::Object* obj) const {
                if (actionObj) {
                    actionObj(ctx, obj);
                }
            }
        private:
            std::function<void(::Zenject::SceneDecoratorContext*, System::Object* obj)> actionObj;
    };
}