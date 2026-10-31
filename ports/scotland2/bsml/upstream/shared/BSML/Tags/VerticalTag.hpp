#pragma once

#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT VerticalTag : public BSMLTag {
        public:
            VerticalTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
