#include "BSML/Tags/ListTag.hpp"
#include "Helpers/getters.hpp"
#include "Helpers/utilities.hpp"
#include "logging.hpp"

#include "BSML/Components/TableView.hpp"
#include "BSML/Components/CustomListTableData.hpp"

#include "UnityEngine/RectTransform.hpp"
#include "UnityEngine/Resources.hpp"
#include "UnityEngine/UI/LayoutElement.hpp"
#include "UnityEngine/UI/ScrollRect.hpp"
#include "UnityEngine/UI/RectMask2D.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/RenderMode.hpp"
#include "UnityEngine/AdditionalCanvasShaderChannels.hpp"
#include "UnityEngine/Vector2.hpp"
#include "HMUI/EventSystemListener.hpp"
#include "HMUI/ScrollView.hpp"
#include "HMUI/TableView.hpp"
#include "HMUI/Touchable.hpp"
#include "VRUIControls/VRGraphicRaycaster.hpp"

using namespace UnityEngine;
using namespace UnityEngine::UI;

namespace BSML {
    static BSMLNodeParser<ListTag> listTagParser({"list", "list2"});
    Canvas* get_listCanvasTemplate() {
        static SafePtrUnity<Canvas> listCanvasTemplate;
        if (!listCanvasTemplate) {
            listCanvasTemplate = Resources::FindObjectsOfTypeAll<Canvas*>()->FirstOrDefault([](auto x){ return x->get_name() == "DropdownTableView"; });
        }
        return listCanvasTemplate.ptr();
    }

    UnityEngine::GameObject* ListTag::CreateObject(UnityEngine::Transform* parent) const {
        DEBUG("Creating List");
        auto container = GameObject::New_ctor("BSMLListContainer")->AddComponent<RectTransform*>();
        auto containerGameObject = container->get_gameObject();
        auto layoutElement = containerGameObject->AddComponent<LayoutElement*>();
        containerGameObject->get_transform()->SetParent(parent, false);

        auto gameObject = GameObject::New_ctor("BSMLList");
        gameObject->get_transform()->SetParent(containerGameObject->get_transform(), false);
        gameObject->SetActive(false);

        auto listCanvasTemplate = get_listCanvasTemplate();
        auto scrollRect = gameObject->AddComponent<ScrollRect*>();
        auto canvas = gameObject->AddComponent<Canvas*>();
        // using this method causes the list to have it's cells be squished:
        //Utilities::AddComponent(gameObject, listCanvasTemplate);
        // therefore we just copy what we need:
        canvas->set_additionalShaderChannels(listCanvasTemplate->get_additionalShaderChannels());
        canvas->set_overrideSorting(listCanvasTemplate->get_overrideSorting());
        canvas->set_pixelPerfect(listCanvasTemplate->get_pixelPerfect());
        canvas->set_referencePixelsPerUnit(listCanvasTemplate->get_referencePixelsPerUnit());
        canvas->set_renderMode(listCanvasTemplate->get_renderMode());
        canvas->set_scaleFactor(listCanvasTemplate->get_scaleFactor());
        canvas->set_sortingLayerID(listCanvasTemplate->get_sortingLayerID());
        canvas->set_sortingOrder(listCanvasTemplate->get_sortingOrder());
        canvas->set_worldCamera(listCanvasTemplate->get_worldCamera());

        gameObject->AddComponent<VRUIControls::VRGraphicRaycaster*>()->_physicsRaycaster = Helpers::GetPhysicsRaycasterWithCache();
        gameObject->AddComponent<HMUI::Touchable*>();
        gameObject->AddComponent<HMUI::EventSystemListener*>();

        auto scrollView = gameObject->AddComponent<HMUI::ScrollView*>();

        HMUI::TableView* tableView = gameObject->AddComponent<BSML::TableView*>();
        auto tableData = container->get_gameObject()->AddComponent<CustomListTableData*>();
        tableData->tableView = tableView;

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
}
