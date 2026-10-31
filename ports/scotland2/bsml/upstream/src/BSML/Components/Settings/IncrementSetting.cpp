#include "BSML/Components/Settings/IncrementSetting.hpp"
#include "logging.hpp"
#include <limits>
#include <fmt/core.h>

DEFINE_TYPE(BSML, IncrementSetting);

namespace BSML {
    float IncrementSetting::get_Value() {
        ValidateRange();
        return currentValue;
    }
    
    void IncrementSetting::set_Value(float value) {
        if (isInt)
            currentValue = ConvertToInt(value);
        else 
            currentValue = value;
        UpdateState();
    }

    void IncrementSetting::ctor() {
        this->construct();
        minValue = -std::numeric_limits<float>::infinity();
        maxValue = std::numeric_limits<float>::infinity();
        increments = 1.0f;
        isInt = false;
        currentValue = 0.0f;
        digits = 2;
    }

    void IncrementSetting::Setup() {
        UpdateState();
        ReceiveValue();
    }

    void IncrementSetting::IncButtonPressed() {
        currentValue += increments;
        EitherPressed();
    }

    void IncrementSetting::DecButtonPressed() {
        currentValue -= increments;
        EitherPressed();
    }

    void IncrementSetting::EitherPressed() {
        UpdateState();

        if (genericSetting) {
            if (isInt) {
                genericSetting->OnChange(ConvertToInt(currentValue));
            } else {
                genericSetting->OnChange(currentValue);
            }
            if (onChange) onChange(currentValue);
            if (genericSetting->applyOnChange) ApplyValue();
        }
        
    }

    void IncrementSetting::ReceiveValue() {
        if (!genericSetting) return;
        set_Value(genericSetting->GetValue<float>());
    }

    void IncrementSetting::ApplyValue() {
        if (!genericSetting) {
            DEBUG("Can't apply value on nullptr genericSetting");
            return;
        }
        
        if (isInt) {
            genericSetting->SetValue(ConvertToInt(currentValue));
        } else {
            genericSetting->SetValue(currentValue);
        }
    }

    void IncrementSetting::ValidateRange() {
        if (currentValue < minValue)
            currentValue = minValue;
        else if (currentValue > maxValue)
            currentValue = maxValue;
    }

    void IncrementSetting::UpdateState() {
        ValidateRange();

        set_enableDec(currentValue > minValue);
        set_enableInc(currentValue < maxValue);
        set_text(TextForValue(currentValue));
    }

    StringW IncrementSetting::TextForValue(float value) {
        // since the formatter can't differentiate between int and float, we just call it
        if (formatter) return formatter(value);

        if (isInt) {
            return fmt::format("{}", ConvertToInt(value));
        } else {
            return fmt::format("{:.{}f}", value, digits);
        }
    }

    int IncrementSetting::ConvertToInt(float value) {
        if (value < std::numeric_limits<int>::min()) {
            return -std::numeric_limits<int>::infinity();
        }
        if (value > std::numeric_limits<int>::max()) {
            return std::numeric_limits<int>::infinity();
        }
        return value;
    }
}