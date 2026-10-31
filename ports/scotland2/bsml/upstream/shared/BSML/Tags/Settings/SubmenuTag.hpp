#pragma once

#include "../../../_config.h"
#include "../BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT SubmenuTag : public BSMLTag {
        public:
            SubmenuTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
