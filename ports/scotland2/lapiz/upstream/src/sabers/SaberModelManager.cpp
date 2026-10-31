#include "sabers/SaberModelManager.hpp"

#include "utilities/logging.hpp"
#include "utilities/saberutil.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/Transform.hpp"

DEFINE_TYPE(Lapiz::Sabers, DesperationContract);
DEFINE_TYPE(Lapiz::Sabers, SaberModelManager);

static inline bool operator==(UnityEngine::Color a, UnityEngine::Color b) {
    return !(
        a.r != b.r ||
        a.g != b.g ||
        a.b != b.b ||
        a.a != b.a
    );
}
namespace Lapiz::Sabers {
    /* SaberModelManager */
    void SaberModelManager::ctor(GlobalNamespace::ColorManager* colorManager, LapizSaberFactory* lapizSaberFactory) {
        INVOKE_CTOR();
        DEBUG("SaberModelManager ctor");
        _lapizSaberLink = LapizSaberLink::New_ctor();
        _saberModelLink = SaberModelLink::New_ctor();
        _desperationList = ListW<DesperationContract*>::New();
        _salvationList = ListW<DesperationContract*>::New();

        _colorManager = colorManager;
        _lapizSaberFactory = lapizSaberFactory;
        _lapizSaberFactory->SaberCreated += {&SaberModelManager::SiraSaberFactory_SaberCreated, this};
    }

    void SaberModelManager::SiraSaberFactory_SaberCreated(LapizSaber* lapizSaber) {
        DEBUG("SaberModelManager SiraSaberFactory_SaberCreated");
        _lapizSaberLink->Add(lapizSaber->_saber, lapizSaber);
        _saberModelLink->Add(lapizSaber->_saber, lapizSaber->_saberModelController);
    }

    GlobalNamespace::SaberModelController* SaberModelManager::GetSaberModelController(GlobalNamespace::Saber* saber) {
        DEBUG("SaberModelManager GetSaberModelController");
        GlobalNamespace::SaberModelController* smc = nullptr;
        if (_saberModelLink->TryGetValue(saber, byref(smc))) {
            return smc;
        } else {
            auto t = saber->get_transform();
            int childCount = t->get_childCount();
            for (int i = 0; i < childCount; i++) {
                auto child = t->GetChild(i);
                auto saberModelController = child->get_gameObject()->GetComponent<GlobalNamespace::SaberModelController*>();
                if (saberModelController) {
                    smc = saberModelController;
                }
            }
            if (smc) {
                _saberModelLink->Add(saber, smc);
                return smc;
            }
        }
        return nullptr;
    }

    UnityEngine::Color SaberModelManager::GetPhysicalSaberColor(GlobalNamespace::Saber* saber) {
        DEBUG("SaberModelManager GetPhysicalSaberColor");
        auto saberModelController = GetSaberModelController(saber);
        if (saberModelController) {
            return SaberUtil::GetColor(saberModelController);
        }
        return _colorManager->ColorForSaberType(saber->get_saberType());
    }

    void SaberModelManager::SetColor(GlobalNamespace::Saber* saber, UnityEngine::Color color) {
        DEBUG("SaberModelManager SetColor");
        LapizSaber* lapizSaber = nullptr;
        if (_lapizSaberLink->TryGetValue(saber, byref(lapizSaber))) {
            lapizSaber->SetColor(color);
        } else {
            auto saberModelController = GetSaberModelController(saber);
            if (saberModelController && saberModelController->m_CachedPtr.m_value) {
                _colorUpdateQueue.push([saber, saberModelController, color, this](){
                    SaberUtil::SetColor(saberModelController, color);
                    if (ColorUpdated.size() > 0) ColorUpdated.invoke(saber, color);
                });

                auto contractItr = std::find_if(_desperationList.begin(), _desperationList.end(), [saber, color](auto x){
                    return x->get_saber() == saber && x->_color == color;
                });

                if (contractItr != _desperationList.end()) {
                    _salvationList->Add(*contractItr);
                }
            } else {
                auto existingItr = std::find_if(_desperationList.begin(), _desperationList.end(), [saber](auto x){ return x->get_saber() == saber; });
                if (existingItr == _desperationList.end()) {
                    _desperationList->Add(DesperationContract::New_ctor(saber, color));
                }
            }
        }
    }


    void SaberModelManager::LateTick() {
        while (!_colorUpdateQueue.empty()) {
            _colorUpdateQueue.front()();
            _colorUpdateQueue.pop();
        }

        for (auto desperator : _desperationList) {
            if (desperator->get_saber()) {
                SetColor(desperator->get_saber(), desperator->_color);
            }
        }

        for (auto salvation : _salvationList) {
            _desperationList->Remove(salvation);
        }

        _salvationList->Clear();
    }

    void SaberModelManager::Dispose() {
        _lapizSaberFactory->SaberCreated -= {&SaberModelManager::SiraSaberFactory_SaberCreated, this};
    }

    /* DesperationContract */
    GlobalNamespace::Saber* DesperationContract::get_saber() {
        _accessed++;
        if (_accessed == _maxAccessTimes) {
            auto saber = _saber;
            _saber = nullptr;
            return saber;
        }
        return _saber;
    }

    void DesperationContract::ctor(GlobalNamespace::Saber* saber, UnityEngine::Color color) {
        _saber = saber;
        _color = color;
    }

}
