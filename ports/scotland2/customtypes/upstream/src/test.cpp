// If LOCAL_TEST is defined, create a mod that uses custom types.
// Otherwise, it will be built as a library.
#ifdef LOCAL_TEST
#include <unordered_map>
#include "NoteData.hpp"
#include "beatsaber-hook/shared/utils/hooking.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-functions.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-type-check.hpp"
#include "beatsaber-hook/shared/utils/il2cpp-utils.hpp"
#include "beatsaber-hook/shared/utils/logging.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"
#include "beatsaber-hook/shared/utils/utils.h"
#include "coroutine.hpp"
#include "logging.hpp"
#include "macros.hpp"
#include "register.hpp"
#include "scotland2/shared/loader.hpp"
#include "types.hpp"

modloader::ModInfo modInfo{ "test", "0.0.0", 0 };

static constexpr auto logger = ::custom_types::logger;

DECLARE_CLASS(Il2CppNamespace, MyType, "UnityEngine", "MonoBehaviour", sizeof(Il2CppObject) + sizeof(void*)) {
    // TODO: Fields need to be copied from base type, size needs to be adjusted to match, offsets of all declared fields need to be correct
    // DECLARE_INSTANCE_FIELD(int, x);
    // DECLARE_INSTANCE_FIELD(Vector3, y);

    DECLARE_CTOR(ctor);

    DECLARE_INSTANCE_METHOD(void, Start);

    DECLARE_STATIC_METHOD(void, Test, int x, float y);

    DECLARE_INSTANCE_METHOD(int, asdf, int q);

    DECLARE_STATIC_FIELD(int, x);

    DECLARE_OVERRIDE_METHOD(Il2CppString*, ToString, il2cpp_utils::FindMethod("UnityEngine", "Object", "ToString"));
};

DEFINE_TYPE(Il2CppNamespace, MyType);

void Il2CppNamespace::MyType::ctor() {
    logger.debug("Called Il2CppNamespace::MyType::ctor");
    // y = {1, 2, 3};
    // x = 5;
}

void Il2CppNamespace::MyType::Start() {
    logger.debug("Called Il2CppNamespace::MyType::Start!");
    logger.debug("Return of asdf(1): {}", asdf(1));
    // Runtime invoke it.
    // We ARE NOT able to call GetClassFromName.
    // This is because our class name is NOT in the nameToClassHashTable
    // However, we ARE able to get our Il2CppClass* via the klass private static field of this type.
    auto* il2cppKlass = il2cpp_utils::GetClassFromName("Il2CppNamespace", "MyType");
    logger.debug("il2cpp obtained klass: {}", fmt::ptr(il2cppKlass));
    logger.debug("klass: {}", fmt::ptr(___TypeRegistration::klass_ptr));
    auto* asdfMethod = il2cpp_utils::FindMethodUnsafe(___TypeRegistration::klass_ptr, "asdf", 1);
    logger.debug("asdf method info: {}", fmt::ptr(asdfMethod));
    // logger.debug("x: {}", x);
    // logger.debug("y: ({}, {}, {})", y.x, y.y, y.z);
    logger.debug("runtime invoke of asdf(1): {}", CRASH_UNLESS(il2cpp_utils::RunMethodOpt<int>(this, asdfMethod, 1)));
}

int Il2CppNamespace::MyType::asdf(int q) {
    return q + 1;
}

Il2CppString* Il2CppNamespace::MyType::ToString() {
    // We want to create a C# string that is different than from what might otherwise be expected!
    logger.info("Calling custom ToString!");
    return il2cpp_utils::newcsstr("My Custom ToString!");
}

void Il2CppNamespace::MyType::Test([[maybe_unused]] int x, [[maybe_unused]] float y) {
    logger.debug("Called Il2CppNamespace::MyType::Test!");
}

DECLARE_CLASS_DLL(Il2CppNamespace, MyTypeDllTest, "UnityEngine", "MonoBehaviour", sizeof(Il2CppObject) + sizeof(void*), "MyCoolDll.dll") {
    DECLARE_CTOR(ctor);
};

DEFINE_TYPE(Il2CppNamespace, MyTypeDllTest);

void Il2CppNamespace::MyTypeDllTest::ctor() {
    logger.debug("MyTypeDllTest debug call!");
    custom_types::logImage(classof(MyTypeDllTest*)->image);
}

