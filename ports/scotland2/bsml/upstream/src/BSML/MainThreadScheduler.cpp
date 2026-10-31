#include "BSML/MainThreadScheduler.hpp"
#include <mutex>

#include "UnityEngine/Time.hpp"

DEFINE_TYPE(BSML, MainThreadScheduler);

namespace BSML {
    std::queue<std::function<void()>> MainThreadScheduler::scheduledMethods;
    std::mutex MainThreadScheduler::scheduledMethodsMutex;
    std::queue<std::function<void()>> MainThreadScheduler::nextFrameScheduledMethods;
    std::mutex MainThreadScheduler::nextFrameScheduledMethodsMutex;
    std::vector<std::pair<float, std::function<void()>>> MainThreadScheduler::scheduledAfterTimeMethods;
    std::mutex MainThreadScheduler::scheduledAfterTimeMethodsMutex;
    std::vector<std::tuple<bool, std::function<bool()>, std::function<void()>>> MainThreadScheduler::scheduledUntilMethods;
    std::mutex MainThreadScheduler::scheduledUntilMethodsMutex;

    bool MainThreadScheduler::CurrentThreadIsMainThread() {
        // unity icall for whether this is the main thread
        static auto currentThreadIsMainThread = il2cpp_utils::resolve_icall<bool>("UnityEngine.Object::CurrentThreadIsMainThread");
        return currentThreadIsMainThread();
    }

    void MainThreadScheduler::Schedule(std::function<void ()> method) {
        // if this thread is already main thread, just run the method
        if (CurrentThreadIsMainThread()) {
            method();
            return;
        }

        std::unique_lock lock(scheduledMethodsMutex);
        scheduledMethods.push(method);
    }

    void MainThreadScheduler::ScheduleNextFrame(std::function<void()> method) {
        std::unique_lock lock(nextFrameScheduledMethodsMutex);
        nextFrameScheduledMethods.push(method);
    }

    void MainThreadScheduler::ScheduleAfterTime(float time, std::function<void()> method) {
        std::unique_lock lock(scheduledAfterTimeMethodsMutex);
        scheduledAfterTimeMethods.emplace_back(time, method);
        std::stable_sort(scheduledAfterTimeMethods.begin(), scheduledAfterTimeMethods.end(), [](auto const& a, auto const& b){ return a.first < b.first; });
    }

    void MainThreadScheduler::ScheduleUntil(std::function<bool()> until, std::function<void()> method) {
        std::unique_lock lock(scheduledUntilMethodsMutex);
        scheduledUntilMethods.emplace_back(false, until, method);
    }

    void MainThreadScheduler::Update() {
        {
            // aqcuire lock and swap collection to allow unlocking the collection while it's in use
            std::unique_lock<std::mutex> lock(scheduledUntilMethodsMutex);
            decltype(scheduledUntilMethods) iterationVector;
            iterationVector.swap(scheduledUntilMethods);
            lock.unlock();

            // go through the collection and see if there are any methods we can invoke
            int invokedCount = 0;
            for (auto& [willBeInvoked, until, method] : iterationVector) {
                if (willBeInvoked) continue;
                if (!until()) continue;

                willBeInvoked = true;
                invokedCount++;

                // we got a method, push it onto the queue of methods that will already be invoked this frame
                std::unique_lock lock(scheduledMethodsMutex);
                scheduledMethods.emplace(method);
            }

            if (invokedCount > 0) {
                // if anything was invoked we need to remove those
                // lock collection while we are changing its contents
                lock.lock();
                scheduledUntilMethods.reserve(scheduledUntilMethods.size() + iterationVector.size() - invokedCount);
                for (auto& v : iterationVector) {
                    // if the first value of the tuple is true, that means it will be invoked and it should not be kept
                    if (std::get<0>(v)) continue;
                    scheduledUntilMethods.emplace_back(std::move(v));
                }
                lock.unlock();
            } else {
                lock.lock();
                // nothing was invoked, but things could've been added in the meantime so insert the "old" values into main vector
                scheduledUntilMethods.insert(scheduledUntilMethods.begin(), iterationVector.begin(), iterationVector.end());
                lock.unlock();
            }
        }

        {
            // get delta time to use in decreasing timers
            auto delta = UnityEngine::Time::get_deltaTime();

            // aqcuire lock for time scheduled methods
            std::unique_lock scheduledAfterTimeLock(scheduledAfterTimeMethodsMutex);
            // decrease all the timers
            for(auto& [time, func] : scheduledAfterTimeMethods) {
                time -= delta;
            }

            // if a timer hit below 0, we can add the method to the collection that will be invoked this frame
            while(!scheduledAfterTimeMethods.empty() && scheduledAfterTimeMethods.front().first <= 0.0f) {
                std::unique_lock lock(scheduledMethodsMutex);
                scheduledMethods.emplace(scheduledAfterTimeMethods.front().second);
                lock.unlock();
                scheduledAfterTimeMethods.erase(scheduledAfterTimeMethods.begin());
            }
        }

        {
            decltype(scheduledMethods) methodsToExecute;

            // acquire lock and swap with empty queue so we can release the lock during execution
            std::unique_lock scheduledMethodsLock(scheduledMethodsMutex);
            methodsToExecute.swap(scheduledMethods);
            scheduledMethodsLock.unlock();

            // run through our backlog
            while (!methodsToExecute.empty()) {
                methodsToExecute.front()();
                methodsToExecute.pop();
            }

            decltype(nextFrameScheduledMethods) methodsToExecuteNextFrame;

            // aqcuire lock and swap with empty queue so we can release the lock
            std::unique_lock<std::mutex> nextFrameMethodsLock(nextFrameScheduledMethodsMutex);
            methodsToExecuteNextFrame.swap(nextFrameScheduledMethods);
            nextFrameMethodsLock.unlock();

            // aqcuire lock for scheduled methods because we are editing that
            scheduledMethodsLock.lock();
            // push all the nextframe methods onto the scheduled methods queue so they will be executed next frame
            while(!methodsToExecuteNextFrame.empty()) {
                scheduledMethods.push(methodsToExecuteNextFrame.front());
                methodsToExecuteNextFrame.pop();
            }
            scheduledMethodsLock.unlock();
        }
    }
}
