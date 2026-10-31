#pragma once

#include "../../_config.h"
#include "BackgroundTag.hpp"

namespace BSML {
    class BSML_EXPORT TabTag : public BackgroundTag {
        public:
            using Base = BackgroundTag;
            TabTag() : BackgroundTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
