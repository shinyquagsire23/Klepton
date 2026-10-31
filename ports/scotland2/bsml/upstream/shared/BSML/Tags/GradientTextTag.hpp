#pragma once

#include "../../_config.h"
#include "TextTag.hpp"

namespace BSML {
    class BSML_EXPORT GradientTextTag : public TextTag {
        public:
            GradientTextTag() : TextTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
