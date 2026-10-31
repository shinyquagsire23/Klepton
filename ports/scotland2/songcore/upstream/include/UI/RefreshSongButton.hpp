#pragma once

#include "custom-types/shared/macros.hpp"
#include "SongLoader/RuntimeSongLoader.hpp"
#include "Zenject/IInitializable.hpp"
#include "System/IDisposable.hpp"
#include "bsml/shared/BSML/MenuButtons/MenuButton.hpp"

DECLARE_CLASS_CODEGEN_INTERFACES(SongCore::UI, RefreshSongButton, System::Object, Zenject::IInitializable*, System::IDisposable*) {
    DECLARE_CTOR(ctor, SongLoader::RuntimeSongLoader* runtimeSongLoader);
    DECLARE_INSTANCE_FIELD_PRIVATE(SongLoader::RuntimeSongLoader*, _runtimeSongLoader);
    DECLARE_INSTANCE_FIELD_PRIVATE(BSML::MenuButton*, _menuButton);

    DECLARE_OVERRIDE_METHOD_MATCH(void, Initialize, &Zenject::IInitializable::Initialize);
    DECLARE_OVERRIDE_METHOD_MATCH(void, Dispose, &System::IDisposable::Dispose);
};