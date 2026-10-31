#include "sabers/LapizSaber.hpp"

#include "UnityEngine/Transform.hpp"
#include "UnityEngine/GameObject.hpp"
#include "UnityEngine/LayerMask.hpp"
#include "UnityEngine/Vector3.hpp"
#include "GlobalNamespace/TimeHelper.hpp"
#include "GlobalNamespace/SaberMovementData.hpp"
#include "utilities/saberutil.hpp"

DEFINE_TYPE(Lapiz::Sabers, LapizSaber);

namespace Lapiz::Sabers {
    void LapizSaber::Construct(GlobalNamespace::NoteCutter* noteCutter, GlobalNamespace::ColorManager* colorManager, SaberModelProvider* saberModelProvider) {
        _noteCutter = noteCutter;
        _colorManager = colorManager;
        _saberModelProvider = saberModelProvider;
    }

    void LapizSaber::Update() {
        if (_saber && _saber->m_CachedPtr.m_value && _saber->gameObject->activeInHierarchy) {
            auto topTransform = _saber->_saberBladeTopTransform;
            auto bottomTransform = _saber->_saberBladeBottomTransform;

            auto topPosition = topTransform->position;
            _saber->_saberBladeTopPos = topPosition;
            auto bottomPosition = bottomTransform->position;
            _saber->_saberBladeBottomPos = bottomPosition;

            auto time = GlobalNamespace::TimeHelper::get_time();
            _trailSaberMovementData->AddNewData(topPosition, bottomPosition, time);

            if (_saber->enabled) {
                _saber->_movementData->AddNewData(topPosition, bottomPosition, time);
                _noteCutter->Cut(_saber);
            }
        }

        if (!_colorProcessNextFrame.empty()) {
            _colorProcessNextFrame.front()();
            _colorProcessNextFrame.pop();
        }
    }

    void LapizSaber::LateUpdate() {
        _constructedThisFrame = false;
    }

    void LapizSaber::SetType(GlobalNamespace::SaberType saberType) {
        _saberTypeObject->_saberType = saberType;
        SaberUtil::SetColor(_saberModelController, _colorManager->ColorForSaberType(saberType));
    }

    void LapizSaber::SetColor(UnityEngine::Color color) {
        if (!_constructedThisFrame) {
            SaberUtil::SetColor(_saberModelController, color);
            if (colorUpdated) colorUpdated(_saber, color);
        } else {
            _colorProcessNextFrame.push([this, color](){
                SaberUtil::SetColor(_saberModelController, color);
                if (colorUpdated) colorUpdated(_saber, color);
            });
        }
    }

    void LapizSaber::Setup(System::Type* backingType, GlobalNamespace::SaberType saberType) {
        auto gameObject = this->gameObject;
        _saberTypeObject = gameObject->AddComponent<GlobalNamespace::SaberTypeObject*>();
        _saber = gameObject->AddComponent(backingType).cast<GlobalNamespace::Saber>();
        static int layer = UnityEngine::LayerMask::NameToLayer("Saber");
        gameObject->set_layer(layer);

        _saber->_saberType = _saberTypeObject;

        auto top = UnityEngine::GameObject::New_ctor("Top")->transform;
        auto bottom = UnityEngine::GameObject::New_ctor("Bottom")->transform;

        top->SetParent(this->transform);
        bottom->SetParent(this->transform);

        top->position = {0, 0, 1};

        _saber->_saberBladeTopTransform = top;
        _saber->_handleTransform = bottom;
        _saber->_saberBladeBottomTransform = bottom;

        _saber->_saberBladeTopPos = top->position;
        _saber->_saberBladeBottomPos = bottom->position;

        _saberTypeObject->_saberType = saberType;
        _saberModelController = _saberModelProvider->NewModel(saberType);
        _saberModelController->Init(this->transform, _saber, _saberModelController->____colorManager->ColorForSaberType(saberType));

        _trailSaberMovementData = GlobalNamespace::SaberMovementData::New_ctor();
        _saberModelController->_saberTrail->_movementData = *_trailSaberMovementData;
        _constructedThisFrame = true;
    }
}
