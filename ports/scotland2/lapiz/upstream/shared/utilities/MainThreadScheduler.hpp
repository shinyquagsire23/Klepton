#pragma once

#include "custom-types/shared/macros.hpp"
#include "Zenject/ITickable.hpp"

#include <functional>
#include <queue>

DECLARE_CLASS_CODEGEN_INTERFACES(Lapiz::Utilities, MainThreadScheduler, System::Object, ::Zenject::ITickable*) {
    DECLARE_DEFAULT_CTOR();
    DECLARE_OVERRIDE_METHOD_MATCH(void, Tick, &::Zenject::ITickable::Tick);

    public:
        static void Schedule(std::function<void()> function);

        template<typename T, typename U>
        static void Schedule(T instance, void (U::*method)()) {
            Schedule(std::bind(method, instance));
        }

    private:
        static std::queue<std::function<void()>> scheduled;
        static std::mutex scheduleLock;
};