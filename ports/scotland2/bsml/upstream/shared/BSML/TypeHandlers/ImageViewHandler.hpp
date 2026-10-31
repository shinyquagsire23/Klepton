#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "HMUI/ImageView.hpp"

namespace BSML {
    class BSML_EXPORT ImageViewHandler : public TypeHandler<HMUI::ImageView*> {
        public:
            using Base = TypeHandler<HMUI::ImageView*>;
            ImageViewHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
    };
}
