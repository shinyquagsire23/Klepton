#pragma once

#include "../../_config.h"
#include "ModalTag.hpp"

namespace BSML {
    class BSML_EXPORT ModalKeyboardTag : public ModalTag {
        public:
            ModalKeyboardTag() : ModalTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
