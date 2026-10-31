#include "BSML/Components/ScrollIndicator.hpp"

DEFINE_TYPE(BSML, ScrollIndicator);

namespace BSML
{
    UnityEngine::RectTransform *ScrollIndicator::get_Handle() {
        return _handle;
    }

    void ScrollIndicator::set_Handle(UnityEngine::RectTransform *value) {
        _handle = value;
    }
}
