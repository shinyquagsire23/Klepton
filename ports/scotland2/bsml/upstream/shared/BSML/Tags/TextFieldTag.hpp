#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"
#include "HMUI/InputFieldView.hpp"

namespace BSML {
    class BSML_EXPORT TextFieldTag : public BSMLTag {
        public:
            TextFieldTag() : BSMLTag() {}
        protected:
            virtual HMUI::InputFieldView* get_fieldViewPrefab() const;
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
