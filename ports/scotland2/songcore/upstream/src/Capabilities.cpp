#include "Capabilities.hpp"
#include "logging.hpp"

DEFINE_TYPE(SongCore, Capabilities);

namespace SongCore {
    void Capabilities::Initialize() {
        SongCore::API::Capabilities::GetCapabilitiesUpdatedEvent() += {&Capabilities::CapabilitiesUpdated, this};
    }

    void Capabilities::Dispose() {
        SongCore::API::Capabilities::GetCapabilitiesUpdatedEvent() -= {&Capabilities::CapabilitiesUpdated, this};
    }

    void Capabilities::RegisterCapability(std::string_view capability) {
        return SongCore::API::Capabilities::RegisterCapability(capability);
    }

    void Capabilities::UnregisterCapability(std::string_view capability) {
        return SongCore::API::Capabilities::UnregisterCapability(capability);
    }

    bool Capabilities::IsCapabilityRegistered(std::string_view capability) const {
        return SongCore::API::Capabilities::IsCapabilityRegistered(capability);
    }

    std::span<const std::string> Capabilities::GetRegisteredCapabilities() const {
        return SongCore::API::Capabilities::GetRegisteredCapabilities();
    }

    UnorderedEventCallback<std::string_view, SongCore::API::Capabilities::CapabilityEventKind>& Capabilities::GetCapabilitiesUpdatedEvent() {
        return _capabilitiesUpdated;
    }
    void Capabilities::CapabilitiesUpdated(std::string_view capability, SongCore::API::Capabilities::CapabilityEventKind eventKind) {
        _capabilitiesUpdated.invoke(capability, eventKind);
    }

}
