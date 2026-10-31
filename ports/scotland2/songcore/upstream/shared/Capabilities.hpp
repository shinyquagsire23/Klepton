#pragma once

#include "custom-types/shared/macros.hpp"
#include "./SongCore.hpp"
#include "System/Object.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"

#include <vector>
#include <string>
#include <ranges>

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore, Capabilities, System::Object, Zenject::IInitializable*, System::IDisposable*) {
        DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
        DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);
    public:
        /// @brief Registers a capability to let songcore know what the game is capable of
        /// @param capability The capability to register
        void RegisterCapability(std::string_view capability);

        /// @brief Unregisters a capability to let songcore know what the game is capable of
        /// @param capability The capability to register
        void UnregisterCapability(std::string_view capability);

        /// @brief checks whether a capability is registered
        /// @param capability The checked capability
        /// @return true for registered, false for not
        bool IsCapabilityRegistered(std::string_view capability) const;

        /// @brief provides access to the registered capabilities without allowing edits
        std::span<const std::string> GetRegisteredCapabilities() const;
        __declspec(property(get=GetRegisteredCapabilities)) std::span<const std::string> RegisteredCapabilities;

        /// @brief provides access to an event that gets invoked when the capabilities are updated. not guaranteed to run on main thread! cleared on soft restart.
        UnorderedEventCallback<std::string_view, SongCore::API::Capabilities::CapabilityEventKind>& GetCapabilitiesUpdatedEvent();

        /// @brief an event that gets invoked when the capabilities are updated. not guaranteed to run on main thread! cleared on soft restart. Invoked after the particular capability is added to the list.
        __declspec(property(get=GetCapabilitiesUpdatedEvent)) UnorderedEventCallback<std::string_view, SongCore::API::Capabilities::CapabilityEventKind>& CapabilitiesUpdatedEvent;

        DECLARE_DEFAULT_CTOR();
    private:
        /// @brief method to register to the general API one
        void CapabilitiesUpdated(std::string_view capability, SongCore::API::Capabilities::CapabilityEventKind eventKind);
        UnorderedEventCallback<std::string_view, SongCore::API::Capabilities::CapabilityEventKind> _capabilitiesUpdated;
};