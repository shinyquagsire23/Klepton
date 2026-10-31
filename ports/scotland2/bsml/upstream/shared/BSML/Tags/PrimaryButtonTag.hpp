#pragma once

#include "../../_config.h"
#include "ButtonTag.hpp"

namespace BSML {
    class BSML_EXPORT PrimaryButtonTag : public ButtonTag {
        private:
            using Base = ButtonTag;
        public:
            PrimaryButtonTag() : Base() {}
        protected:
            virtual UnityEngine::UI::Button* get_buttonPrefab() const override;
    };
}
