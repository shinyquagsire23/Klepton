#include "delegate.hpp"

namespace custom_types {

int get_delegate_count() {
    static int counter = 0;
    return counter++;
}

void log_delegate(Il2CppDelegate* d) {
    auto& logger = custom_types::logger;
    logger.debug("LOGGING DELEGATE: {}", fmt::ptr(d));
    if (d) {
        logger.debug("data: {}", fmt::ptr(d->data));
        logger.debug("delegate_trampoline: {}", fmt::ptr(d->delegate_trampoline));
        logger.debug("extraArg: {}", d->extraArg);
        logger.debug("invoke_impl: {}", fmt::ptr(d->invoke_impl));
        logger.debug("method: {}", fmt::ptr(d->method));
        // logger.debug("method_code: {}", fmt::ptr(d->));
        logger.debug("method_info: {}", fmt::ptr(d->method_info));
        logger.debug("method_is_virtual: {}", (bool)d->method_is_virtual);
        logger.debug("method_ptr: {}", fmt::ptr(d->method_ptr));
        logger.debug("klass ptr: {}", fmt::ptr(d->object.klass));
        logger.debug("original_method_info: {}", fmt::ptr(d->original_method_info));
        logger.debug("target: {}", fmt::ptr(d->target));
    }
}

#ifdef TEST_DELEGATE

static int capture;

void main() {
    int cap = 3;
    std::function<void (Il2CppObject*, int, Il2CppObject*)> f([cap](Il2CppObject*, int, Il2CppObject*) {
        capture += cap;
    });
    // Static
    custom_types::MakeDelegate<Il2CppObject*>(f);
    auto ptr = DelegateWrapperStatic<void, Il2CppObject*, int, Il2CppObject*>::___TypeRegistration::klass_ptr;

    // Instance
    Il2CppObject inst{};
    custom_types::MakeDelegate<Il2CppObject*>(&inst, f);
    auto ptr2 = DelegateWrapperInstance<void, Il2CppObject*, int, Il2CppObject*>::___TypeRegistration::klass_ptr;
}

#endif

}
