#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"
#include "UnityEngine/UI/Button.hpp"

namespace BSML {
    class BSML_EXPORT ButtonTag : public BSMLTag {
        public:
            ButtonTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
            virtual UnityEngine::UI::Button* get_buttonPrefab() const;
    };
}
