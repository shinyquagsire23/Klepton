#include "sabers/effects/SaberClashEffectAdjuster.hpp"

#include "UnityEngine/Color.hpp"
#include "UnityEngine/ParticleSystem.hpp"

DEFINE_TYPE(Lapiz::Sabers::Effects, SaberClashEffectAdjuster);

namespace Lapiz::Sabers::Effects {
    SaberClashEffectAdjuster* SaberClashEffectAdjuster::instance;
    SaberClashEffectAdjuster* SaberClashEffectAdjuster::get_instance() {
        return instance;
    }

    void SaberClashEffectAdjuster::ctor(Lapiz::Sabers::SaberModelManager* saberModelManager, GlobalNamespace::SaberClashChecker* saberClashChecker) {
        INVOKE_CTOR();
        instance = this;
        _saberModelManager = saberModelManager;
        _saberClashChecker = il2cpp_utils::try_cast<Lapiz::Sabers::Effects::LapizSaberClashChecker>(saberClashChecker).value_or(nullptr);
    }

    void SaberClashEffectAdjuster::Initialize() {
        _saberClashChecker->NewSabersClashed += {&SaberClashEffectAdjuster::SaberClashChecker_NewSabersClashed, this};
    }

    void SaberClashEffectAdjuster::Dispose() {
        instance = nullptr;
        _saberClashChecker->NewSabersClashed -= {&SaberClashEffectAdjuster::SaberClashChecker_NewSabersClashed, this};
    }

    void SaberClashEffectAdjuster::SaberClashChecker_NewSabersClashed(GlobalNamespace::Saber* saberA, GlobalNamespace::Saber* saberB) {
        if (!_glowParticleSystem || !_glowParticleSystem->m_CachedPtr.m_value) return;
        if (!_sparkleParticleSystem || !_sparkleParticleSystem->m_CachedPtr.m_value) return;
        if (!_saberClashEffect || !_saberClashEffect->m_CachedPtr.m_value) return;

        auto colorA = _saberModelManager->GetPhysicalSaberColor(saberA);
        auto colorB = _saberModelManager->GetPhysicalSaberColor(saberB);

        auto combined = UnityEngine::Color::Lerp(colorA, colorB, 0.5f);
        combined.a = 1.0f;

        auto gradient = UnityEngine::ParticleSystem::MinMaxGradient();
        gradient._ctor(combined);

        _glowParticleSystem->get_main().set_startColor(gradient);
        _sparkleParticleSystem->get_main().set_startColor(gradient);
    }

    void SaberClashEffectAdjuster::SaberClashEffect_Start_Postfix(GlobalNamespace::SaberClashEffect* self) {
        _saberClashEffect = self;
        _glowParticleSystem = self->_glowParticleSystem;
        _sparkleParticleSystem = self->_sparkleParticleSystem;
    }
}
