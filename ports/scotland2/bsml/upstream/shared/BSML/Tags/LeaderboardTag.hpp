#pragma once

#include "../../_config.h"
#include "BSMLTag.hpp"

namespace BSML {
    class BSML_EXPORT LeaderboardTag : public BSMLTag {
        public:
            LeaderboardTag() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
    };
}
