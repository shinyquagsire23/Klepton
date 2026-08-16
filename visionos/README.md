# The visionOS host app

The Klepton app itself: Swift for the platform layer (P4).
`spikes/device-probe` stays what it is: a small, disposable thing
for answering device questions. This is the app those answers were for.

```bash
./run.sh              # the booted visionOS Simulator — rung 2, tests dyld
./run.sh device       # a physical Vision Pro         — rung 3, tests AMFI

KLEPTON_TARGET=superhot ./run.sh device      # ...for another guest
../build_run_vpro.sh superhot                # the same, wrapped: builds, runs,
                                             # and reads the log back
```

Both do the same five steps: build the runtime (`make xros`), translate the
guest libraries (`mkguest.sh`), regenerate the project, install, stage
assets, launch. `KL_SKIP_STAGE=1` skips the upload when the assets are already
there, which on device is the difference between a 20-second loop and a
20-minute one.

## Which guest — one table, five consumers

`targets.py` is the authority: tree, APK, asset directory, entry library, boot
sequence, bundle id, display name and **product name**. `run.sh`, `mkguest.sh`,
`stage_assets.sh` and `gen_xcodeproj.py` all read it, and `make targets`
generates `runtime/kl_target_table.h` from it so the C runtime — the app's
`Sources/kl_app.c` and the host's `build/m_boot` — reads the same row.

That last one is not tidiness. Two drivers describing one guest differently is a
class of bug with no error surface at all: the build succeeds, the app installs,
and the guest is told the wrong thing about itself. The failure is silent in
both directions — one title's APK opened as another's zip, or a title writing
its saves into another's directory.

**A bundle id separates the installed apps and nothing else about a build**, so
the target also carries the PRODUCT name, and with it the `.xcodeproj`, the
`.app`, the derived-data directory and the `Frameworks/<target>/` the
translations are staged into. ANGLE is deliberately not per-target: it is the
same renderer for every guest and `mkangle.sh` writes it once, into
`Frameworks/` itself.

The id itself is **derived, not stored**: `$USER.dev.klepton.target.<target>`.
Two halves, for two different collisions. The `<target>` half separates the apps
on one device. **The `$USER` half separates DEVELOPERS**, and that one is not
cosmetic: an App ID can be registered to exactly one team, automatic signing
registers it on the first build, and so whoever builds this tree first silently
takes the id away from everyone else — the next person's build fails with
`Failed Registering Bundle Identifier: … cannot be registered to your
development team`, which reads like a broken project rather than a name already
spoken for, and it takes the two memory entitlements with it because those need
an explicit App ID.

`KLEPTON_BUNDLE_SCOPE` overrides the `$USER` part (an unset or empty one leaves
the id unscoped rather than emitting a leading dot); `KLEPTON_BUNDLE_ID`
overrides the whole id. **Changing the id orphans the container** — the assets,
the OBB and the guest's saves live in the old app's Documents, and the new id is
a new app with an empty one. That is not silent: the staging stamp is keyed on
the bundle id, so the first build after a change re-stages by itself. It is
still a 2.2 GB upload, and the old app is still installed until you delete it.

`KLEPTON_TARGET` selects it at BUILD time; the chosen name is compiled into the
app as `KL_TARGET_DEFAULT`, because an app launched by hand from the Home View
has no environment at all. `KL_TARGET` overrides that at run time — but the two
apps embed *different* guest frameworks, so pointing one at the other's target
stops at `kl_app_configure` with "missing guest libraries" rather than
misbehaving.

## What lives where, and why

|  | where | why |
|---|---|---|
| C runtime | `build/Klepton.xcframework`, linked **statically** | `make xros` builds both slices. Static, so it is inside the app binary — no load-time cost, and nothing for dyld to resolve. |
| guest libraries | `Frameworks/<name>.xcframework`, **embedded, not linked** | klepton-ld output. Embedded so Xcode code-signs them; a loose Mach-O elsewhere in the bundle is only *sealed* by the outer signature, which is not the same thing and is not what AMFI accepted in P3. Not *linked*, because nothing references their symbols — the runtime `dlopen`s them by path, and linking would make dyld want an exports trie klepton-ld deliberately does not emit. |
| APK assets (2.2 GB) | the app's **Documents container**, staged once | They change only when the APK does; the code changes every few minutes. Bundling them would put a multi-minute upload in front of every build. |

The asymmetry is deliberate: **code in the bundle, data in the container.** P3
established that AMFI accepts a `klepton-ld` dylib inside a bundle we signed,
and established nothing at all about one pushed into Documents afterwards.
Assets carry no code, so they have no such constraint.

## Language boundary

Swift owns the App, and will own the ImmersiveSpace, Compositor Services,
Metal and ARKit. C owns the guest. The seam is `Sources/kl_app.h` — four
functions — imported through `Sources/Klepton-Bridging-Header.h`. There is no
Objective-C anywhere and none is needed; see below for why the
boundary lands there.

`Sources/kl_app.c` is `t_boot`'s sequence with the harness removed: no forked
DRM-guard self-test, no re-exec'd recon child (an app bundle never forks, and
the Metal-refuses-forked-children reason for it is moot once it does not), no
argv, no SDL viewer. What is left is load libmain → `JNI_OnLoad` →
`NativeLoader.load` → `UnityPlayer.initJni`.

## Reading a run

Everything the guest prints goes to `Documents/klepton-boot.log`,
**line-buffered** — an unimplemented JNI slot aborts the process by design, and
a fully-buffered stream loses the whole report when the process dies on a
signal. The UI polls the file while the run is going and offers it via
ShareLink afterwards; on the simulator it is easier to read straight off disk:

```bash
UDID=$(xcrun simctl list devices booted | grep -o '[0-9A-F-]\{36\}' | head -1)
BUNDLE=$(python3 targets.py beatsaber bundle)     # $USER.dev.klepton.target.<target>
LC_ALL=C less "$(xcrun simctl get_app_container $UDID $BUNDLE data)/Documents/klepton-boot.log"
```

`LC_ALL=C` because the log contains guest bytes — the same trap as on the host.

## Gotchas that cost time here

- **Reinstalling rotates the data container.** Assets staged before an install
  end up in an orphaned container and the app reports them missing, which reads
  like a staging bug rather than an ordering one. `run.sh` stages *after*
  installing, always.
- **`$VAR…` is not `$VAR` followed by an ellipsis.** bash folds the multi-byte
  character into the variable name and dies with `unbound variable` on a name
  you never wrote. Brace it.
- **The archives need real Make prerequisites.** With only a `.PHONY` rule
  producing `build/xros/libklepton.a`, an edited runtime source left a stale
  archive, the xcframework was built from it, and the app silently ran the
  *previous* loader — which presents as a bug in the app. Fixed in the Makefile.
