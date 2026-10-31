#include "utilities/hooking.hpp"
#include "utilities/logging.hpp"
#include "arrayutils.hpp"
/* I hate it here */

#include "GlobalNamespace/BoolSO.hpp"
#include "GlobalNamespace/FakeMirrorObjectsInstaller.hpp"
#include "GlobalNamespace/NoteDebrisPoolInstaller.hpp"
#include "GlobalNamespace/NoteDebris.hpp"
#include "GlobalNamespace/BeatmapObjectsInstaller.hpp"
#include "GlobalNamespace/EffectPoolsManualInstaller.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerInstaller.hpp"
#include "GlobalNamespace/MultiplayerLobbyInstaller.hpp"
#include "GlobalNamespace/MultiplayerPlayersManager.hpp"
#include "GlobalNamespace/MultiplayerPlayerLayout.hpp"
#include "GlobalNamespace/GameplayCoreSceneSetupData.hpp"
#include "GlobalNamespace/GameplayModifiers.hpp"
#include "GlobalNamespace/GameNoteController.hpp"
#include "GlobalNamespace/BurstSliderGameNoteController.hpp"
#include "GlobalNamespace/BombNoteController.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/SliderController.hpp"
#include "GlobalNamespace/BeatLine.hpp"

#include "GlobalNamespace/FlyingTextEffect.hpp"
#include "GlobalNamespace/FlyingScoreEffect.hpp"
#include "GlobalNamespace/FlyingSpriteEffect.hpp"
#include "GlobalNamespace/BeatEffect.hpp"
#include "GlobalNamespace/NoteCutSoundEffect.hpp"
#include "GlobalNamespace/BombCutSoundEffect.hpp"

#include "GlobalNamespace/MultiplayerConnectedPlayerGameNoteController.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerBombNoteController.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerObstacleController.hpp"

#include "GlobalNamespace/PlayersSpecificSettingsAtGameStartModel.hpp"
#include "GlobalNamespace/IConnectedPlayer.hpp"
#include "GlobalNamespace/PracticeSettings.hpp"
#include "GlobalNamespace/ColorScheme.hpp"
#include "GlobalNamespace/ColorManager.hpp"
#include "GlobalNamespace/MultiplayerPlayerStartState.hpp"
#include "GlobalNamespace/IConnectedPlayerBeatmapObjectEventManager.hpp"
#include "GlobalNamespace/PlayerSpecificSettingsNetSerializable.hpp"
#include "GlobalNamespace/ColorSchemeConverter.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerBeatmapObjectManager.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerBeatmapObjectEventManager.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/IBeatmapObjectSpawner.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerSongTimeSyncController.hpp"
#include "GlobalNamespace/BeatmapCharacteristicSO.hpp"
#include "GlobalNamespace/SaberManager.hpp"
#include "GlobalNamespace/SaberTypeExtensions.hpp"
#include "GlobalNamespace/SaberModelController.hpp"
#include "GlobalNamespace/PlayerSpecificSettings.hpp"
#include "GlobalNamespace/MultiplayerLobbyAvatarPlace.hpp"
#include "GlobalNamespace/MultiplayerLobbyAvatarController.hpp"

#include "GlobalNamespace/MultiplayerLocalActivePlayerFacade.hpp"
#include "GlobalNamespace/MultiplayerLocalInactivePlayerFacade.hpp"
#include "GlobalNamespace/MultiplayerLocalPlayerInstaller.hpp"
#include "GlobalNamespace/MultiplayerConnectedPlayerFacade.hpp"

#include "GlobalNamespace/FakeReflectionDynamicObjectsState.hpp"
#include "GlobalNamespace/MirroredBeatmapObjectManager.hpp"
#include "GlobalNamespace/MirroredBombNoteController.hpp"
#include "GlobalNamespace/MirroredObstacleController.hpp"
#include "GlobalNamespace/MirroredGameNoteController.hpp"
#include "GlobalNamespace/MirroredSliderController.hpp"
#include "GlobalNamespace/MirrorRendererGraphicsSettingsPresets.hpp"

#include "System/Collections/Generic/Dictionary_2.hpp"

#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Object.hpp"

