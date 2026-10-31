#include "objects/LapizPrefabContainer.hpp"

#include "UnityEngine/Transform.hpp"

DEFINE_TYPE(Lapiz::Objects, LapizPrefabContainer);

namespace Lapiz::Objects {
    UnityEngine::GameObject* LapizPrefabContainer::get_prefab() {
        return _prefab;
    }

    void LapizPrefabContainer::set_prefab(UnityEngine::GameObject* value) {
        _prefab = UnityEngine::Object::Instantiate(value);
        _prefab->get_transform()->SetParent(get_transform());
        _prefab->SetActive(false);
    }

    void LapizPrefabContainer::Start() {
        if (get_transform()->get_childCount() > 0) {
            _prefab = get_transform()->GetChild(0)->get_gameObject();
        }
    }
}
