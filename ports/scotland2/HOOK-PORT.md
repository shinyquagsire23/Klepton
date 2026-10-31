# Prepared hook backend

Hooks are prepared in unsigned translated frameworks before Apple code signing.
An exact manifest binds the original ARM64 ELF, translation and each entry word.
Supported entry instructions are relocated into fixed signed trampolines; all
other instructions fail explicitly. No JIT or runtime executable-page write is
required or enabled.

`tools/prepare_hooks.py` emits read/execute gates, signed records, and separate
read/write dispatch slots. `runtime/guest/kl_aot_hooks.c` validates the layout,
permissions and instruction forms before registering them. Dispatch publication
uses acquire/release ordering. Hook installation publishes the caller's original
pointer before making a replacement reachable. Chaining, restoration, native
abort gates, unsupported targets and malformed records have isolated fixtures.

The original And64 and Flamingo APIs are adapted to this shared backend. Their
Android C++ state remains in guest modules. Original-entry depth is computed from
compiler-emitted catalogs, including shared target chains and the explicit
SongCore generic target. Scene and MetaCore native Unity targets remain bound to
exact measured profiles for the supported game.

Two narrow support-library boundaries use the same mechanism: beatsaber-hook's
shell-based directory helper and the released BeatSaverPlusPlus archive extractor.
The latter retains the entire released module/export surface while routing only
archive writes through the bounded staging adapter used by SongDownloader.

The host's guest images have process lifetime. General hot unloading, midpoint
hooks, caller-provided executable trampolines, arbitrary new QMODs and untested
versions are unsupported. See the audit and validation reports for actual evidence
and limits; successful compilation alone is not runtime validation.