#include "Zenject/DiContainer.hpp"
#include "Zenject/MemoryPoolIdInitialSizeMaxSizeBinder_1.hpp"
#include "Zenject/ConcreteIdBinderGeneric_1.hpp"
#include "Zenject/ConcreteIdBinderNonGeneric.hpp"
#include "Zenject/NameTransformScopeConcreteIdArgConditionCopyNonLazyBinder.hpp"
#include "Zenject/FactoryToChoiceIdBinder_2.hpp"
#include "Zenject/FactorySubContainerBinder_2.hpp"
#include "Zenject/FactoryToChoiceIdBinder_3.hpp"
#include "Zenject/FactorySubContainerBinder_3.hpp"
#include "Zenject/MemoryPoolIdInitialSizeMaxSizeBinder_1.hpp"

#include "sombrero/shared/linq_functional.hpp"

#include "objects/RedecoratorRegistration.hpp"
#include "objects/ObjectDiffuser.hpp"

using namespace GlobalNamespace;
using namespace UnityEngine;
using namespace Sombrero::Linq::Functional;
using namespace Lapiz::Objects;
using namespace Lapiz::ArrayUtils;

#define PREFAB_INITIALIZE(field_) (std::decay_t<decltype(self->field_)>)PrefabInitializing(self->field_, container, #field_, type)
static UnityEngine::Object* PrefabInitializing(UnityEngine::Object* originalPrefab, ::Zenject::DiContainer* container, std::string_view fieldName, System::Type* mainType) {
    // get all the redecorator registrations that are installed
    auto resolved = container->get_AncestorContainers()[0]->TryResolve<ListW<RedecoratorRegistration*>>();
    // if not found, just return the original prefab
    if (!resolved) {
        DEBUG("No redecorations found for contract {}", fieldName);
        return originalPrefab;
    }

    // filter for the ones that match
    auto registrations = resolved | Where(
        [&mainType, &fieldName](auto rr){
            return rr->ContainerType == mainType && rr->Contract == fieldName;
        }
    ) | ToArray();
    // if no matches, return the original prefab
    if (registrations.size() <= 0) {
        DEBUG("No redecorations found for contract {}", fieldName);
        return originalPrefab;
    }

    DEBUG("Redecorating contract {}", fieldName);
    // sort by priority, check if this is the right order! (asc / desc)
    std::stable_sort(
        registrations.begin(),
        registrations.end(),
        [](auto a, auto b) -> bool {
            return b->Priority < a->Priority;
        }
    );

    auto irgo = GameObject::New_ctor("Lapiz | InternalRedecorator");
    irgo->SetActive(false);
    Object::Instantiate(originalPrefab, irgo->transform);
    auto clone = irgo->GetComponentInChildren(registrations->First()->PrefabType, true);

    // apply all redecorations
    for (auto reg : registrations) {
        reg->Redecorate_internal(clone);
        if (!reg->Chain) break;
    }

    container->LazyInject(Lapiz::Objects::ObjectDiffuser::New_ctor(irgo));
    return clone;
}

