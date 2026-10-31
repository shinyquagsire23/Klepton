// This constructor would kill the test if import validation ran too late.
extern void sc2_intentionally_missing_import(void);
__attribute__((constructor)) static void must_not_run(void) { __builtin_trap(); }
__attribute__((visibility("default"))) void needs_missing_symbol(void) {
    sc2_intentionally_missing_import();
}
