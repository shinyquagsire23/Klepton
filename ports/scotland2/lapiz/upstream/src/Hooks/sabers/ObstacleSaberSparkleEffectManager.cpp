#include "utilities/hooking.hpp"

#include "sabers/effects/ObstacleSaberSparkleEffectManagerLatch.hpp"
#include "GlobalNamespace/BeatmapObjectManager.hpp"
#include "GlobalNamespace/ObstacleController.hpp"
#include "GlobalNamespace/HapticFeedbackManager.hpp"
#include "GlobalNamespace/SaberType.hpp"
#include "UnityEngine/XR/XRNode.hpp"
#include "System/Action_1.hpp"

using namespace Lapiz::Sabers::Effects;

MAKE_AUTO_HOOK_MATCH(ObstacleSaberSparkleEffectManager_Start, &GlobalNamespace::ObstacleSaberSparkleEffectManager::Start, void, GlobalNamespace::ObstacleSaberSparkleEffectManager* self) {
    ObstacleSaberSparkleEffectManager_Start(self);
    auto instance = ObstacleSaberSparkleEffectManagerLatch::get_instance();
    if (instance) {
        instance->ObstacleSaberSparkleEffectManager_Start_Postfix(self);
    }
}