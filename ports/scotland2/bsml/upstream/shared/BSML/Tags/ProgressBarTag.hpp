#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT ProgressBarTag : public BSMLTag {
        private:
            using Base = BSMLTag;
        public:
            ProgressBarTag() : Base() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform *parent) const override;
    };
}
