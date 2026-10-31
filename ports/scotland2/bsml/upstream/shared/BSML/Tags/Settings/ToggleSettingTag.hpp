#pragma once

#include "../../../_config.h"
#include "../BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT ToggleSettingTag : public BSMLTag {
        public:
            ToggleSettingTag() : BSMLTag() {}

        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
