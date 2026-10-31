#include "sabers/effects/ObstacleSaberSparkleEffectManagerLatch.hpp"
#include "utilities/logging.hpp"
#include "utilities/typeutil.hpp"

#include "UnityEngine/Transform.hpp"
#include "GlobalNamespace/SaberType.hpp"

DEFINE_TYPE(Lapiz::Sabers::Effects, ObstacleSaberSparkleEffectManagerLatch);

namespace Lapiz::Sabers::Effects {
    ObstacleSaberSparkleEffectManagerLatch* ObstacleSaberSparkleEffectManagerLatch::instance;
    ObstacleSaberSparkleEffectManagerLatch* ObstacleSaberSparkleEffectManagerLatch::get_instance() {
        return instance;
    }

    void ObstacleSaberSparkleEffectManagerLatch::ctor(GlobalNamespace::ColorManager* colorManager, Lapiz::Sabers::LapizSaberFactory* lapizSaberFactory) {
        INVOKE_CTOR();
        instance = this;
        _earlySabers = EarlySabers::New_ctor();
        _colorManager = colorManager;
        _lapizSaberFactory = lapizSaberFactory;
        _lapizSaberFactory->SaberCreated += {&ObstacleSaberSparkleEffectManagerLatch::LapizSaberFactory_SaberCreated, this};
    }

    void ObstacleSaberSparkleEffectManagerLatch::Dispose() {
        instance = nullptr;
        _lapizSaberFactory->SaberCreated -= {&ObstacleSaberSparkleEffectManagerLatch::LapizSaberFactory_SaberCreated, this};
    }

    void ObstacleSaberSparkleEffectManagerLatch::LapizSaberFactory_SaberCreated(Lapiz::Sabers::LapizSaber* lapizSaber) {
        if (!_obstacleSaberSparkleEffectManager || !_obstacleSaberSparkleEffectManager->m_CachedPtr.m_value) {
            _earlySabers->Enqueue(lapizSaber);
        } else {
            AddSaber(lapizSaber->_saber);
        }
    }

    void ObstacleSaberSparkleEffectManagerLatch::AddSaber(GlobalNamespace::Saber* saber) {
        if (!_obstacleSaberSparkleEffectManager || !_obstacleSaberSparkleEffectManager->m_CachedPtr.m_value) return;

        _obstacleSaberSparkleEffectManager->_sabers = TypeUtil::AppendArrayOrDefault(_obstacleSaberSparkleEffectManager->_sabers, saber);

        auto effect = CreateNewObstacleSaberSparkleEffect();
        _obstacleSaberSparkleEffectManager->_effects = TypeUtil::AppendArrayOrDefault(_obstacleSaberSparkleEffectManager->_effects, effect);

    }

    GlobalNamespace::ObstacleSaberSparkleEffect* ObstacleSaberSparkleEffectManagerLatch::CreateNewObstacleSaberSparkleEffect() {
        auto obstacleSaberSparkleEffect = UnityEngine::Object::Instantiate(_obstacleSaberSparkleEffectManager->_obstacleSaberSparkleEffectPrefab);
        obstacleSaberSparkleEffect->set_name(fmt::format("Lapiz | {}", obstacleSaberSparkleEffect->get_name()));
        obstacleSaberSparkleEffect->set_color(_colorManager->GetObstacleEffectColor());
        return obstacleSaberSparkleEffect;
    }

    void ObstacleSaberSparkleEffectManagerLatch::ObstacleSaberSparkleEffectManager_Start_Postfix(GlobalNamespace::ObstacleSaberSparkleEffectManager* self) {
        _obstacleSaberSparkleEffectManager = self;
        auto iter = _earlySabers->GetEnumerator();
        while(iter.MoveNext()) {
            AddSaber(iter.get_Current()->_saber);
        }
        _earlySabers->Clear();
    }
}
