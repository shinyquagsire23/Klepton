# AVP Play startup metadata repair

The core-mod AVP adapter installed the app without the metadata-only staging
pass used by `visionos/run.sh`. AVP Play stages assets separately, so
`apktool.yml` and `AndroidManifest.xml`, which live beside `assets/`, were
missing from the app's Documents/beatsaber140 directory on a fresh installation.
The native guest then defaulted to versionCode 545 and searched for a 1.28 OBB
instead of the installed 1.40.8 OBB (1716). Addressables could not load
`assets/aa/settings.json` or its `AppInit` scene, leaving a black screen after
Start.

The adapter now requires the pinned game's version metadata and manifest before
building. After same-identity installation it runs the existing
`stage_assets.sh --meta <device>` pass. Failure propagates to AVP Play rather
than reporting a completed installation. This copies only the two metadata
files; the recipe still stages the APK, OBB and assets. No launch, app-ID
registration, uninstall or save reset is added. Build-only mode performs no
device staging.

Regression: `python3 tests/t_avp_metadata.py` exercises the adapter's main path,
installation order and inherited identity, staging failure, build-only mode,
and rejection of missing/wrong metadata before any build or device command.
These are isolated installer tests, not proof of headset gameplay.
