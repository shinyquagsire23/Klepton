#include "Helpers/extension.hpp"

#include "BGLib/Polyglot/LocalizedTextMeshProUGUI.hpp"
#include "TMPro/TMP_Text.hpp"


using namespace TMPro;
using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML::Helpers {
    void SetButtonText(Button* button, StringW text) {
            auto localizer = button->GetComponentInChildren<BGLib::Polyglot::LocalizedTextMeshProUGUI*>(true);
            if (localizer)
                Object::DestroyImmediate(localizer);

            auto tmpUgui = button->GetComponentInChildren<TMP_Text*>();
            if (tmpUgui)
                tmpUgui->SetText(text, true);
    }

}
