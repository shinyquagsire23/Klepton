#pragma once

#include "../../_config.h"
#include "ModalTag.hpp"

namespace BSML {
    class BSML_EXPORT ModalColorPickerTag : public ModalTag {
        private:
            using Base = ModalTag;
        public:
            ModalColorPickerTag() : Base() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
