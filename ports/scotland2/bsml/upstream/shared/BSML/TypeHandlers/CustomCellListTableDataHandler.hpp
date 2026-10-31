#pragma once

#include "../../_config.h"
#include "TypeHandler.hpp"
#include "../Components/CustomCellListTableData.hpp"

namespace BSML {
    class BSML_EXPORT CustomCellListTableDataHandler : public TypeHandler<BSML::CustomCellListTableData*> {
        public:
            using Base = TypeHandler<BSML::CustomCellListTableData*>;
            CustomCellListTableDataHandler() : Base() {}

            virtual Base::PropMap get_props() const override;
            virtual Base::SetterMap get_setters() const override;
            virtual void HandleType(const ComponentTypeWithData& componentType, BSMLParserParams& parserParams) override;
    };
}
