#pragma once

#include "RegistrationHelper.hpp"
#include "GlobalNamespace/BeatmapObjectsInstaller.hpp"

#include "GlobalNamespace/GameNoteController.hpp"
#include "GlobalNamespace/BombNoteController.hpp"
#include "GlobalNamespace/BurstSliderGameNoteController.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerGameNoteController.hpp"
#include "GlobalNamespace/SliderController.hpp"

namespace Lapiz::Objects::Beatmap {
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(BasicNoteRegistration, "_normalBasicNotePrefab", GlobalNamespace::GameNoteController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(BombNoteRegistration, "_bombNotePrefab", GlobalNamespace::BombNoteController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(BurstSliderHeadNoteRegistration, "_burstSliderHeadNotePrefab", GlobalNamespace::GameNoteController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(BurstSliderNoteRegistration, "_burstSliderNotePrefab", GlobalNamespace::BurstSliderGameNoteController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(ConnectedPlayerNoteRegistration, "_multiplayerGameNoteControllerPrefab", GlobalNamespace::MultiplayerConnectedPlayerGameNoteController*, GlobalNamespace::MultiplayerConnectedPlayerGameNoteController*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(ProModeNoteRegistration, "_proModeNotePrefab", GlobalNamespace::GameNoteController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(LongSliderNoteRegistration, "_sliderLongPrefab", GlobalNamespace::SliderController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(MediumSliderNoteRegistration, "_sliderMediumPrefab", GlobalNamespace::SliderController*, GlobalNamespace::BeatmapObjectsInstaller*);
    LAPIZ_REDECORATION_REGISTRATION_HELPER_DEFINITION(ShortSliderNoteRegistration, "_sliderShortPrefab", GlobalNamespace::SliderController*, GlobalNamespace::BeatmapObjectsInstaller*);
}
