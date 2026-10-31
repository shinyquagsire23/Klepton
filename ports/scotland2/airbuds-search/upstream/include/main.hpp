#pragma once

#if !defined(SC2_DIAGNOSTICS) || SC2_DIAGNOSTICS
#define CT_DEBUG
#endif

// Include the modloader header, which allows us to tell the modloader which mod
// this is, and the version etc.
#include "scotland2/shared/modloader.h"

// beatsaber-hook is a modding framework that lets us call functions and fetch
// field values from in the game It also allows creating objects, configuration,
// and importantly, hooking methods to modify their values
#include "beatsaber-hook/shared/config/config-utils.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"

#include "_config.hpp"

#include "Airbuds/AirbudsClient.hpp"
#include "UI/FlowCoordinators/AirbudsSearchFlowCoordinator.hpp"

namespace AirbudsSearch {

inline bool returnToAirbudsSearch = false;

inline std::shared_ptr<airbuds::Client> airbudsClient;

inline SafePtrUnity<AirbudsSearch::UI::FlowCoordinators::AirbudsSearchFlowCoordinator> airbudsSearchFlowCoordinator_;

void openAirbudsSearchFlowCoordinator();

}
