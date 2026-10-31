#include "BSML/Animations/AnimationStateUpdater.hpp"

DEFINE_TYPE(BSML, AnimationStateUpdater);

namespace BSML {
    AnimationControllerData* AnimationStateUpdater::get_controllerData() {
        return _controllerData;
    }

    void AnimationStateUpdater::set_controllerData(AnimationControllerData* value) {
        if (_controllerData) {
            OnDisable();
            _controllerData->Remove(this);
        }
        _controllerData = value;
        if (_controllerData) _controllerData->Add(this);

        if (get_isActiveAndEnabled()) {
            OnEnable();
        }
    }

    void AnimationStateUpdater::OnEnable() {
        if (_controllerData && image) {
            _controllerData->get_activeImages()->Add(image);
        }
    }

    void AnimationStateUpdater::OnDisable() {
        if (_controllerData && image) {
            _controllerData->get_activeImages()->Remove(image);
        }
    }

    void AnimationStateUpdater::OnDestroy() {
        if (_controllerData) {
            if (image) {
                _controllerData->get_activeImages()->Remove(image);
            }

            _controllerData->Remove(this);
        }
    }
}
