# Building the custom-song application

The supported game is Beat Saber **1.40.8_7379**. Both native library SHA-256
values must match `ports/scotland2/selected-mods.json`; the builder checks them
before compilation. Use an Apple-silicon Mac with Xcode and the visionOS SDK.
The current validation environment and exact limitations are recorded in
`docs/cleanup/VALIDATION-REPORT.md`.

## Inputs

- Your matching extracted game, containing `lib/arm64-v8a` and
  `assets/bin/Data/Managed`, plus your matching original APK.
- A public graphics input cache matching `graphics-inputs.json`, with `vendor/out`
  and `vendor-moltenvk/out` under its root. The underlying Klepton graphics source
  recipes remain `make angle-xros angle-xrsim` and `make mvk`; their source pins
  are in the Makefile and `tools/mvk_fetch.sh`. The validation report states
  whether those graphics were rebuilt or independently verified as cache inputs.
- Your Apple development team and chosen bundle ID. Reuse your installed app's
  ID for updates. The builder never needs a new ID for release versus diagnostic
  mode and never installs an additional app.
- Separate output and content-addressed cache directories. Paths may contain
  spaces. The output must be separate from the source checkout.

## One build entry point

```sh
python3 ports/scotland2/tools/build_release.py \
  --game "/path/to/your/extracted-game" \
  --apk "/path/to/your/original.apk" \
  --graphics "/path/to/verified-graphics-inputs" \
  --output "/path/to/new-private-build" \
  --cache "/path/to/download-cache" \
  --team YOURTEAMID \
  --bundle-id your.existing.application.id
```

`--preflight` checks inputs without building. Downloads use pinned SHA-256
values, HTTPS transport, temporary files and a resumable content-addressed cache.
`--seed DIRECTORY` can supply original archives without downloading; their hashes
are still checked. `--offline` fails clearly when a required verified input is
missing. An independently bootstrapped dependency workspace can be supplied with
`--dependency-workspace DIRECTORY`; extracted headers and the NDK tree are checked.

`--resume` resumes only a builder-owned output with the same identity and input
settings. It records a new explicit source snapshot. A final reproducibility
check must still use an empty output, not only a resumed build.

The final app is under
`visionos/build/release-dd/Build/Products/Release-xros/Klepton.app` in the private
output. Structural validation and deep signature verification run after the
build. `build-validation.json`, source/input receipts and logs belong with that
private output. Do not include them in a public source release.

`--unsigned` permits recipient-identity generation and packaging checks without
registering an app ID. It does not prove signing or device operation.

## Install, preserve data, and roll back

Install the signed app as an **update to the same bundle ID**, using Xcode or
`xcrun devicectl device install app`. Never uninstall or clear the data container
as a build or cleanup step. Existing songs, settings and saves stay in that
container; game assets are staged separately through Klepton's normal workflow.

Keep a hash-verified, previously tested signed app privately. If rollback is
needed, install it over the same ID. Preserve a current backup of the data first;
never roll back or replace saves automatically.

## Diagnostics and supported behavior

The default builder produces the selected ten-mod release composition. Original
Scotland2 phase ordering, shared hook backend, mod UI, networking, TMP glyph cache
and atlas upload remain in use. Required scene startup is a small guest adapter.
Observer fixtures are compiled/packaged separately in diagnostic compositions.

The historical `build-test-app.sh` now forwards to the same authoritative builder.
The older progressive `package*.sh` and host fixtures remain for diagnosis; they
are not separate release selections. Host save fixtures use isolated directories.
Never run those fixtures against a headset's live data.

Runtime modules have process lifetime; hot unloading and arbitrary mod injection
are outside the supported scope. Restart the application to reload its static,
signed composition. Do not claim compatibility with untested game or SDK versions.

## Through AVP Play (Beat Saber 1.45.3)

In AVP Play the mods are a switch on Beat Saber's page ("Mods (custom
songs)"), off by default. A build with it on passes `KLEPTON_SCOTLAND2=1`
to `visionos/run.sh`; the bundle id stays the game's usual one, so switching
is an update of the same app and its saves and songs stay.

The toolchain carries the mods prebuilt, one profile per game version
(`ports/scotland2/avp-profiles.json` → `ports/scotland2/prebuilt/<version
code>`): the guest libraries, their hook catalog and the native loader.
`tools/stage_avp_mods.py` checks the owner's game files against the profile,
translates the libraries and prepares the hook gates in the game's own
`libil2cpp`/`libunity`; nothing is compiled or downloaded on the user's Mac.
A game version without a profile builds without mods (AVP Play greys the
switch out and says so).

Making a profile is developer work: build the mod workspace with the official
NDK r29 (`prepare_1453_ndk.py`, `link_1453_workspace.py`,
`build_1453_support.py`, `build_1453_flamingo.py`, `build_1453_stack.py`),
then `tools/pack_avp_mods.py --workspace … --reference …`, which refuses a
library that differs from the recorded reference output.
