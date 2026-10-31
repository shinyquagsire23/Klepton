# Beat Saber 1.45.3 source port

This experimental profile targets the owned 1.45.3_30054 game (version code 3237, Unity 6.3 and metadata version 39). It lives in its own source folder and retains the existing Scotland2, SongCore, SongDownloader, BetterSongSearch and Airbuds implementations. Fake ID is excluded.

Pinned public source and QMOD inputs are in `inputs.json`; game inputs remain local. `prepare_1453_sources.py` reconstructs the sources and SDK from hash-verified archives and the patches in this folder. It accepts the existing independently bootstrapped dependency sources explicitly. `prepare_1453_ndk.py` verifies the NDK29 archive and extracts its required ARM64 compiler subset.

`build_1453_support.py`, `build_1453_flamingo.py`, `build_1453_network.py` and `build_1453_module.py` compile the original components. These tools do not install or package a game. The module build preserves compiler-generated game hook addresses for separate verification. Manual Unity hooks are measured against exact ELF hashes and instruction words.

The original component sources compile. Host fixtures cover Flamingo priority rollback, fmt compile-time checking, atomic save recovery, Apple glyph rasterization, Airbuds settings persistence, Android frame callbacks and memory reads, and SongDownloader response validation. The static API audit checks 3,322 selected strong symbols and all 32 published BeatSaverPlusPlus API symbols, with supporting RTTI supplied by the module/shared runtime. Source reconstruction currently covers 23 trees and 49,109 files.

Actual native checks load 23 signed guest libraries with strict imports and constructors; initialize the game's IL2CPP runtime; register the original mod types; verify all five custom-type hooks and 40 Lapiz/BSML early hooks; and exercise the original download clients. Three clients accept trusted local HTTPS and reject untrusted certificates/wrong hostnames. Both production archive paths reject 13 malicious/corrupt ZIP cases while retaining an existing download. The scene-hook test supplies the independently measured Unity icall registration on the host: production Unity registration and scene execution are separate checks.

The runtime and Swift launcher compile for visionOS. The 1.45.3 launcher profile selects Flamingo 1.2 and the source-adapted filesystem/archive boundaries. `selected-mods.json` declares the exact composition, including the additional MetaCoreBS dependency. Release ELFs retain dynamic public APIs while private full-symbol files stay in the local build workspace. `check_1453_reproducibility.py` repeated compilation from independently reconstructed sources: all 25 actual library outputs matched byte for byte. The proof covers this Mac and toolchain, rather than a second machine.

`stage_1453_bundle.py` requires completed artifact, reproducibility and native receipts before staging unsigned frameworks. `stage_1453_game.py` prepares the exact owned game images offline. `assemble_1453_app.py` built and signed the release candidate, verified its signature, exact metadata inventory, game hashes, release settings, icon and start/settings screen configuration. The AVP adapter also completed its build-only path. No 1.45.3 Unity startup, gameplay, headset or authenticated Airbuds test is complete. The working 1.40.8 profile, normal game, songs/saves and rollback remain separate.

Only the existing custom test application ID is authorized for the final candidate. Private game assets, saves, logs and signing material must stay outside public source packages.


## Using the experimental AVP Play profile

Use the matching `port/beat-saber-1.45.3` branch of the AVP Play fork. Import your matching APK in its Version menu, retain its matching OBB/add-on recipe, and enable core mods. All three game fingerprints must match before the switch is eligible. The 1.40.8 profile remains supported separately. The experimental 1.45.3 installer updates only the existing Scotland2 Test identity; it does not allocate another application ID.

The source profile contains the builder and public graphics dependencies. It does **not** contain a precompiled game or a self-contained mod build cache. The modern adapter requires the independently prepared, validated workspace described below. Configure it in a local `.scotland2-avp-local.json`, or in `core-mods-build-inputs.json` under `AVPPLAY_HOME` (normally `~/Library/Application Support/AVPPlay`):

```json
{
  "dependencyWorkspace": "/absolute/path/to/verified-legacy-dependencies",
  "cache": "/absolute/path/to/download-cache",
  "port1453": {"workspace": "/absolute/path/to/verified-1453-workspace"}
}
```

The legacy dependency workspace has its normal `port-deps/sources` layout. The modern workspace holds `build/libs`, the completed `build/artifact-audit.json`, completed native checks, and the independently reconstructed `repro-final/build/reproducibility.json`. Optional explicit `reproducibility`, `importCheck`, `earlyCheck` and `downloadCheck` paths belong inside `port1453`. Paths and receipts stay private. The adapter rejects changed outputs or stale source manifests. Do not share another person's receipt as proof for a different build.

For source reconstruction, run `prepare_1453_sources.py --workspace NEW_WORKSPACE --cache CACHE --legacy-dependencies LEGACY_SOURCES`; `--seed DIRECTORY` and `--offline` support verified local inputs. Prepare NDK29 with `prepare_1453_ndk.py`, then build support, Flamingo and the component stack with the tools above. Their `--help` describes explicit inputs. Run the artifact, game, import, registration, download and hook checks; repeat reconstruction in a second workspace before `check_1453_reproducibility.py --workspace SECOND_WORKSPACE --reference FIRST_WORKSPACE --legacy-dependencies LEGACY_SOURCES`. Retain the resulting receipts and logs. Assembly and signing are later steps, using only your own game/signing inputs.

`avp_play.py --no-install` runs the real AVP build route without contacting the headset. The ordinary installation route also stages startup metadata before the matching recipe stages its OBB/add-ons. Preserve the existing data container and use an update rather than uninstalling.

To package committed source and verified public graphics reproducibly, run `package_avp_source_profile.py --output /path/to/profile.tar.gz`. It creates the matching `.json` sidecar and a source hash manifest, excludes inactive Fake ID source, and refuses a dirty tracked tree. Package the Mac app with `AVPPLAY_COREMODS_ARCHIVE` pointing to that archive. The release is experimental until production Unity startup and gameplay have actually been checked; see [validation](VALIDATION.md).
