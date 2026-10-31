#include "utilities/hooking.hpp"
#include "utilities/logging.hpp"

#include "sabers/effects/SaberBurnMarkSparklesLatch.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Vector3.hpp"
#include "UnityEngine/Color.hpp"
#include "UnityEngine/Color32.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/ParticleSystem.hpp"

static inline UnityEngine::Vector3 operator*(UnityEngine::Vector3 vec, float v) {
    return { vec.x * v, vec.y * v, vec.z * v };
}

static inline UnityEngine::Vector3 operator/(UnityEngine::Vector3 vec, float v) {
    return { vec.x / v, vec.y / v, vec.z / v };
}

static inline UnityEngine::Vector3 operator+(UnityEngine::Vector3 a, UnityEngine::Vector3 b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}

static inline UnityEngine::Vector3 operator-(UnityEngine::Vector3 a, UnityEngine::Vector3 b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
MAKE_AUTO_HOOK_MATCH(SaberBurnMarkSparkles_Start, &GlobalNamespace::SaberBurnMarkSparkles::Start, void, GlobalNamespace::SaberBurnMarkSparkles* self) {
    SaberBurnMarkSparkles_Start(self);
    auto instance = Lapiz::Sabers::Effects::SaberBurnMarkSparklesLatch::get_instance();
    if (instance) {
        instance->SaberBurnMarkSparkles_Start_Postfix(self);
    }
}

void SaberBurnMarkSparkles_LateUpdate_Replacement(GlobalNamespace::SaberBurnMarkSparkles* self);

MAKE_AUTO_HOOK_ORIG_MATCH(SaberBurnMarkSparkles_LateUpdate, &GlobalNamespace::SaberBurnMarkSparkles::LateUpdate, void, GlobalNamespace::SaberBurnMarkSparkles* self) {
    auto instance = Lapiz::Sabers::Effects::SaberBurnMarkSparklesLatch::get_instance();
    if (instance) {
        instance->SaberBurnMarkSparkles_LateUpdate_Prefix(self);
        SaberBurnMarkSparkles_LateUpdate_Replacement(self);
        instance->SaberBurnMarkSparkles_LateUpdate_Postfix(self);
    } else {
        SaberBurnMarkSparkles_LateUpdate_Replacement(self);
    }
}

void SaberBurnMarkSparkles_LateUpdate_Replacement(GlobalNamespace::SaberBurnMarkSparkles* self) {
	for (int i = 0; i < self->_sabers.size(); i++) {
        auto saber = self->_sabers[i];
		UnityEngine::Vector3 vector{0.0f, 0.0f, 0.0f};
		bool flag = saber->get_isActiveAndEnabled() && self->GetBurnMarkPos(saber->saberBladeBottomPos, saber->saberBladeTopPos, byref(vector));

        if (flag) {
			self->_burnMarksPS[i]->get_transform()->set_localPosition(vector);
		} else if (!self->_prevBurnMarkPosValid[i]) {
            self->_burnMarksPS[i]->Clear();
        }
        if (!self->_prevBurnMarkPosValid[i])
            self->_burnMarksEmissionModules[i].set_enabled(flag);
        UnityEngine::Color32 col;
        auto saberColor = self->_colorManager->ColorForSaberType(saber->get_saberType());
        col.r = saberColor.r * 255;
        col.g = saberColor.g * 255;
        col.b = saberColor.b * 255;
        col.a = saberColor.a * 255;
		self->_sparklesEmitParams.set_startColor(col);
		if (flag && self->_prevBurnMarkPosValid[i]) {
			auto a = vector - self->_prevBurnMarkPos[i];
			float magnitude = a.get_magnitude();
			float num = 0.05f;
			int num2 = (int)(magnitude / num);
			int num3 = (num2 > 0) ? num2 : 1;
			for (int j = 0; j <= num2; j++) {
				self->_sparklesEmitParams.set_position(self->_prevBurnMarkPos[i] + a * (float)j / (float)num3);
				self->_sparklesPS->Emit(self->_sparklesEmitParams, 1);
			}
		}
		self->_prevBurnMarkPosValid[i] = flag;
		self->_prevBurnMarkPos[i] = vector;
	}
}

MAKE_AUTO_HOOK_ORIG_MATCH(ColorManager_ColorForSaberType, &GlobalNamespace::ColorManager::ColorForSaberType, UnityEngine::Color, GlobalNamespace::ColorManager* self, GlobalNamespace::SaberType type) {
    auto instance = Lapiz::Sabers::Effects::SaberBurnMarkSparklesLatch::get_instance();
    UnityEngine::Color color;
    if (!instance || instance->ColorManager_ColorForSaberType_Prefix(color)) {
        color = ColorManager_ColorForSaberType(self, type);
    }
    return color;
}

// DOES NOT CALL ORIG
MAKE_AUTO_HOOK_ORIG_MATCH(SaberBurnMarkSparkles_OnEnable, &GlobalNamespace::SaberBurnMarkSparkles::OnEnable, void, GlobalNamespace::SaberBurnMarkSparkles* self) {
	if (!self->_burnMarksPS) return;
    for (auto ps : self->_burnMarksPS) {
        if (ps && ps->m_CachedPtr.m_value) {
            ps->get_gameObject()->SetActive(true);
        }
	}
}

// DOES NOT CALL ORIG
MAKE_AUTO_HOOK_ORIG_MATCH(SaberBurnMarkSparkles_OnDisable, &GlobalNamespace::SaberBurnMarkSparkles::OnDisable, void, GlobalNamespace::SaberBurnMarkSparkles* self) {
	if (!self->_burnMarksPS) return;
    for (auto ps : self->_burnMarksPS) {
        if (ps && ps->m_CachedPtr.m_value) {
            ps->get_gameObject()->SetActive(false);
        }
	}
}
