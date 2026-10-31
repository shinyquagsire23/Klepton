# Experimental 1.45.3 validation and audit record

Recorded 2026-10-27. This record covers the isolated Unity 6.3 source port and AVP integration, not production headset operation.

## Completed checks

- Exact owned game library/metadata hashes and version code 3237 checked before staging. Source pins, 16 patches and 23 reconstructed source trees (49,109 files) matched the actual build inputs.
- All original component sources compiled. A second independent source workspace reproduced all 25 library outputs byte for byte, including shared support/link reference outputs. Release private symbol tables were stripped; dynamic public APIs remain.
- Artifact audit preserved 3,322 original strong namespace APIs plus 32 actual published BeatSaverPlusPlus APIs; supporting RTTI was accounted for. No Fake ID, test exports or private symbol tables in selected shipping ELFs.
- Native strict import/constructor checks loaded 23 actual signed modules. Registration checked the game's 188 original assemblies, 183 original mod types, object allocation/initialization and all five custom-type hooks. Early callbacks exercised 26 Lapiz and 14 BSML hooks and registered 106 original early types. Diagnostic fixture types stay outside the shipping selection.
- All three production HTTP clients accepted trusted local HTTPS and rejected untrusted certificates and wrong hostnames (nine requests). Both actual production extraction paths rejected 13 corrupt/malicious archive cases and preserved an existing download.
- Host fixtures passed for Flamingo hook ordering/rollback, fmt compile-time checks, atomic save recovery, Unicode rasterization, Airbuds private settings persistence, Android frame scheduling/memory reads and SongDownloader response parsing (ASan/UBSan).
- Device runtime and Swift launcher compiled. The local release app built, signed and passed deep/strict signature verification, selected-module and release-control checks. The owner's icon/start artwork stayed local. AVP's real build-only adapter route completed without device calls.
- AVP full suite: 333 tests, five skipped, zero failures. Exact APK fingerprints, version-aware builder selection and legacy metadata installation boundaries are covered.

## Change review and resolved findings

The source manifest and patches keep original component implementations and isolate platform changes. Reviewed boundaries include source/download reconstruction, Android ABI and imports, signed hook preparation and ownership, hook order and lifecycle, networking/extraction, saves, Unicode, authentication/settings storage, launcher metadata and framework composition.

| Finding / trigger | Resolution | Verification |
| --- | --- | --- |
| Cross-translation-unit static initialization and path-hashed link order could construct callback consumers first | Stable source link order and explicit SongDetails callback/singleton initialization priorities | Actual constructors/early callbacks and independent binary rebuild |
| Empty/malformed SongDownloader responses could advance an end iterator or reuse a stale document | Validate parsed objects and response members in the original client | Actual parser fixture under ASan/UBSan |
| Hidden BeatSaverPlusPlus extraction function could not use the older exported-symbol hook | Build original source; adapt its extraction body to the bounded adapter, preserve models/network/API | Actual AcceptData path and malicious ZIP fixtures |
| Transitive WebUtils headers emitted an undeclared BSML dependency | Explicit WEBUTILS_NO_BSML for BeatSaver source | Phase-aware native imports |
| Flamingo must preserve priorities while using prepared signed gates | Transactional pointer ownership, stable rejected handles and cycle checks before graph mutation | Actual graph/order/rollback fixture |
| TLS, archive paths and temporary files cross trust boundaries | Verified CA/peer/host, HTTPS redirects, extraction bounds, exclusive temporary creation | Actual clients and both archive paths |
| Save restoration/config writes could replace newer, malformed or symlinked data | Atomic private writes, preserve newer/live files and symlinks, bounded schema parsing | Isolated save and Airbuds configuration fixtures |
| New Unity imports and duplicate APK/mod C++ runtimes | Per-ALooper frame callbacks, fault-safe self reads; embed one selected C++ runtime | Android shim fixture and actual device app build |
| Imported game metadata could select an old OBB after Start | Validate and stage version 3237 metadata using the existing repair boundary | AVP adapter routing/install-boundary regression checks |

Native runtime/launcher changes are version selected. The normal installation and the tested 1.40.8 rollback remain separate. Original sources, full-symbol diagnostic images and all build receipts remain in private local inputs; inactive Fake ID source is excluded from shareable packaging.

## Limits

No production Unity startup, scene callback, late UI, full song, device save persistence or authenticated Airbuds request has been run for 1.45.3. The scene hook fixture supplies the measured Unity icall registration on the host; it does not prove Unity's production registration sequence. SDK hook fixtures were compile checked, not all executed. Graphics inputs were hash verified from a preexisting public cache. Reproducibility was demonstrated in two source workspaces on this Mac, not on a fresh second Mac. The Mac app is locally ad hoc signed, not notarized for public distribution. No game, paid artwork, saves, device logs or signing material belongs in a public release.
