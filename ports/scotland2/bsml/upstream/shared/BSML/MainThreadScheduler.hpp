#pragma once

#include "custom-types/shared/macros.hpp"

#include "UnityEngine/MonoBehaviour.hpp"
#include <queue>
#include <tuple>
#include <type_traits>

DECLARE_CLASS_CODEGEN(BSML, MainThreadScheduler, UnityEngine::MonoBehaviour) {
    private:
        static std::queue<std::function<void()>> scheduledMethods;
        static std::mutex scheduledMethodsMutex;
        static std::queue<std::function<void()>> nextFrameScheduledMethods;
        static std::mutex nextFrameScheduledMethodsMutex;
        static std::vector<std::pair<float, std::function<void()>>> scheduledAfterTimeMethods;
        static std::mutex scheduledAfterTimeMethodsMutex;
        static std::vector<std::tuple<bool, std::function<bool()>, std::function<void()>>> scheduledUntilMethods;
        static std::mutex scheduledUntilMethodsMutex;

    public:
        /// @brief schedule a method to be called on the main thread, or if you are on the main thread right now immediately
        static void Schedule(std::function<void()> method);

        template<typename T, typename U>
        static void Schedule(T instance, void (U::*method)()) {
            Schedule(std::bind(method, instance));
        }

        /// @brief schedule a method to be called the next frame
        static void ScheduleNextFrame(std::function<void()> method);

        template<typename T, typename U>
        static void ScheduleNextFrame(T instance, void (U::*method)()) {
            ScheduleNextFrame(std::bind(method, instance));
        }

        /// @brief schedule a method to be called after time elapses
        static void ScheduleAfterTime(float time, std::function<void()> method);

        template<typename T, typename U>
        static void ScheduleAfterTime(float time, T instance, void (U::*method)()) {
            ScheduleAfterTime(time, std::bind(method, instance));
        }

        /// @brief schedule a method that waits until the until method returns true
        static void ScheduleUntil(std::function<bool()> until, std::function<void()> method);

        /// @brief method to use to await a shared future to complete execution, and execute method on finished
        /// @param future the future to await
        /// @param method the method to execute
        template<typename T>
        static void AwaitFuture(std::shared_future<T> future, std::function<void()> method) {
            static auto AwaitFuture = [](std::shared_future<T> future) {
                return !future.valid() || future.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
            };

            ScheduleUntil(std::bind(AwaitFuture, future), method);
        }

        /// @brief method to use to await a shared future to complete execution, and binding an instance method to execute when finished
        /// @param future the future to await
        /// @param method the instance method to execute
        /// @param instance the instance to use for the instance method
        template<typename T, typename U, typename V>
        requires(std::is_invocable_v<void(U::*)(), V*>)
        static void AwaitFuture(std::shared_future<T> future, void(U::*method)(), V* instance) {
            static auto AwaitFuture = [](std::shared_future<T> future) {
                return !future.valid() || future.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
            };

            static auto InvokeMethod = [](void(U::*method)(), V* instance){
                std::invoke(method, instance);
            };

            ScheduleUntil(std::bind(AwaitFuture, future), std::bind(InvokeMethod, method, instance));
        }

        /// @brief method to use to await a shared future to complete execution, the future result is passed into the method
        /// @param future the future to await
        /// @param method the method to execute when the future completes
        template<typename T>
        requires(!std::is_same_v<T, void> && std::is_copy_constructible_v<T>)
        static void AwaitFuture(std::shared_future<T> future, std::function<void(T)> method) {
            static auto AwaitFuture = [](std::shared_future<T> future) {
                return !future.valid() || future.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
            };

            static auto InvokeMethod = [](std::shared_future<T> future, std::function<void(T)> method){
                std::invoke(method, future.get());
            };

            ScheduleUntil(std::bind(AwaitFuture, future), std::bind(InvokeMethod, future, method));
        }

        /// @brief method to use to await a shared future to complete execution, and binding an instance method to execute when finished, the future result is passed into the method
        /// @param future the future to await
        /// @param method the instance method to execute
        /// @param instance the instance to use for the instance method
        template<typename T, typename U, typename V>
        requires(std::is_invocable_v<void(U::*)(T), V*, T> && !std::is_same_v<T, void> && std::is_copy_constructible_v<T>)
        static void AwaitFuture(std::shared_future<T> future, void(U::* method)(T), V* instance) {
            static auto AwaitFuture = [](std::shared_future<T> future) {
                return !future.valid() || future.wait_for(std::chrono::nanoseconds(0)) == std::future_status::ready;
            };

            static auto InvokeMethod = [](std::shared_future<T> future, void(U::* method)(T), V* instance){
                std::invoke(method, instance, future.get());
            };

            ScheduleUntil(std::bind(AwaitFuture, future), std::bind(InvokeMethod, future, method, instance));
        }

        /// @brief method that checks whether the thread it's called from is the main thread
        static bool CurrentThreadIsMainThread();

    DECLARE_INSTANCE_METHOD(void, Update);
};