MAKE_AUTO_HOOK_ORIG_MATCH(BeatmapObjectsInstaller_InstallBindings, &BeatmapObjectsInstaller::InstallBindings, void, BeatmapObjectsInstaller* self) {
	DEBUG("Redecoration BeatmapObjectsInstaller_InstallBindings");
    bool proMode = self->_sceneSetupData->gameplayModifiers->get_proMode();
    auto type = self->GetType();
    auto container = self->get_Container();

    // save orig
    auto orig_proModeNotePrefab = self->_proModeNotePrefab;
    auto orig_normalBasicNotePrefab = self->_normalBasicNotePrefab;
    auto orig_burstSliderHeadNotePrefab = self->_burstSliderHeadNotePrefab;
    auto orig_burstSliderNotePrefab = self->_burstSliderNotePrefab;
    auto orig_bombNotePrefab = self->_bombNotePrefab;
    auto orig_obstaclePrefab = self->_obstaclePrefab;
    auto orig_sliderShortPrefab = self->_sliderShortPrefab;
    auto orig_sliderMediumPrefab = self->_sliderMediumPrefab;
    auto orig_sliderLongPrefab = self->_sliderLongPrefab;
    auto orig_beatLinePrefab = self->_beatLinePrefab;

    // edit prefabs
    if (proMode) self->_proModeNotePrefab = PREFAB_INITIALIZE(_proModeNotePrefab);
    else self->_normalBasicNotePrefab = PREFAB_INITIALIZE(_normalBasicNotePrefab);
    self->_burstSliderHeadNotePrefab = PREFAB_INITIALIZE(_burstSliderHeadNotePrefab);
    self->_burstSliderNotePrefab = PREFAB_INITIALIZE(_burstSliderNotePrefab);
    self->_bombNotePrefab = PREFAB_INITIALIZE(_bombNotePrefab);
    self->_obstaclePrefab = PREFAB_INITIALIZE(_obstaclePrefab);
    self->_sliderShortPrefab = PREFAB_INITIALIZE(_sliderShortPrefab);
    self->_sliderMediumPrefab = PREFAB_INITIALIZE(_sliderMediumPrefab);
    self->_sliderLongPrefab = PREFAB_INITIALIZE(_sliderLongPrefab);
    self->_beatLinePrefab = PREFAB_INITIALIZE(_beatLinePrefab);

    // run method
    BeatmapObjectsInstaller_InstallBindings(self);

    // restore fields
    if (proMode) self->_proModeNotePrefab = orig_proModeNotePrefab;
    else self->_normalBasicNotePrefab = orig_normalBasicNotePrefab;
    self->_burstSliderHeadNotePrefab = orig_burstSliderHeadNotePrefab;
    self->_burstSliderNotePrefab = orig_burstSliderNotePrefab;
    self->_bombNotePrefab = orig_bombNotePrefab;
    self->_obstaclePrefab = orig_obstaclePrefab;
    self->_sliderShortPrefab = orig_sliderShortPrefab;
    self->_sliderMediumPrefab = orig_sliderMediumPrefab;
    self->_sliderLongPrefab = orig_sliderLongPrefab;
    self->_beatLinePrefab = orig_beatLinePrefab;
}

MAKE_AUTO_HOOK_ORIG_MATCH(EffectPoolsManualInstaller_ManualInstallBindings, &EffectPoolsManualInstaller::ManualInstallBindings, void, EffectPoolsManualInstaller* self, ::Zenject::DiContainer* container, bool shortBeatEffect) {
    DEBUG("Redecoration EffectPoolsManualInstaller_ManualInstallBindings");
    auto type = self->GetType();

    // save orig
    auto orig_flyingTextEffectPrefab = self->_flyingTextEffectPrefab;
    auto orig_flyingScoreEffectPrefab = self->_flyingScoreEffectPrefab;
    auto orig_flyingSpriteEffectPrefab = self->_flyingSpriteEffectPrefab;
    auto orig_shortBeatEffectPrefab = self->_shortBeatEffectPrefab;
    auto orig_beatEffectPrefab = self->_beatEffectPrefab;
    auto orig_noteCutSoundEffectPrefab = self->_noteCutSoundEffectPrefab;
    auto orig_bombCutSoundEffectPrefab = self->_bombCutSoundEffectPrefab;

    // edit prefabs
    self->_flyingTextEffectPrefab = PREFAB_INITIALIZE(_flyingTextEffectPrefab);
    self->_flyingScoreEffectPrefab = PREFAB_INITIALIZE(_flyingScoreEffectPrefab);
    self->_flyingSpriteEffectPrefab = PREFAB_INITIALIZE(_flyingSpriteEffectPrefab);
    if (shortBeatEffect) self->_shortBeatEffectPrefab = PREFAB_INITIALIZE(_shortBeatEffectPrefab);
    else self->_beatEffectPrefab = PREFAB_INITIALIZE(_beatEffectPrefab);
    self->_noteCutSoundEffectPrefab = PREFAB_INITIALIZE(_noteCutSoundEffectPrefab);
    self->_bombCutSoundEffectPrefab = PREFAB_INITIALIZE(_bombCutSoundEffectPrefab);

    // run method
    EffectPoolsManualInstaller_ManualInstallBindings(self, container, shortBeatEffect);

    // restore fields
    self->_flyingTextEffectPrefab = orig_flyingTextEffectPrefab;
    self->_flyingScoreEffectPrefab = orig_flyingScoreEffectPrefab;
    self->_flyingSpriteEffectPrefab = orig_flyingSpriteEffectPrefab;
    if (shortBeatEffect) self->_shortBeatEffectPrefab = orig_shortBeatEffectPrefab;
    else self->_beatEffectPrefab = orig_beatEffectPrefab;
    self->_noteCutSoundEffectPrefab = orig_noteCutSoundEffectPrefab;
    self->_bombCutSoundEffectPrefab = orig_bombCutSoundEffectPrefab;
}

