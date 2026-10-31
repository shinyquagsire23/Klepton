#include "BSML/Components/Settings/DropdownListSetting.hpp"
#include "Helpers/delegates.hpp"
#include "logging.hpp"

#include "TMPro/TextMeshProUGUI.hpp"
#include "UnityEngine/UI/Button.hpp"
#include "UnityEngine/GameObject.hpp"
#include "System/Action_2.hpp"
#include "System/Object.hpp"

DEFINE_TYPE(BSML, DropdownListSetting);

namespace BSML {
    void DropdownListSetting::ctor() {
        genericSetting = GenericSettingWrapper::New_ctor();
        values = ListW<System::Object*>::New();
        index = 0;
        formatter = nullptr;
    }

    bool DropdownListSetting::get_interactable() {
        return dropdown ? dropdown->_button->get_interactable() : false;
    }

    void DropdownListSetting::set_interactable(bool value) {
        if (dropdown) dropdown->_button->set_interactable(value);
    }

    void DropdownListSetting::Setup() {
        if (dropdown) {
            std::function<void(UnityW<HMUI::DropdownWithTableView>, int)> fun = std::bind(&DropdownListSetting::OnSelectIndex, this, std::placeholders::_1, std::placeholders::_2);
            auto delegate = MakeSystemAction(fun);
            dropdown->add_didSelectCellWithIdxEvent(delegate);
        }

        ReceiveValue();
        UpdateChoices();
        get_gameObject()->SetActive(true);
    }

    void DropdownListSetting::UpdateChoices() {
        auto texts = ListW<StringW>::New();
        texts->EnsureCapacity(values->get_Count());
        bool formatted = formatter != nullptr;
        for (auto v : values) {
            if (!v) texts->Add("NULL");
            if (formatted) [[unlikely]] {
                texts->Add(formatter(v));
            } else [[likely]] {
                texts->Add(v->ToString());
            }
        }

        dropdown->SetTexts(*texts);
    }

    void DropdownListSetting::ValidateRange() {
        if (index >= values.size())
            index = values.size() - 1;

        if (index < 0)
            index = 0;
    }

    void DropdownListSetting::UpdateState() {
        if (dropdown && dropdown->_text) {
            auto value = get_Value();
            if (value) {
                dropdown->_text->set_text(formatter ? formatter(value) : value->ToString() );
            } else {
                dropdown->_text->set_text("NULL");
            }
        }
    }

    void DropdownListSetting::OnSelectIndex(HMUI::DropdownWithTableView* tableView, int index) {
        this->index = index;
        UpdateState();
        if (genericSetting) {
            auto v = get_Value();
            genericSetting->OnChange(v);
            if (onChange) onChange(v);
            if (genericSetting->applyOnChange) ApplyValue();
        }
    }

    void DropdownListSetting::ReceiveValue() {
        if (!genericSetting) return;
        set_Value(genericSetting->GetValue<System::Object*>());
    }

    void DropdownListSetting::ApplyValue() {
        if (genericSetting)
            genericSetting->SetValue(get_Value());
    }

    System::Object* DropdownListSetting::get_Value() {
        ValidateRange();
        return values[index];
    }

    void DropdownListSetting::set_Value(System::Object* value) {
        index = 0;
        for (auto& v : values) {
            // if both are the same, or v has a value and Equals the value
            if ((v == value) || (v && v->Equals(value)))
                break;
            index++;
        }

        if (index == values.size())
            index = 0;

        dropdown->SelectCellWithIdx(index);

        UpdateState();
    }
}
