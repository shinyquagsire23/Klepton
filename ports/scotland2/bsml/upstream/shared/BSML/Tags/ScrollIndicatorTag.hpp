#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT ScrollIndicatorTag : public BSMLTag {
        public:
            ScrollIndicatorTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform *parent) const override;
    };
}
