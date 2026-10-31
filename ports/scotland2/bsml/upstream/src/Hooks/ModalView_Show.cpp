#include "hooking.hpp"

#include "HMUI/ModalView.hpp"
#include "HMUI/Screen.hpp"
#include "UnityEngine/Transform.hpp"
#include "UnityEngine/Canvas.hpp"
#include "UnityEngine/GameObject.hpp"

MAKE_AUTO_HOOK_MATCH(ModalView_Show, &HMUI::ModalView::Show, void, HMUI::ModalView* self, bool animated, bool moveToCenter, System::Action* finishedCallback)
{
	ModalView_Show(self, animated, moveToCenter, finishedCallback);
	auto cb = self->_blockerGO->get_gameObject()->GetComponent<UnityEngine::Canvas*>();
	auto screen = self->get_transform()->get_parent()->get_gameObject()->GetComponentInParent<HMUI::Screen*>();
	auto canvases = screen->get_gameObject()->GetComponentsInChildren<UnityEngine::Canvas*>(true);

	int highest = 0;
	for (auto& canvas : canvases) {
		if (canvas->get_sortingLayerID() == cb->get_sortingLayerID()) {
			// if highest lower than current, assign
			if (highest < canvas->get_sortingOrder()) {
				highest = canvas->get_sortingOrder();
			}
		}
	}

	highest ++;
	cb->set_overrideSorting(true);
	cb->set_sortingOrder(highest);

	auto cm = self->get_gameObject()->GetComponent<UnityEngine::Canvas*>();
	cm->set_overrideSorting(true);
	cm->set_sortingOrder(highest + 1);
}
