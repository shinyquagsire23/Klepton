#pragma once

#include <queue>

#include "UnityEngine/EventSystems/IEventSystemHandler.hpp"
#include "UnityEngine/EventSystems/IPointerUpHandler.hpp"
#include "UnityEngine/MonoBehaviour.hpp"
#include "custom-types/shared/macros.hpp"

#define UES UnityEngine::EventSystems

DECLARE_CLASS_CODEGEN(MetaCore, ObjectSignal, UnityEngine::MonoBehaviour) {
    DECLARE_DEFAULT_CTOR();

    DECLARE_INSTANCE_METHOD(void, OnEnable);
    DECLARE_INSTANCE_METHOD(void, OnDisable);

   public:
    std::function<void()> onEnable = nullptr;
    std::function<void()> onDisable = nullptr;

    static std::unordered_map<int, std::function<void()>> onDestroys;
};

DECLARE_CLASS_CODEGEN_INTERFACES(MetaCore, EndDragHandler, UnityEngine::MonoBehaviour, UES::IEventSystemHandler*, UES::IPointerUpHandler*) {
    DECLARE_DEFAULT_CTOR();

    DECLARE_OVERRIDE_METHOD_MATCH(void, OnPointerUp, &UES::IPointerUpHandler::OnPointerUp, UES::PointerEventData* eventData);

   public:
    std::function<void()> callback = nullptr;
};

DECLARE_CLASS_CODEGEN(MetaCore, KeyboardCloseHandler, UnityEngine::MonoBehaviour) {
    DECLARE_DEFAULT_CTOR();

   public:
    std::function<void()> closeCallback = nullptr;
    std::function<void()> okCallback = nullptr;
};

DECLARE_CLASS_CODEGEN(MetaCore, MainThreadScheduler, UnityEngine::MonoBehaviour) {
    DECLARE_DEFAULT_CTOR();

    DECLARE_INSTANCE_METHOD(void, Update);

   public:
    static void Schedule(std::function<void()> callback);
    static void Schedule(std::function<bool()> wait, std::function<void()> callback);
    static void AddUpdate(std::function<void()> callback);

    template <class T>
    static void Await(T task, std::function<void()> callback) {
        Schedule([task]() { return task->IsCompleted; }, std::move(callback));
    }
};

#undef UES