MAKE_AUTO_HOOK_ORIG_MATCH(MultiplayerConnectedPlayerInstaller_InstallBindings, &MultiplayerConnectedPlayerInstaller::InstallBindings, void, MultiplayerConnectedPlayerInstaller* self) {
    DEBUG("Redecoration MultiplayerConnectedPlayerInstaller_InstallBindings");
    auto container = self->get_Container();
    auto type = self->GetType();

    // save orig
    auto orig_connectedPlayerBeatmapObjectEventManagerPrefab = self->_connectedPlayerBeatmapObjectEventManagerPrefab;
    auto orig_connectedPlayerAudioTimeSyncControllerPrefab = self->_connectedPlayerAudioTimeSyncControllerPrefab;
    auto orig_multiplayerGameNoteControllerPrefab = self->_multiplayerGameNoteControllerPrefab;
    auto orig_multiplayerBurstSliderHeadGameNoteControllerPrefab = self->_multiplayerBurstSliderHeadGameNoteControllerPrefab;
    auto orig_multiplayerBurstSliderGameNoteControllerPrefab = self->_multiplayerBurstSliderGameNoteControllerPrefab;
    auto orig_multiplayerBombNoteControllerPrefab = self->_multiplayerBombNoteControllerPrefab;
    auto orig_multiplayerObstacleControllerPrefab = self->_multiplayerObstacleControllerPrefab;

    // edit prefabs
    self->_connectedPlayerBeatmapObjectEventManagerPrefab = PREFAB_INITIALIZE(_connectedPlayerBeatmapObjectEventManagerPrefab);
    self->_connectedPlayerAudioTimeSyncControllerPrefab = PREFAB_INITIALIZE(_connectedPlayerAudioTimeSyncControllerPrefab);
    self->_multiplayerGameNoteControllerPrefab = PREFAB_INITIALIZE(_multiplayerGameNoteControllerPrefab);
    self->_multiplayerBurstSliderHeadGameNoteControllerPrefab = PREFAB_INITIALIZE(_multiplayerBurstSliderHeadGameNoteControllerPrefab);
    self->_multiplayerBurstSliderGameNoteControllerPrefab = PREFAB_INITIALIZE(_multiplayerBurstSliderGameNoteControllerPrefab);
    self->_multiplayerBombNoteControllerPrefab = PREFAB_INITIALIZE(_multiplayerBombNoteControllerPrefab);
    self->_multiplayerObstacleControllerPrefab = PREFAB_INITIALIZE(_multiplayerObstacleControllerPrefab);

    // run method
    MultiplayerConnectedPlayerInstaller_InstallBindings(self);

    // restore fields
    self->_connectedPlayerBeatmapObjectEventManagerPrefab = orig_connectedPlayerBeatmapObjectEventManagerPrefab;
    self->_connectedPlayerAudioTimeSyncControllerPrefab = orig_connectedPlayerAudioTimeSyncControllerPrefab;
    self->_multiplayerGameNoteControllerPrefab = orig_multiplayerGameNoteControllerPrefab;
    self->_multiplayerBurstSliderHeadGameNoteControllerPrefab = orig_multiplayerBurstSliderHeadGameNoteControllerPrefab;
    self->_multiplayerBurstSliderGameNoteControllerPrefab = orig_multiplayerBurstSliderGameNoteControllerPrefab;
    self->_multiplayerBombNoteControllerPrefab = orig_multiplayerBombNoteControllerPrefab;
    self->_multiplayerObstacleControllerPrefab = orig_multiplayerObstacleControllerPrefab;
}

MAKE_AUTO_HOOK_ORIG_MATCH(MultiplayerLobbyInstaller_InstallBindings, &MultiplayerLobbyInstaller::InstallBindings, void, MultiplayerLobbyInstaller* self) {
	DEBUG("Redecoration MultiplayerLobbyInstaller_InstallBindings");
    auto container = self->get_Container();
    auto type = self->GetType();

    // save orig
    auto orig_multiplayerAvatarPlacePrefab = self->_multiplayerAvatarPlacePrefab;
    auto orig_multiplayerLobbyAvatarControllerPrefab = self->_multiplayerLobbyAvatarControllerPrefab;

    // edit prefabs
    self->_multiplayerAvatarPlacePrefab = PREFAB_INITIALIZE(_multiplayerAvatarPlacePrefab);
    self->_multiplayerLobbyAvatarControllerPrefab = PREFAB_INITIALIZE(_multiplayerLobbyAvatarControllerPrefab);

    // run method
    MultiplayerLobbyInstaller_InstallBindings(self);

    // restore fields
    self->_multiplayerAvatarPlacePrefab = orig_multiplayerAvatarPlacePrefab;
    self->_multiplayerLobbyAvatarControllerPrefab = orig_multiplayerLobbyAvatarControllerPrefab;
}

