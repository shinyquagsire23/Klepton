#include "BSML/Parsing/BSMLStringValue.hpp"

namespace BSML {
    void BSMLStringValue::SetValue(System::Object* val) {
        if (val) {
            static auto stringKlass = classof(StringW);
            if (val->klass == stringKlass) {
                value = static_cast<std::string>(StringW(val));
            } else {
                value = static_cast<std::string>(val->ToString());
            }
        }
    }

    System::Object* BSMLStringValue::GetValue() {
        return static_cast<System::Object*>(StringW(value).convert());
    }
}
