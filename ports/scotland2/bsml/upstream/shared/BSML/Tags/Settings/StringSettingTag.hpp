#pragma once

#include "../../../_config.h"
#include "../ModalKeyboardTag.hpp"

namespace BSML {
    class BSML_EXPORT StringSettingTag : public ModalKeyboardTag {
        private:
            using Base = ModalKeyboardTag;
        public:
            StringSettingTag() : Base() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
