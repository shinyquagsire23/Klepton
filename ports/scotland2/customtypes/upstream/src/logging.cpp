#include "logging.hpp"
#include "beatsaber-hook/shared/utils/typedefs.h"

namespace custom_types {

void logAname(const Il2CppAssemblyName* name, std::string_view anameLabel) {
    if (!name) {
        custom_types::logger.debug("{}: NULL ASSEMBLY NAME!", anameLabel.data());
        return;
    }
    custom_types::logger.debug("0 ======================ASSEMBLY NAME INFO FOR: {}======================", anameLabel.data());
    custom_types::logger.debug("name: {}", name->name ? name->name : "NULL");
    custom_types::logger.debug("culture: {}", name->culture ? name->culture : "NULL");
    custom_types::logger.debug("public_key: {}", name->public_key ? (char*)name->public_key : "NULL");
    custom_types::logger.debug("hash_alg: {}", name->hash_alg);
    custom_types::logger.debug("hash_len: {}", name->hash_len);
    custom_types::logger.debug("flags: 0x{:x}", name->flags);
    custom_types::logger.debug("major: {}", name->major);
    custom_types::logger.debug("minor: {}", name->minor);
    custom_types::logger.debug("build: {}", name->build);
    custom_types::logger.debug("revision: {}", name->revision);
    custom_types::logger.debug("public_key_token: {}", fmt::ptr(name->public_key_token));
    custom_types::logger.debug("0 ======================END ASSEMBLY NAME INFO======================");
}

void logFields(const Il2CppClass* klass) {
    if (!klass) {
        custom_types::logger.debug("NULL IL2CPPCLASS* FOR LOG FIELDS!");
        return;
    }
    custom_types::logger.debug("0 ======================FIELDS FOR: {}======================", fmt::ptr(klass));
    custom_types::logger.debug("field_count: {}", klass->field_count);
    if (klass->fields) {
        for (uint16_t i = 0; i < klass->field_count; i++) {
            custom_types::logger.debug("0 ======================FIELD: {}======================", i);
            auto* fieldRef = &klass->fields[i];
            custom_types::logger.debug("name: {}", fieldRef->name);
            custom_types::logger.debug("offset: 0x{:x}", fieldRef->offset);
            custom_types::logger.debug("parent: {}", fmt::ptr(fieldRef->parent));
            custom_types::logger.debug("token: {}", fieldRef->token);
            custom_types::logger.debug("type: {}", fmt::ptr(fieldRef->type));
            if (fieldRef->type) {
                logType(fieldRef->type, "Field Type");
            }
            custom_types::logger.debug("0 ======================END FIELD: {}======================", i);
        }
    }
    custom_types::logger.debug("0 ======================END FIELDS======================");
}

void logAsm(const Il2CppAssembly* assem) {
    if (!assem) {
        custom_types::logger.debug("NULL ASSEMBLY!");
        return;
    }
    custom_types::logger.debug("0 ======================ASSEMBLY INFO FOR: {}======================", fmt::ptr(assem));
    custom_types::logger.debug("image: {}", fmt::ptr(assem->image));
    custom_types::logger.debug("token: {}", assem->token);
    custom_types::logger.debug("referencedAssemblyStart: {}", assem->referencedAssemblyStart);
    custom_types::logger.debug("referencedAssemblyCount: {}", assem->referencedAssemblyCount);
    logAname(&assem->aname, "aname");
    custom_types::logger.debug("0 ======================END ASSEMBLY INFO======================");
}

void logCodegen(const Il2CppCodeGenModule* m, std::string_view s) {
    if (!m) {
        custom_types::logger.debug("{}: NULL MODULE!", s.data());
        return;
    }
    custom_types::logger.debug("0 ======================CODEGEN INFO FOR: {}, {}======================", fmt::ptr(m), s.data());
    custom_types::logger.debug("moduleName: {}", m->moduleName ? m->moduleName : "NULL");
    custom_types::logger.debug("methodPointerCount: {}", m->methodPointerCount);
    custom_types::logger.debug("methodPointers: {}", fmt::ptr(m->methodPointers));
    custom_types::logger.debug("invokerIndices: {}", fmt::ptr(m->invokerIndices));
    custom_types::logger.debug("reversePInvokeWrapperCount: {}", m->reversePInvokeWrapperCount);
    custom_types::logger.debug("reversePInvokeWrapperIndices: {}", fmt::ptr(m->reversePInvokeWrapperIndices));
    custom_types::logger.debug("rgctxRangesCount: {}", m->rgctxRangesCount);
    custom_types::logger.debug("rgctxRanges: {}", fmt::ptr(m->rgctxRanges));
    custom_types::logger.debug("rgctxsCount: {}", m->rgctxsCount);
    custom_types::logger.debug("rgctxs: {}", fmt::ptr(m->rgctxs));
    custom_types::logger.debug("debuggerMetadata: {}", fmt::ptr(m->debuggerMetadata));
    custom_types::logger.debug("0 ======================END CODEGEN INFO======================");
}

void logImage(const Il2CppImage* img) {
    if (!img) {
        custom_types::logger.debug("NULL IMAGE!");
        return;
    }
    custom_types::logger.debug("0 ======================IMAGE INFO FOR: {}======================", fmt::ptr(img));
    custom_types::logger.debug("name: {}", img->name ? img->name : "NULL");
    custom_types::logger.debug("nameNoExt: {}", img->nameNoExt ? img->nameNoExt : "NULL");
    logAsm(img->assembly);

    // as seen in GlobalMetadata::GetImageMetadata
    auto metadata = reinterpret_cast<const Il2CppImageGlobalMetadata*>(img->metadataHandle);
    custom_types::logger.debug("typeStart: {}", metadata->typeStart);
    custom_types::logger.debug("typeCount: {}", img->typeCount);
    custom_types::logger.debug("exportedTypeStart: {}", metadata->exportedTypeStart);
    custom_types::logger.debug("exportedTypeCount: {}", img->exportedTypeCount);
    custom_types::logger.debug("customAttributeStart: {}", metadata->customAttributeStart);
    custom_types::logger.debug("customAttributeCount: {}", img->customAttributeCount);
    custom_types::logger.debug("entryPointIndex: {}", metadata->entryPointIndex);
    custom_types::logger.debug("nameToClassHashTable: {}", fmt::ptr(img->nameToClassHashTable));
    logCodegen(img->codeGenModule, "codeGenModule");
    custom_types::logger.debug("token: {}", img->token);
    custom_types::logger.debug("dynamic: {}", img->dynamic);
    custom_types::logger.debug("0 ======================END IMAGE INFO======================");
}

void logType(const Il2CppType* t, std::string_view s) {
    custom_types::logger.debug("0 ======================TYPE INFO FOR: {}======================", s.data());
    custom_types::logger.debug("klassIndex: {}", t->data.__klassIndex);
    custom_types::logger.debug("attrs: 0x{:x}", t->attrs);
    custom_types::logger.debug("type: 0x{:x}", (int)t->type);
    custom_types::logger.debug("num_mods: {}", t->num_mods);
    custom_types::logger.debug("byref: {}", t->byref);
    custom_types::logger.debug("pinned: {}", t->pinned);
    custom_types::logger.debug("0 ======================END TYPE INFO======================");
}

void logVtable(const VirtualInvokeData* invokeData) {
    if (!invokeData) {
        custom_types::logger.debug("NULL VirtualInvokeData!");
        return;
    }
    custom_types::logger.debug("0 ======================Virtual Invoke Data FOR: {}======================", fmt::ptr(invokeData));
    custom_types::logger.debug("method info: {}", fmt::ptr(invokeData->method));
    // custom_types::logger.debug("method info class: {}", fmt::ptr(invokeData->method ? invokeData->method->klass : nullptr));
    logMethod(invokeData->method);
    custom_types::logger.debug("method pointer: {}", fmt::ptr(invokeData->methodPtr));
    custom_types::logger.debug("0 ======================END VIRTUAL INVOKE DATA======================");
}

void logInterfaceOffset(const Il2CppRuntimeInterfaceOffsetPair* pair) {
    if (!pair) {
        custom_types::logger.debug("NULL Il2CppRuntimeInterfaceOffsetPair!");
        return;
    }
    custom_types::logger.debug("0 ======================Interface Offset Pair FOR: {}======================", fmt::ptr(pair));
    custom_types::logger.debug("Klass: {}", fmt::ptr(pair->interfaceType));
    custom_types::logger.debug("klass ID: {}::{}", pair->interfaceType->namespaze, pair->interfaceType->name);
    custom_types::logger.debug("Offset: {}", pair->offset);
    custom_types::logger.debug("0 ======================END INTERFACE OFFSET======================");
}

void logParam(const Il2CppType* t, int index) {
    if (!t) {
        custom_types::logger.debug("NULL PARAMETER INFO!");
        return;
    }
    custom_types::logger.debug("0 ======================TYPE INFO FOR PARAM {}:======================", index);

    custom_types::logger.debug("klassIndex: {}", t->data.__klassIndex);
    custom_types::logger.debug("attrs: 0x{:x}", t->attrs);
    custom_types::logger.debug("type: 0x{:x}", (int)t->type);
    custom_types::logger.debug("num_mods: {}", t->num_mods);
    custom_types::logger.debug("byref: {}", t->byref);
    custom_types::logger.debug("pinned: {}", t->pinned);
    custom_types::logger.debug("0 ======================END PARAM TYPE INFO======================");
}

void logMethod(const MethodInfo* info) {
    if (!info) {
        custom_types::logger.debug("NULL METHODINFO!");
        return;
    }
    custom_types::logger.debug("0 ======================METHOD INFO FOR: {}======================", fmt::ptr(info));
    custom_types::logger.debug("methodPointer: {}", fmt::ptr(info->methodPointer));
    custom_types::logger.debug("invoker_method: {}", fmt::ptr(info->invoker_method));
    custom_types::logger.debug("name: {}", info->name ? info->name : "NULL");
    custom_types::logger.debug("klass: {}", fmt::ptr(info->klass));
    custom_types::logger.debug("klass ID: {}::{}", info->klass->namespaze, info->klass->name);
    custom_types::logger.debug("return_type: {}", fmt::ptr(info->return_type));
    if (info->parameters) {
        for (int i = 0; i < info->parameters_count; i++) {
            logParam(info->parameters[i], i);
        }
    } else {
        custom_types::logger.debug("parameters: 0x0");
    }

    // custom_types::logger.debug("parameters: {}", fmt::ptr(info->parameters));
    custom_types::logger.debug("methodMetadataHandle: {}", fmt::ptr(info->methodMetadataHandle));
    custom_types::logger.debug("genericContainerHandle: {}", fmt::ptr(info->genericContainerHandle));
    custom_types::logger.debug("token: {}", info->token);
    custom_types::logger.debug("flags: 0x{:x}", info->flags);
    custom_types::logger.debug("iflags: 0x{:x}", info->iflags);
    custom_types::logger.debug("slot: {}", info->slot);
    custom_types::logger.debug("parameters_count: {}", info->parameters_count);
    custom_types::logger.debug("is_generic: {}", info->is_generic);
    custom_types::logger.debug("is_inflated: {}", info->is_inflated);
    custom_types::logger.debug("wrapper_type: {}", info->wrapper_type);
    custom_types::logger.debug("0 ======================END METHOD INFO======================");
}

// Log every field on an Il2CppClass*, including the internal Il2CppType structs
void logAll(const Il2CppClass* klass) {
    custom_types::logger.debug("0 ======================CLASS INFO FOR CLASS: {}::{}======================", klass->namespaze, klass->name);
    logImage(klass->image);
    custom_types::logger.debug("gc_desc: {}", fmt::ptr(klass->gc_desc));
    custom_types::logger.debug("name: {}", klass->name);
    custom_types::logger.debug("namespaze: {}", klass->namespaze);
    logType(&klass->byval_arg, "byval_arg");
    logType(&klass->this_arg, "this_arg");
    custom_types::logger.debug("element_class: {}", fmt::ptr(klass->element_class));
    custom_types::logger.debug("castClass: {}", fmt::ptr(klass->castClass));
    custom_types::logger.debug("declaringType: {}", fmt::ptr(klass->declaringType));
    custom_types::logger.debug("parent: {}", fmt::ptr(klass->parent));
    custom_types::logger.debug("generic_class: {}", fmt::ptr(klass->generic_class));
    custom_types::logger.debug("typeMetadataHandle: {}", fmt::ptr(klass->typeMetadataHandle));
    custom_types::logger.debug("interopData: {}", fmt::ptr(klass->interopData));
    custom_types::logger.debug("klass: {}", fmt::ptr(klass->klass));
    logFields(klass);
    custom_types::logger.debug("events: {}", fmt::ptr(klass->events));
    custom_types::logger.debug("properties: {}", fmt::ptr(klass->properties));
    custom_types::logger.debug("methods: {}", fmt::ptr(klass->methods));
    if (klass->methods) {
        for (int i = 0; i < klass->method_count; ++i) {
            logMethod(klass->methods[i]);
            usleep(5000);  // 0.005s
        }
    }
    custom_types::logger.debug("nestedTypes: {}", fmt::ptr(klass->nestedTypes));
    custom_types::logger.debug("implementedInterfaces: {}", fmt::ptr(klass->implementedInterfaces));
    custom_types::logger.debug("interfaceOffsets: {}", fmt::ptr(klass->interfaceOffsets));
    custom_types::logger.debug("static_fields: {}", fmt::ptr(klass->static_fields));
    custom_types::logger.debug("rgctx_data: {}", fmt::ptr(klass->rgctx_data));
    custom_types::logger.debug("typeHierarchy: {}", fmt::ptr(klass->typeHierarchy));
    custom_types::logger.debug("unity_user_data: {}", fmt::ptr(klass->unity_user_data));
    custom_types::logger.debug("initializationExceptionGCHandle: {}", klass->initializationExceptionGCHandle);
    custom_types::logger.debug("cctor_started: {}", klass->cctor_started);
    custom_types::logger.debug("cctor_finished_or_no_cctor: {}", klass->cctor_finished_or_no_cctor);
    custom_types::logger.debug("cctor_thread: {}", klass->cctor_thread);
    custom_types::logger.debug("genericContainerHandle: {}", fmt::ptr(klass->genericContainerHandle));
    custom_types::logger.debug("instance_size: {}", klass->instance_size);
    custom_types::logger.debug("actualSize: {}", klass->actualSize);
    custom_types::logger.debug("element_size: {}", klass->element_size);
    custom_types::logger.debug("native_size: {}", klass->native_size);
    custom_types::logger.debug("static_fields_size: {}", klass->static_fields_size);
    custom_types::logger.debug("thread_static_fields_size: {}", klass->thread_static_fields_size);
    custom_types::logger.debug("thread_static_fields_offset: {}", klass->thread_static_fields_offset);
    custom_types::logger.debug("flags: 0x{:x}", klass->flags);
    custom_types::logger.debug("token: {}", klass->token);
    custom_types::logger.debug("method_count: {}", klass->method_count);
    custom_types::logger.debug("property_count: {}", klass->property_count);
    custom_types::logger.debug("event_count: {}", klass->event_count);
    custom_types::logger.debug("nested_type_count: {}", klass->nested_type_count);
    custom_types::logger.debug("vtable_count: {}", klass->vtable_count);
    custom_types::logger.debug("interfaces_count: {}", klass->interfaces_count);
    custom_types::logger.debug("interface_offsets_count: {}", klass->interface_offsets_count);
    custom_types::logger.debug("typeHierarchyDepth: {}", klass->typeHierarchyDepth);
    custom_types::logger.debug("genericRecursionDepth: {}", klass->genericRecursionDepth);
    custom_types::logger.debug("rank: {}", klass->rank);
    custom_types::logger.debug("minimumAlignment: {}", klass->minimumAlignment);
    custom_types::logger.debug("packingSize: {}", klass->packingSize);
    custom_types::logger.debug("initialized_and_no_error: {}", klass->initialized_and_no_error);
    custom_types::logger.debug("nullabletype: {}", klass->nullabletype);
    custom_types::logger.debug("initialized: {}", klass->initialized);
    custom_types::logger.debug("enumtype: {}", klass->enumtype);
    custom_types::logger.debug("is_generic: {}", klass->is_generic);
    custom_types::logger.debug("has_references: {}", klass->has_references);
    custom_types::logger.debug("init_pending: {}", klass->init_pending);
    custom_types::logger.debug("size_init_pending: {}", klass->size_init_pending);
    custom_types::logger.debug("size_inited: {}", klass->size_inited);
    custom_types::logger.debug("has_finalize: {}", klass->has_finalize);
    custom_types::logger.debug("has_cctor: {}", klass->has_cctor);
    custom_types::logger.debug("is_blittable: {}", klass->is_blittable);
    custom_types::logger.debug("is_import_or_windows_runtime: {}", klass->is_import_or_windows_runtime);
    custom_types::logger.debug("is_vtable_initialized: {}", klass->is_vtable_initialized);
    custom_types::logger.debug("is_byref_like: {}", klass->is_byref_like);
    for (uint16_t i = 0; i < klass->interface_offsets_count; i++) {
        logInterfaceOffset(&klass->interfaceOffsets[i]);
    }
    for (uint16_t i = 0; i < klass->vtable_count; i++) {
        logVtable(&klass->vtable[i]);
    }
    custom_types::logger.debug("0 ======================END CLASS INFO======================");
}
}  // namespace custom_types