MAKE_AUTO_HOOK_ORIG_MATCH(MultiplayerPlayersManager_BindPlayerFactories, &MultiplayerPlayersManager::BindPlayerFactories, void, MultiplayerPlayersManager* self, ::MultiplayerPlayerLayout layout) {
    DEBUG("Redecoration MultiplayerPlayersManager_BindPlayerFactories");
    auto container = self->_container;
    auto type = self->GetType();

    // save orig
    auto orig_inactiveLocalPlayerControllerPrefab = self->_inactiveLocalPlayerControllerPrefab;
    auto orig_activeLocalPlayerDuelControllerPrefab = self->_activeLocalPlayerDuelControllerPrefab;
    auto orig_connectedPlayerDuelControllerPrefab = self->_connectedPlayerDuelControllerPrefab;
    auto orig_activeLocalPlayerControllerPrefab = self->_activeLocalPlayerControllerPrefab;
    auto orig_connectedPlayerControllerPrefab = self->_connectedPlayerControllerPrefab;

    // edit prefabs
    self->_inactiveLocalPlayerControllerPrefab = PREFAB_INITIALIZE(_inactiveLocalPlayerControllerPrefab);
    if (layout == MultiplayerPlayerLayout::Duel) {
        self->_activeLocalPlayerDuelControllerPrefab = PREFAB_INITIALIZE(_activeLocalPlayerDuelControllerPrefab);
        self->_connectedPlayerDuelControllerPrefab = PREFAB_INITIALIZE(_connectedPlayerDuelControllerPrefab);
    } else {
        self->_activeLocalPlayerControllerPrefab = PREFAB_INITIALIZE(_activeLocalPlayerControllerPrefab);
        self->_connectedPlayerControllerPrefab = PREFAB_INITIALIZE(_connectedPlayerControllerPrefab);
    }

    // run method
    MultiplayerPlayersManager_BindPlayerFactories(self, layout);

    // restore fields
    self->_inactiveLocalPlayerControllerPrefab = orig_inactiveLocalPlayerControllerPrefab;
    if (layout == MultiplayerPlayerLayout::Duel) {
        self->_activeLocalPlayerDuelControllerPrefab = orig_activeLocalPlayerDuelControllerPrefab;
        self->_connectedPlayerDuelControllerPrefab = orig_connectedPlayerDuelControllerPrefab;
    } else {
        self->_activeLocalPlayerControllerPrefab = orig_activeLocalPlayerControllerPrefab;
        self->_connectedPlayerControllerPrefab = orig_connectedPlayerControllerPrefab;
    }
}