DECLARE_CLASS_INTERFACES(Il2CppNamespace, MyCustomBeatmapLevelPackCollection, "System", "Object", sizeof(Il2CppObject), INTERFACE_NAME("", "IBeatmapLevelPackCollection")) {
    DECLARE_INSTANCE_FIELD(Il2CppArray*, wrappedArr);

    DECLARE_OVERRIDE_METHOD(Il2CppArray*, get_beatmapLevelPacks, il2cpp_utils::FindMethod("", "IBeatmapLevelPackCollection", "get_beatmapLevelPacks"));
    DECLARE_CTOR(ctor, Il2CppArray* originalArray);
};

DEFINE_TYPE(Il2CppNamespace, MyCustomBeatmapLevelPackCollection);

void Il2CppNamespace::MyCustomBeatmapLevelPackCollection::ctor(Il2CppArray* originalArray) {
    // We want to basically wrap the original instance.
    // Also log.
    wrappedArr = originalArray;
    logger.debug("Added original array: {}", fmt::ptr(originalArray));
}

Il2CppArray* Il2CppNamespace::MyCustomBeatmapLevelPackCollection::get_beatmapLevelPacks() {
    logger.debug("My cool getter wrappedArr: {}!", fmt::ptr(wrappedArr));
    return wrappedArr;
}

DECLARE_CLASS_CUSTOM(Il2CppNamespace, MyCustomBeatmapCollection2, Il2CppNamespace::MyCustomBeatmapLevelPackCollection) {
    DECLARE_CTOR(ctor, Il2CppArray* originalArray);
};

DEFINE_TYPE(Il2CppNamespace, MyCustomBeatmapCollection2);

void Il2CppNamespace::MyCustomBeatmapCollection2::ctor(Il2CppArray* originalArray) {
    wrappedArr = originalArray;
    logger.debug("Custom inherited type original array: {}", fmt::ptr(originalArray));
}

// TODO: Self references still do not work!
// DECLARE_CLASS(SmallTest, Test, "System", "Object", sizeof(Il2CppObject)) {
//     DECLARE_STATIC_METHOD(SmallTest::Test*, SelfRef, int x);
//     DECLARE_STATIC_FIELD(SmallTest::Test*, selfRefField);
//     DECLARE_STATIC_FIELD(Il2CppNamespace::MyType*, AnotherRef);
// };

// DEFINE_TYPE(SmallTest, Test);

// SmallTest::Test* SmallTest::Test::SelfRef(int x) {
//     return CRASH_UNLESS(il2cpp_utils::New<SmallTest::Test*>());
// }

DECLARE_VALUE(ValueTest, Test, "System", "ValueType", 0) {
    DECLARE_INSTANCE_FIELD(int, x);
    DECLARE_INSTANCE_FIELD(int, y);
    DECLARE_INSTANCE_FIELD(int, z);
};

DEFINE_TYPE(ValueTest, Test);

DECLARE_CLASS(SmallTest, TestIt2, "System", "Object", sizeof(Il2CppObject)) {
    std::vector<void*> allocField;
    int x = 3;
    DECLARE_CTOR(ctor);
    DECLARE_SIMPLE_DTOR();
};

DEFINE_TYPE(SmallTest, TestIt2);

void SmallTest::TestIt2::ctor() {
    // This invokes the C++ constructor for this type
    INVOKE_CTOR();
    logger.debug("X is: {}", x);
}

DECLARE_CLASS(SmallTest, TestIt3, "System", "Object", sizeof(Il2CppObject)) {
    // These fields are initialized in the DEFAULT_CTOR
    public:
    std::vector<void*> allocField;
    int x = 3;
    DECLARE_DEFAULT_CTOR();
    DECLARE_SIMPLE_DTOR();
};

DEFINE_TYPE(SmallTest, TestIt3);

// DECLARE_CLASS_INTERFACES(Il2CppNamespace, MyBeatmapObjectManager, "", "BeatmapObjectManager", INTERFACE_NAME("", "IBeatmapObjectSpawner")) {
//     DECLARE_CTOR(ctor);
//     DECLARE_OVERRIDE_METHOD(void, SpawnBasicNote, il2cpp_utils::FindMethodUnsafe("", "IBeatmapObjectSpawner", "SpawnBasicNote", 11),
//     NoteData* noteData, Vector3 moveStartPos, Vector3 moveEndPos, Vector3 jumpEndPos, float moveDuration, float jumpDuration, float jumpGravity, float rotation, bool disappearingArrow, bool
//     ghostNote, float cutDirectionAngleOffset); REGISTER_FUNCTION(MyBeatmapObjectManager,
//         logger.debug("Registering MyBeatmapObjectManager!");
//         REGISTER_METHOD(ctor);
//         REGISTER_METHOD(SpawnBasicNote);
//     )
//     public:
//     static inline Il2CppObject* actualBeatmapObjectManager;
//     static inline void setActual(Il2CppObject* beatmapObjectManager) {
//         actualBeatmapObjectManager = actualBeatmapObjectManager;
//     }
// };

