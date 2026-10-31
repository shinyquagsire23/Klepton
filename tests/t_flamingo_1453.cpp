// Exercise the real 1.2.1 priority registry with a data-only platform boundary.
// This host fixture does not load the game or claim device validation.
#include "installer.hpp"
#include <cassert>
#include <cstring>
#include <mutex>
#include <string>

namespace {
using Function = int (*)(int);
std::recursive_mutex transaction;
std::string calls;
int original(int value) { calls += 'O'; return value + 1; }
int unavailable(int value) { return value; }
Function current = original;
Function a_orig, b_orig, c_orig, d_orig;
unsigned exchanges;
int a(int value) { calls += 'A'; return a_orig(value) + 10; }
int b(int value) { calls += 'B'; return b_orig(value) + 100; }
int c(int value) { calls += 'C'; return c_orig(value) + 1000; }
int d(int value) { calls += 'D'; return d_orig(value) + 10000; }
int invoke(std::string expected) {
    calls.clear();
    int result = current(0);
    assert(calls == expected);
    return result;
}
flamingo::HookNameFilter filter(char const* name) {
    return flamingo::HookNameMetadata{.name = name};
}
flamingo::HookInfo hook(char const* name, Function function, Function* orig,
                        flamingo::HookPriority priority = {}) {
    return {function, reinterpret_cast<void*>(original), orig, 5,
            flamingo::CallingConvention::Cdecl, flamingo::HookNameMetadata{.name = name},
            std::move(priority), {true, false, false}};
}
}

extern "C" void sc2_aot_begin() { transaction.lock(); }
extern "C" void sc2_aot_end() { transaction.unlock(); }
extern "C" void* sc2_aot_current(void const* target) {
    return target == reinterpret_cast<void*>(original) ? reinterpret_cast<void*>(current) : nullptr;
}
extern "C" void* sc2_aot_original(void const* target) {
    return target == reinterpret_cast<void*>(original) ? reinterpret_cast<void*>(original) : nullptr;
}
extern "C" int sc2_aot_exchange(void* target, void* replacement, void** previous) {
    if (!sc2_aot_current(target)) return -1;
    if (previous) *previous = reinterpret_cast<void*>(current);
    current = reinterpret_cast<Function>(replacement);
    ++exchanges;
    return 0;
}
extern "C" int sc2_aot_entry_word(void const* target, uint32_t* word) {
    if (!sc2_aot_current(target)) return -1;
    *word = 0xd503201f; // The platform owns original code inspection in production.
    return 0;
}

int main() {
    setbuf(stdout, nullptr);
    using namespace flamingo;
    assert(invoke("O") == 1);
    auto first = Install(hook("A", a, &a_orig, {.befores = {filter("B")}}));
    assert(first.has_value() && invoke("AO") == 11);
    auto second = Install(hook("B", b, &b_orig, {.afters = {filter("A")}}));
    assert(second.has_value() && invoke("ABO") == 111);

    unsigned before = exchanges;
    auto cycle = Install(hook("C", c, &c_orig,
                              {.befores = {filter("A")}, .afters = {filter("B")}}));
    assert(!cycle.has_value());
    assert(std::holds_alternative<installation::TargetBadPriorities>(cycle.error()));
    assert(exchanges == before && !c_orig && invoke("ABO") == 111);

    auto final = Install(hook("D", d, &d_orig, {.is_final = true}));
    assert(final.has_value() && invoke("ABDO") == 10111);
    auto conflict = Install(hook("C", c, &c_orig, {.is_final = true}));
    assert(!conflict.has_value() && invoke("ABDO") == 10111);

    // Handles returned before a rejected cycle must still identify live nodes.
    assert(Uninstall(first.value().returned_handle).has_value());
    assert(invoke("BDO") == 10101);
    assert(Uninstall(second.value().returned_handle).has_value());
    assert(invoke("DO") == 10001);
    assert(Uninstall(final.value().returned_handle).has_value());
    assert(invoke("O") == 1);

    auto missing = Install(HookInfo(a, reinterpret_cast<void*>(unavailable), &a_orig));
    assert(!missing.has_value() && std::holds_alternative<installation::TargetTooSmall>(missing.error()));
    auto null = Install(HookInfo(a, nullptr, &a_orig));
    assert(!null.has_value() && std::holds_alternative<installation::TargetIsNull>(null.error()));
    assert(invoke("O") == 1);
    puts("PASS real Flamingo 1.2.1 order, cycle rollback, stable handles, final priority and missing-target rejection");
}
