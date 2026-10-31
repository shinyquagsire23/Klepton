#include "objects/ObjectStateContainer.hpp"

namespace Lapiz::Objects {
            ObjectStateContainer::ObjectStateContainer(UnityEngine::Transform* mainParent) {
                if (!mainParent || !mainParent->m_CachedPtr.m_value) {
                    throw std::invalid_argument("Null transform passed!");
                }
                Snapshot(mainParent, objects);
            }

            void ObjectStateContainer::Revert() {
                for (auto& o : objects) {
                    o.Revert();
                }
            }

            void ObjectStateContainer::Snapshot(UnityEngine::Transform* transform, std::vector<ObjectState>& populator) {
                int childCount = transform->get_childCount();
                for (int i = 0; i < childCount; i++) {
                    auto child = transform->GetChild(i);
                    populator.emplace_back(child);
                    Snapshot(child, populator);
                }
            }
            
            ObjectStateContainer::ObjectState::ObjectState(UnityEngine::Transform* transform) {
                active = transform->get_gameObject()->get_activeSelf();
                pose = UnityEngine::Pose(transform->get_localPosition(), transform->get_localRotation());
                scale = transform->get_localScale();
                this->transform = transform;
                parent = transform->get_parent();
            }

            void ObjectStateContainer::ObjectState::Revert() {
                if (transform && transform->m_CachedPtr.m_value) {
                    if (parent && parent->m_CachedPtr.m_value) transform->SetParent(parent);
                    transform->set_localScale(scale);
                    transform->get_gameObject()->SetActive(active);
                    transform->set_localPosition(pose.position);
                    transform->set_localRotation(pose.rotation);
                }
            }
}
