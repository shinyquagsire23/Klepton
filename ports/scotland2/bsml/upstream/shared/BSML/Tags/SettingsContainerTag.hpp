#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"
#include "ScrollViewTag.hpp"

namespace BSML {
    class BSML_EXPORT SettingsContainerTag : public ScrollViewTag {
        public:
            SettingsContainerTag() : ScrollViewTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
