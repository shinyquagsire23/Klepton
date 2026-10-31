#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT ClickableImageTag : public BSMLTag {
        public:
            ClickableImageTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
