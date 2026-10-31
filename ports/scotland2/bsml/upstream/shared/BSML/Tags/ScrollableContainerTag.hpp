#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT ScrollableContainerTag : public BSMLTag {
        public:
            ScrollableContainerTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject *CreateObject(UnityEngine::Transform *parent) const override;
    };
}
