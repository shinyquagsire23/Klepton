#pragma once

#include "custom-types/shared/macros.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "System/Object.hpp"
#include "SongCore.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore, PlayButtonInteractable, System::Object, Zenject::IInitializable*, System::IDisposable*) {
    DECLARE_CTOR(ctor);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);
    public:

#ifdef MOD_ID
        /// @brief disallows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        /// @param reason optional reason for why it was disabled
        void DisablePlayButton(std::string modID = MOD_ID, std::string reason = "");

        /// @brief allows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        void EnablePlayButton(std::string modID = MOD_ID);
#else
        /// @brief disallows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        /// @param reason optional reason for why it was disabled
        void DisablePlayButton(std::string modID, std::string reason = "");

        /// @brief allows the playbutton to be enabled by your mod
        /// @param modInfo the modinfo for the mod disabling the play button
        void EnablePlayButton(std::string modID);
#endif
        using PlayButtonDisablingModInfo = SongCore::API::PlayButton::PlayButtonDisablingModInfo;

        /// @brief event ran when the disabling mod infos change, like when Disable or Enable Play button is called from any mod, provides a span of the disabling mod ids and reasons
        UnorderedEventCallback<std::span<PlayButtonDisablingModInfo const>> PlayButtonDisablingModsChanged;

        /// @brief provides a span of the disabling mod infos
        std::span<PlayButtonDisablingModInfo const> GetPlayButtonDisablingModInfos();
        __declspec(property(get=GetPlayButtonDisablingModInfos)) std::span<PlayButtonDisablingModInfo const> PlayButtonDisablingModInfos;
    private:
        /// @brief invokes the PlayButtonDisablingModsChanged event
        void InvokePlayButtonDisablingModsChanged(std::span<PlayButtonDisablingModInfo const> disablingModInfos);
};