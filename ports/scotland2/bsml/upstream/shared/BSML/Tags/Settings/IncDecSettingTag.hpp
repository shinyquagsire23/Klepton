#pragma once

#include "../../../_config.h"
#include "../BSMLTag.hpp"
#include "../../Components/Settings/IncDecSetting.hpp"

namespace BSML {
    class BSML_EXPORT IncDecSettingTagBase : public BSMLTag {
        public:
            IncDecSettingTagBase() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
            virtual System::Type* get_type() const = 0;
    };

    template<typename T>
    requires(std::is_convertible_v<T, ::BSML::IncDecSetting*>)
    class BSML_EXPORT IncDecSettingTag : public IncDecSettingTagBase {
        public:
            IncDecSettingTag() : IncDecSettingTagBase() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override {
                return IncDecSettingTagBase::CreateObject(parent);
            }
            System::Type* get_type() const override { return csTypeOf(T); };
    };
}
