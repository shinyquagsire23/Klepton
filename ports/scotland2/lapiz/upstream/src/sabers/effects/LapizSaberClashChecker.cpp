#include "sabers/effects/LapizSaberClashChecker.hpp"
#include "utilities/logging.hpp"

#include "GlobalNamespace/SaberMovementData.hpp"
#include "GlobalNamespace/BladeMovementDataElement.hpp"

#include "UnityEngine/Time.hpp"
DEFINE_TYPE(Lapiz::Sabers::Effects, LapizSaberClashChecker);

namespace Lapiz::Sabers::Effects {
    void LapizSaberClashChecker::ctor(::Zenject::DiContainer* container, GlobalNamespace::SaberManager* saberManager, Lapiz::Sabers::LapizSaberFactory* lapizSaberFactory) {
        INVOKE_CTOR();
        DEBUG("LapizSaberClashChecker ctor");
        _sabers = ListW<GlobalNamespace::Saber*>::New();
        _container = container;
        _saberManager = saberManager;
        _lapizSaberFactory = lapizSaberFactory;

        _sabers->Add(_saberManager->leftSaber);
        _sabers->Add(_saberManager->rightSaber);
        _lapizSaberFactory->SaberCreated += {&LapizSaberClashChecker::LapizSaberFactory_SaberCreated, this};
    }

    void LapizSaberClashChecker::Dispose() {
        _lapizSaberFactory->SaberCreated -= {&LapizSaberClashChecker::LapizSaberFactory_SaberCreated, this};
    }

    void LapizSaberClashChecker::LapizSaberFactory_SaberCreated(Lapiz::Sabers::LapizSaber* lapizSaber) {
        _extraSabersDetected = true;
        _sabers->Add(lapizSaber->_saber);
    }

    bool LapizSaberClashChecker::SaberClashChecker_AreSabersClashing_override(UnityEngine::Vector3& clashingPoint, bool& result) {
        if (!_extraSabersDetected) return true;
        if (_leftSaber->movementDataForLogic->get_lastAddedData().time < 0.1f) {
            clashingPoint = this->_clashingPoint;
            result = false;
            return false;
        }

        if (_prevGetFrameNum == UnityEngine::Time::get_frameCount()) {
            clashingPoint = this->_clashingPoint;
            result = _sabersAreClashing;
            return false;
        }

        _prevGetFrameNum = UnityEngine::Time::get_frameCount();
        int saberCount = _sabers.size();
        for (int i = 0; i < saberCount; i++) {
            auto saberA = _sabers[i];
            for (int h = 0; h < saberCount; h++) {
                if (i > h) {
                    auto saberB = _sabers[h];
                    if (
                        saberA == saberB || // if same saber
                        (!saberA || !saberA->m_CachedPtr.m_value) || // if null
                        (!saberB || !saberB->m_CachedPtr.m_value)) { // if null
                        break;
                    }
                    auto saberBladeTopPos = saberA->saberBladeTopPos;
                    auto saberBladeTopPos2 = saberB->saberBladeTopPos;
                    auto saberBladeBottomPos = saberA->saberBladeBottomPos;
                    auto saberBladeBottomPos2 = saberB->saberBladeBottomPos;
                    UnityEngine::Vector3 clashingPoint2;
                    if (saberA->get_isActiveAndEnabled() && saberB->get_isActiveAndEnabled() && SegmentToSegmentDist(saberBladeBottomPos, saberBladeTopPos, saberBladeBottomPos2, saberBladeTopPos2, byref(clashingPoint2)) < 0.08f)
                    {
                        if ((!_lastSaberA || !_lastSaberA->m_CachedPtr.m_value) && (!_lastSaberB || !_lastSaberB->m_CachedPtr.m_value))
                        {
                            _lastSaberA = saberA;
                            _lastSaberB = saberB;
                            if (NewSabersClashed.size() > 0)
                                NewSabersClashed.invoke(_lastSaberA, _lastSaberB);
                        }
                        clashingPoint = clashingPoint2;
                        this->_clashingPoint = clashingPoint;
                        _sabersAreClashing = result = true;
                        return false;
                    }
                    else
                    {
                        _lastSaberA = nullptr;
                        _lastSaberB = nullptr;
                        _sabersAreClashing = false;
                    }
                }
            }
        }
        clashingPoint = this->_clashingPoint;
        result = _sabersAreClashing;
        return false;
    }
}
