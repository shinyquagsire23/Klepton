#include "BSML/Tags/CustomListTag.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/utilities.hpp"
#include "logging.hpp"

#include "BSML/Components/TableView.hpp"
#include "BSML/Components/CustomCellListTableData.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ScrollRect.hpp"
#include "UnityEngine/UI/RectMask2D.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Vector2.hpp"
#include "UnityEngine/RenderMode.hpp"
#include "UnityEngine/AdditionalCanvasShaderChannels.hpp"
#include "HMUI/EventSystemListener.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/Touchable.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"
#include "GlobalNamespace/GameplaySetupViewController.hpp"
#include "GlobalNamespace/PlayerSettingsPanelController.hpp"
#include "GlobalNamespace/NoteJumpStartBeatOffsetDropdown.hpp"
#include "HMUI/SimpleTextDropdown.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;
namespace BSML {
    static BSMLNodeParser<CustomListTag> customListTagParser({"custom-list"});

    Canvas* get_customListCanvasTemplate() {
        static SafePtrUnity<Canvas> customListCanvasTemplate;
        if (!customListCanvasTemplate) {
            customListCanvasTemplate = Helpers::GetDiContainer()->Resolve<GlobalNamespace::GameplaySetupViewController*>()->_playerSettingsPanelController->_noteJumpStartBeatOffsetDropdown->_simpleTextDropdown->_tableView->GetComponent<Canvas*>();
        }
        return customListCanvasTemplate.ptr();
    }

    UnityEngine::GameObject* CustomListTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating Custom List");
        auto container = GameObject::New_ctor("BSMLCustomListContainer")->AddComponent<RectTransform*>();
        auto containerGameObject = container->get_gameObject();
        auto layoutElement = containerGameObject->AddComponent<LayoutElement*>();
        containerGameObject->get_transform()->SetParent(parent, false);

        auto gameObject = GameObject::New_ctor("BSMLCustomList");
        gameObject->get_transform()->SetParent(containerGameObject->get_transform(), false);
        gameObject->SetActive(false);

        auto customListCanvasTemplate = get_customListCanvasTemplate();
        auto scrollRect = gameObject->AddComponent<ScrollRect*>();
        auto canvas = gameObject->AddComponent<Canvas*>();
        canvas->set_additionalShaderChannels(customListCanvasTemplate->get_additionalShaderChannels());
        canvas->set_overrideSorting(customListCanvasTemplate->get_overrideSorting());
        canvas->set_pixelPerfect(customListCanvasTemplate->get_pixelPerfect());
        canvas->set_referencePixelsPerUnit(customListCanvasTemplate->get_referencePixelsPerUnit());
        canvas->set_renderMode(customListCanvasTemplate->get_renderMode());
        canvas->set_scaleFactor(customListCanvasTemplate->get_scaleFactor());
        canvas->set_sortingLayerID(customListCanvasTemplate->get_sortingLayerID());
        canvas->set_sortingOrder(customListCanvasTemplate->get_sortingOrder());
        canvas->set_worldCamera(customListCanvasTemplate->get_worldCamera());

        gameObject->AddComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();
        gameObject->AddComponent<HMUI::Touchable*>();
        gameObject->AddComponent<HMUI::EventSystemListener*>();

        auto scrollView = gameObject->AddComponent<HMUI::ScrollView*>();

        HMUI::TableView* tableView = gameObject->AddComponent<BSML::TableView*>();
        auto tableData = container->get_gameObject()->AddComponent<CustomCellListTableData*>();
        tableData->tableView = tableView;
        tableData->bsmlString = bsmlString;

        tableView->_preallocatedCells = ArrayW<HMUI::TableView::CellsGroup*>(il2cpp_array_size_t(0));
        tableView->_isInitialized = false;
        tableView->_scrollView = scrollView;

        auto viewPort = GameObject::New_ctor("ViewPort")->AddComponent<RectTransform*>();
        viewPort->SetParent(gameObject->get_transform(), false);
        viewPort->get_gameObject()->AddComponent<RectMask2D*>();
        scrollRect->set_viewport(viewPort);

        auto content = GameObject::New_ctor("Content")->AddComponent<RectTransform*>();
        content->SetParent(viewPort, false);

        scrollView->_contentRectTransform = content;
        scrollView->_viewport = viewPort;

        viewPort->set_anchorMin({0, 0});
        viewPort->set_anchorMax({1, 1});
        viewPort->set_anchoredPosition({0, 0});
        viewPort->set_sizeDelta({0, 0});

        auto tableViewRectTransform = tableView->transform.cast<RectTransform>();
        tableViewRectTransform->set_anchorMin({0, 0});
        tableViewRectTransform->set_anchorMax({1, 1});
        tableViewRectTransform->set_anchoredPosition({0, 0});
        tableViewRectTransform->set_sizeDelta({0, 0});

        tableView->SetDataSource(tableData->i_IDataSource(), false);
        return containerGameObject;
    }

    void CustomListTag::parse(const tinyxml2::XMLElement& elem) {
        BSMLTag::parse(elem);
        INFO("Getting BSML string contents");

        bsmlString = "";
        auto handle = tinyxml2::XMLConstHandle(elem).FirstChildElement();
        for (
            const tinyxml2::XMLElement* element = nullptr;
            (element = handle.ToElement()) != nullptr;
            handle = handle.NextSiblingElement()
        ) {
            tinyxml2::XMLPrinter printer(nullptr , true);
            element->Accept(&printer);
            bsmlString += std::string_view(printer.CStr(), printer.CStrSize());
        }

        INFO("Got BSML string: {}", bsmlString);
    }

    void CustomListTag::HandleChildren(UnityEngine::Transform* parent, BSMLParserParams& parserParams, std::vector<ComponentTypeWithData*>& componentInfo) const {
        // intentionally not doing anything
    }
}
