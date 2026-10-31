#include "utilities/MainThreadScheduler.hpp"

DEFINE_TYPE(Lapiz::Utilities, MainThreadScheduler);

namespace Lapiz::Utilities {
    std::queue<std::function<void()>> MainThreadScheduler::scheduled;
    std::mutex MainThreadScheduler::scheduleLock;
    
    void MainThreadScheduler::Schedule(std::function<void()> function) {
        using CurrentThreadIsMainThreadMethod = function_ptr_t<bool>;
        static CurrentThreadIsMainThreadMethod currentThreadIsMainThread = il2cpp_utils::resolve_icall<bool>("UnityEngine.Object::CurrentThreadIsMainThread");
        if(currentThreadIsMainThread()) {
            function();
            return;
        }

        std::lock_guard<std::mutex> lock(scheduleLock);
        scheduled.push(function);
    }

    void MainThreadScheduler::Tick() {
        if (!scheduled.empty()) {
            std::lock_guard<std::mutex> lock(scheduleLock);
            scheduled.front()();
            scheduled.pop();
        }
    }
}