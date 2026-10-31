#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT LoadingIndicatorTag : public BSMLTag {
        public:
            LoadingIndicatorTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