MAKE_AUTO_HOOK_ORIG_MATCH(NoteDebrisPoolInstaller_InstallBindings, &NoteDebrisPoolInstaller::InstallBindings, void, NoteDebrisPoolInstaller* self) {
	DEBUG("Redecoration NoteDebrisPoolInstaller_InstallBindings");
    auto container = self->get_Container();
    auto type = self->GetType();
    bool hd = self->_noteDebrisHDConditionVariable->get_value();

    // save orig
    auto orig_normalNoteDebrisHDPrefab = self->_normalNoteDebrisHDPrefab;
    auto orig_burstSliderHeadNoteDebrisHDPrefab = self->_burstSliderHeadNoteDebrisHDPrefab;
    auto orig_burstSliderElementNoteHDPrefab = self->_burstSliderElementNoteHDPrefab;
    auto orig_normalNoteDebrisLWPrefab = self->_normalNoteDebrisLWPrefab;
    auto orig_burstSliderHeadNoteDebrisLWPrefab = self->_burstSliderHeadNoteDebrisLWPrefab;
    auto orig_burstSliderElementNoteLWPrefab = self->_burstSliderElementNoteLWPrefab;

    // edit prefabs
    if (hd) {
        self->_normalNoteDebrisHDPrefab = PREFAB_INITIALIZE(_normalNoteDebrisHDPrefab);
        self->_burstSliderHeadNoteDebrisHDPrefab = PREFAB_INITIALIZE(_burstSliderHeadNoteDebrisHDPrefab);
        self->_burstSliderElementNoteHDPrefab = PREFAB_INITIALIZE(_burstSliderElementNoteHDPrefab);
    } else {
        self->_normalNoteDebrisLWPrefab = PREFAB_INITIALIZE(_normalNoteDebrisLWPrefab);
        self->_burstSliderHeadNoteDebrisLWPrefab = PREFAB_INITIALIZE(_burstSliderHeadNoteDebrisLWPrefab);
        self->_burstSliderElementNoteLWPrefab = PREFAB_INITIALIZE(_burstSliderElementNoteLWPrefab);
    }

    // run method
    NoteDebrisPoolInstaller_InstallBindings(self);

    // restore fields
    if (hd) {
        self->_normalNoteDebrisHDPrefab = orig_normalNoteDebrisHDPrefab;
        self->_burstSliderHeadNoteDebrisHDPrefab = orig_burstSliderHeadNoteDebrisHDPrefab;
        self->_burstSliderElementNoteHDPrefab = orig_burstSliderElementNoteHDPrefab;
    } else {
        self->_normalNoteDebrisLWPrefab = orig_normalNoteDebrisLWPrefab;
        self->_burstSliderHeadNoteDebrisLWPrefab = orig_burstSliderHeadNoteDebrisLWPrefab;
        self->_burstSliderElementNoteLWPrefab = orig_burstSliderElementNoteLWPrefab;
    }
}

MAKE_AUTO_HOOK_ORIG_MATCH(FakeMirrorObjectsInstaller_InstallBindings, &FakeMirrorObjectsInstaller::InstallBindings, void, FakeMirrorObjectsInstaller* self) {
	DEBUG("Redecoration FakeMirrorObjectsInstaller_InstallBindings");
    auto container = self->get_Container();
    auto type = self->GetType();

    // save orig
    auto orig_mirroredGameNoteControllerPrefab = self->_mirroredGameNoteControllerPrefab;
    auto orig_mirroredBurstSliderHeadGameNoteControllerPrefab = self->_mirroredBurstSliderHeadGameNoteControllerPrefab;
    auto orig_mirroredBurstSliderGameNoteControllerPrefab = self->_mirroredBurstSliderGameNoteControllerPrefab;
    auto orig_mirroredBombNoteControllerPrefab = self->_mirroredBombNoteControllerPrefab;
    auto orig_mirroredObstacleControllerPrefab = self->_mirroredObstacleControllerPrefab;
    auto orig_mirroredSliderControllerPrefab = self->_mirroredSliderControllerPrefab;

    // edit prefabs
    self->_mirroredGameNoteControllerPrefab = PREFAB_INITIALIZE(_mirroredGameNoteControllerPrefab);
    self->_mirroredBurstSliderHeadGameNoteControllerPrefab = PREFAB_INITIALIZE(_mirroredBurstSliderHeadGameNoteControllerPrefab);
    self->_mirroredBurstSliderGameNoteControllerPrefab = PREFAB_INITIALIZE(_mirroredBurstSliderGameNoteControllerPrefab);
    self->_mirroredBombNoteControllerPrefab = PREFAB_INITIALIZE(_mirroredBombNoteControllerPrefab);
    self->_mirroredObstacleControllerPrefab = PREFAB_INITIALIZE(_mirroredObstacleControllerPrefab);
    self->_mirroredSliderControllerPrefab = PREFAB_INITIALIZE(_mirroredSliderControllerPrefab);

    // run method
    FakeMirrorObjectsInstaller_InstallBindings(self);

    // restore fields
    self->_mirroredGameNoteControllerPrefab = orig_mirroredGameNoteControllerPrefab;
    self->_mirroredBurstSliderHeadGameNoteControllerPrefab = orig_mirroredBurstSliderHeadGameNoteControllerPrefab;
    self->_mirroredBurstSliderGameNoteControllerPrefab = orig_mirroredBurstSliderGameNoteControllerPrefab;
    self->_mirroredBombNoteControllerPrefab = orig_mirroredBombNoteControllerPrefab;
    self->_mirroredObstacleControllerPrefab = orig_mirroredObstacleControllerPrefab;
    self->_mirroredSliderControllerPrefab = orig_mirroredSliderControllerPrefab;
}
