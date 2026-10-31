#include "objects/ObjectDiffuser.hpp"

DEFINE_TYPE(Lapiz::Objects, ObjectDiffuser);

namespace Lapiz::Objects {
    void ObjectDiffuser::Resolved() {
        if (_object && _object->m_CachedPtr.m_value) {
            UnityEngine::Object::Destroy(_object);
        }
        _object = nullptr;
    }

    void ObjectDiffuser::ctor(UnityEngine::Object* obj) {
        _object = obj;
    }
}
