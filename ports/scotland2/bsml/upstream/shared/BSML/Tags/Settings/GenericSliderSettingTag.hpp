#pragma once

#include "../../../_config.h"
#include "../BSMLTag.hpp"
#include "../../Components/Settings/SliderSettingBase.hpp"

namespace BSML {
    class BSML_EXPORT GenericSliderSettingTagBase : public BSMLTag {
        public:
            GenericSliderSettingTagBase() : BSMLTag() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override;
            virtual System::Type* get_type() const = 0;
    };

    template<typename T>
    requires(std::is_convertible_v<T, ::BSML::SliderSettingBase*>)
    class BSML_EXPORT GenericSliderSettingTag : public GenericSliderSettingTagBase {
        public:
            GenericSliderSettingTag() : GenericSliderSettingTagBase() {}
        protected:
            virtual UnityEngine::GameObject* CreateObject(UnityEngine::Transform* parent) const override {
                return GenericSliderSettingTagBase::CreateObject(parent);
            }
            System::Type* get_type() const override { return csTypeOf(T); };
    };
}
