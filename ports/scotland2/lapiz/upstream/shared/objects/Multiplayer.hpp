#pragma once

#include "RegistrationHelper.hpp"

#include "GlobalNamespace/MultiplayerConnectedPlayerFacade.hpp"
#include "GlobalNamespace/MultiplayerPlayersManager.hpp"
#include "GlobalNamespace/MultiplayerLobbyAvatarPlace.hpp"
#include "GlobalNamespace/MultiplayerLobbyInstaller.hpp"
#include "GlobalNamespace/MultiplayerLobbyAvatarController.hpp"
#include "GlobalNamespace/MultiplayerLocalActivePlayerFacade.hpp"
#include "GlobalNamespace/MultiplayerLocalInactivePlayerFacade.hpp"

namespace Lapiz::Objects::Multiplayer {
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(ConnectedPlayerDuelRegistration, "_connectedPlayerDuelControllerPrefab", GlobalNamespace::MultiplayerConnectedPlayerFacade*, GlobalNamespace::MultiplayerPlayersManager*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(ConnectedPlayerRegistration, "_connectedPlayerControllerPrefab", GlobalNamespace::MultiplayerConnectedPlayerFacade*, GlobalNamespace::MultiplayerPlayersManager*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LobbyAvatarPlaceRegistration, "_multiplayerAvatarPlacePrefab", GlobalNamespace::MultiplayerLobbyAvatarPlace*, GlobalNamespace::MultiplayerLobbyInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LobbyAvatarRegistration, "_multiplayerLobbyAvatarControllerPrefab", GlobalNamespace::MultiplayerLobbyAvatarController*, GlobalNamespace::MultiplayerLobbyInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LocalActivePlayerDuelRegistration, "_activeLocalPlayerDuelControllerPrefab", GlobalNamespace::MultiplayerLocalActivePlayerFacade*, GlobalNamespace::MultiplayerPlayersManager*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LocalActivePlayerRegistration, "_activeLocalPlayerControllerPrefab", GlobalNamespace::MultiplayerLocalActivePlayerFacade*, GlobalNamespace::MultiplayerPlayersManager*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LocalInactivePlayerRegistration, "_inactiveLocalPlayerControllerPrefab", GlobalNamespace::MultiplayerLocalInactivePlayerFacade*, GlobalNamespace::MultiplayerPlayersManager*);
}