// void Il2CppNamespace::MyBeatmapObjectManager::ctor() {
//     logger.debug("Called Il2CppNamespace::MyBeatmapObjectManager::ctor");
// }

// void Il2CppNamespace::MyBeatmapObjectManager::SpawnBasicNote(NoteData* noteData, Vector3 moveStartPos, Vector3 moveEndPos, Vector3 jumpEndPos, float moveDuration, float jumpDuration, float
// jumpGravity, float rotation, bool disappearingArrow, bool ghostNote, float cutDirectionAngleOffset) {
//     logger.debug("Called Il2CppNamespace::MyBeatmapObjectManager::SpawnBasicNote, calling orig now!");
//     auto* method = il2cpp_utils::FindMethodUnsafe("", "BeatmapObjectManager", "SpawnBasicNote", 11);
//     il2cpp_utils::RunMethod(actualBeatmapObjectManager, method, noteData, moveStartPos, moveEndPos, jumpEndPos, moveDuration, jumpDuration, jumpGravity, rotation);
// }

static custom_types::ClassWrapper* klassWrapper;

CUSTOM_TYPES_FUNC void setup(CModInfo* info) {
    info->id = MOD_ID;
    info->version = VERSION;
    info->version_long = VERSION_LONG;
    modInfo.assign(*info);
    logger.debug("Completed setup!");
}

MAKE_HOOK_FIND_CLASS_UNSAFE_INSTANCE(MainMenuViewController_DidActivate, "", "MainMenuViewController", "DidActivate", void, Il2CppObject* self, bool firstActivation, bool addedToHierarchy,
                                     bool screenSystemEnabling) {
    logger.debug("MainMenuViewController.DidActivate!");
    MainMenuViewController_DidActivate(self, firstActivation, addedToHierarchy, screenSystemEnabling);
    logger.debug("Getting GO...");
    auto* go = RET_V_UNLESS(logger, il2cpp_utils::GetPropertyValue(self, "gameObject").value_or(nullptr));
    logger.debug("Got GO: {}", fmt::ptr(go));
    custom_types::logAll(classof(Il2CppNamespace::MyType*));
    custom_types::logAll(classof(Il2CppNamespace::MyType*)->parent);
    auto* customType = il2cpp_utils::GetSystemType(custom_types::Register::classes[0]);
    logger.debug("Custom System.Type: {}", fmt::ptr(customType));
    auto strType = RET_V_UNLESS(logger, il2cpp_utils::RunMethodOpt<StringW>(customType, "ToString"));
    logger.debug("ToString: {}", strType);
    auto name = RET_V_UNLESS(logger, il2cpp_utils::GetPropertyValue<StringW>(customType, "Name"));
    logger.debug("Name: {}", name);
    logger.debug("Actual type: {}", fmt::ptr(&custom_types::Register::classes[0]->byval_arg));
    logger.debug("Type: {}", fmt::ptr(customType->type));
    // crashNow = true;
    auto* comp = RET_V_UNLESS(logger, il2cpp_utils::RunMethodOpt<Il2CppObject*>(go, "AddComponent", customType));
    logger.debug("Custom Type added as a component: {}", fmt::ptr(comp));
}

// static bool first = false;
// MAKE_HOOK_FIND_CLASS_UNSAFE_INSTANCE(BeatmapObjectSpawnController_SpawnNote, void, Il2CppObject* self, Il2CppObject* noteData, float cutDirAngle) {
//     if (!first) {
//         first = true;
//         // We log, set the interface field, log, and then call orig.
//         logger.debug("Calling BeatmapObjectSpawnController.SpawnNote!");
//         auto* created = CRASH_UNLESS(il2cpp_utils::New<Il2CppNamespace::MyBeatmapObjectManager*>("Il2CppNamespace", "MyBeatmapObjectManager"));
//         logger.debug("Created MyBeatmapObjectManager: {}!", fmt::ptr(created));
//         auto* old = CRASH_UNLESS(il2cpp_utils::GetFieldValue(self, "_beatmapObjectSpawner"));
//         logger.debug("Got old: {}!", fmt::ptr(old));
//         Il2CppNamespace::MyBeatmapObjectManager::setActual(old);
//         logger.debug("Set actual to old!");
//         il2cpp_utils::SetFieldValue(self, "_beatmapObjectSpawner", created);
//         logger.debug("Set current to MyBeatmapObjectManager: {}!", fmt::ptr(created));
//         // Call orig
//         BeatmapObjectSpawnController_SpawnNote(self, noteData, cutDirAngle);
//         logger.debug("Called orig!");
//         il2cpp_utils::SetFieldValue(self, "_beatmapObjectSpawner", old);
//         // Then, we set the field back.
//         logger.debug("Set field back to old: {}", fmt::ptr(old));
//     } else {
//         BeatmapObjectSpawnController_SpawnNote(self, noteData, cutDirAngle);
//     }
// }

