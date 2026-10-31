#include "types.hpp"

DEFINE_TYPE(MetaCore, ObjectSignal);
DEFINE_TYPE(MetaCore, EndDragHandler);
DEFINE_TYPE(MetaCore, KeyboardCloseHandler);
DEFINE_TYPE(MetaCore, MainThreadScheduler);

std::unordered_map<int, std::function<void()>> MetaCore::ObjectSignal::onDestroys;

void MetaCore::ObjectSignal::OnEnable() {
    if (onEnable)
        onEnable();
}

void MetaCore::ObjectSignal::OnDisable() {
    if (onDisable)
        onDisable();
}

void MetaCore::EndDragHandler::OnPointerUp(UnityEngine::EventSystems::PointerEventData* eventData) {
    if (callback)
        callback();
}

static std::mutex callbacksMutex;
static std::queue<std::function<void()>> callbacks;
static std::mutex waitersMutex;
static std::vector<std::pair<std::function<bool()>, std::function<void()>>> waiters;
static std::mutex updatesMutex;
static std::vector<std::function<void()>> updates;

void MetaCore::MainThreadScheduler::Update() {
    std::unique_lock waitersLock(waitersMutex);
    std::unique_lock callbacksLock(callbacksMutex);
    std::unique_lock updatesLock(updatesMutex);

    decltype(waiters) waitersCopy;
    waitersCopy.swap(waiters);

    for (auto& [until, callback] : waitersCopy) {
        if (until())
            callbacks.emplace(std::move(callback));
        else
            waiters.emplace_back(std::move(until), std::move(callback));
    }

    decltype(callbacks) callbacksCopy;
    callbacksCopy.swap(callbacks);

    decltype(updates) updatesCopy = {updates.begin(), updates.end()};

    waitersLock.unlock();
    callbacksLock.unlock();
    updatesLock.unlock();

    while (!callbacksCopy.empty()) {
        callbacksCopy.front()();
        callbacksCopy.pop();
    }

    for (auto& callback : updatesCopy)
        callback();
}

void MetaCore::MainThreadScheduler::Schedule(std::function<void()> callback) {
    std::unique_lock lock(callbacksMutex);
    callbacks.emplace(std::move(callback));
}

void MetaCore::MainThreadScheduler::Schedule(std::function<bool()> wait, std::function<void()> callback) {
    std::unique_lock lock(waitersMutex);
    waiters.emplace_back(std::move(wait), std::move(callback));
}

void MetaCore::MainThreadScheduler::AddUpdate(std::function<void()> callback) {
    std::unique_lock lock(updatesMutex);
    updates.emplace_back(std::move(callback));
}
