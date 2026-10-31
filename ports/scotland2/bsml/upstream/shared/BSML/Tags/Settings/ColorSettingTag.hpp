#pragma once

#include "../../../_config.h"
#include "../ModalColorPickerTag.hpp"

namespace BSML {
    class BSML_EXPORT ColorSettingTag : public ModalColorPickerTag {
        private:
            using Base = ModalColorPickerTag;
        public:
            ColorSettingTag() : Base() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