MAKE_HOOK_FIND_CLASS_UNSAFE_INSTANCE(BeatmapLevelModels_UpdateAllLoadedBeatmapLevelPacks, "", "BeatmapLevelModels", "UpdateAllLoadedBeatmapLevelPacks", void, Il2CppObject* self) {
    BeatmapLevelModels_UpdateAllLoadedBeatmapLevelPacks(self);
    auto* existing = CRASH_UNLESS(il2cpp_utils::GetFieldValue(self, "_allLoadedBeatmapLevelPackCollection"));
    logger.debug("Existing: {}", fmt::ptr(existing));
    auto* arr = CRASH_UNLESS(il2cpp_utils::GetPropertyValue(existing, "beatmapLevelPacks"));
    logger.debug("Existing arr: {}", fmt::ptr(arr));
    logger.debug("Constructing custom type and setting it to field!");
    // auto* myType = CRASH_UNLESS(il2cpp_utils::New<Il2CppNamespace::MyCustomBeatmapLevelPackCollection*>(arr));
    auto myType = Il2CppNamespace::MyCustomBeatmapLevelPackCollection::New_ctor((Il2CppArray*)arr);
    logger.debug("Created new type: {}", fmt::ptr(myType));
    auto* k = il2cpp_functions::object_get_class(existing);
    custom_types::logAll(k);
    k = il2cpp_functions::object_get_class((Il2CppObject*)myType);
    custom_types::logAll(k);
    CRASH_UNLESS(il2cpp_utils::SetFieldValue(self, "_allLoadedBeatmapLevelPackCollection", myType));
}

CUSTOM_TYPES_FUNC void load() {
    static constexpr auto& logger = custom_types::logger;
    logger.debug("Registering types! (current size: {})", custom_types::Register::classes.size());
    custom_types::Register::AutoRegister();
    logger.debug("Registered: {} types!", custom_types::Register::classes.size());
    INSTALL_HOOK(logger, MainMenuViewController_DidActivate);
    INSTALL_HOOK(logger, BeatmapLevelModels_UpdateAllLoadedBeatmapLevelPacks);
    // auto k = CRASH_UNLESS(custom_types::Register::RegisterType<Il2CppNamespace::MyBeatmapObjectManager>());
    // INSTALL_HOOK_OFFSETLESS(BeatmapObjectSpawnController_SpawnNote, il2cpp_utils::FindMethodUnsafe("", "BeatmapObjectSpawnController", "SpawnNote", 2));
    // il2cpp_utils::LogClass(il2cpp_utils::GetClassFromName("Il2CppNamespace", "MyBeatmapObjectManager"));
    // logger.debug("Vtables for MyBeatmapObjectManager: {}", k->get()->vtable_count);
    logger.debug("Custom types size: {}", custom_types::Register::classes.size());
    logger.debug("Logging vtables for custom type! There are: {} vtables", custom_types::Register::classes[0]->vtable_count);
    il2cpp_utils::LogClass(logger, custom_types::Register::classes[0]);
    il2cpp_utils::LogClass(logger, custom_types::Register::classes[1]);
    il2cpp_utils::New<SmallTest::TestIt2*>();
    auto* testIt3 = CRASH_UNLESS(il2cpp_utils::New<SmallTest::TestIt3*>());
    logger.debug("testIt3 default value {}", testIt3->x);
    for (auto itr : custom_types::Register::classes) {
        logger.debug("Image for custom type: {}::{} {}", itr->namespaze, itr->name, fmt::ptr(itr->image) );
    }
    // custom_types::logImage(custom_types::Register::classes[0]->image);
    // for (int i = 0; i < k->get()->vtable_count; i++) {
    //     custom_types::logVtable(&k->get()->vtable[i]);
    // }

    // logger.debug("Vtables for parent: {}", k->get()->parent->vtable_count);
    // for (int i = 0; i < k->get()->parent->vtable_count; i++) {
    //     custom_types::logVtable(&k->get()->parent->vtable[i]);
    // }
    // custom_types::logMethod(il2cpp_utils::FindMethodUnsafe("", "IBeatmapObjectSpawner", "SpawnBasicNote", 11));
}
#endif
