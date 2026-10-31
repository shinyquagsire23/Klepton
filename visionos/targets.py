#!/usr/bin/env python3
"""Which guest a visionOS build is for — one table, three consumers.

`mkguest.sh`, `stage_assets.sh`, `run.sh` and `gen_xcodeproj.py` all need the
same handful of facts about a target, and until there was a second one they each
carried their own copy as Beat Saber literals. A table in four places is a table
that disagrees with itself, and the way that failure presents is the worst kind:
the build succeeds, the app installs, and the guest is described wrongly to
itself.

So this is the table, and the shells read it the same way the generator does:

    python3 targets.py <target> <field>      # one field, bare
    python3 targets.py <target>              # every field, as shell assignments
    python3 targets.py --list
    python3 targets.py --c-table             # ...and the RUNTIME's copy, generated

The last one is the fifth consumer and the reason this file grew `entry` and
`kind`: the runtime needs the same table (which tree, which APK, which library
the chain starts at, which boot sequence), and it needed it twice — once in
`visionos/Sources/kl_app.c` for the app and once in `mains/m_boot.c` for the
host driver. Two hand-written C copies of a Python table is the exact failure
this file's header warns about, so `make targets` generates
`runtime/kl_target_table.h` from here instead. It is committed, like
`kl_libc_table.h` and `kl_jni_slots.h`, so a build never depends on Python.

Two apps built from this tree must not collide, and a bundle ID separates none
of the things that would. So the target also decides
the PRODUCT name — which carries the .xcodeproj, the .app and the derived-data
directory with it — and the subdirectory the translated guest frameworks are
staged into. ANGLE is deliberately NOT per-target: it is the same renderer for
every guest and `mkangle.sh` writes it once, into Frameworks/ itself.
"""
import sys
import shlex

TARGETS = {
    "beatsaber": {
        # Unity + IL2CPP + Oculus Mobile SDK. The chain is staged: libmain
        # dlopens libunity, which dlopens the rest — but every one is embedded,
        # because kl_load_auto resolves a DT_NEEDED against Frameworks/ and does
        # not care who asked.
        #
        # DISCOVERED, not pinned (None asks the Makefile — see libs_for below).
        # It used to name five libraries, which was the whole of 1.28; 1.40
        # ships eleven translatable ones and the pinned list silently left
        # libOculusXRPlugin — the XR-SDK display provider, i.e. the entire
        # render path — out of the bundle. On device that is not a fallback but
        # a dead end: the ELF tree is deliberately not in the bundle, so a
        # library with no translation cannot load at all. Same lesson as
        # `4dc27b1`: the guest library set is a property of the APK.
        "libs":    None,
        "srcdir":  "beatsaber/lib/arm64-v8a",
        "tree":    "beatsaber",
        "apk":     "beatsaber.apk",
        "assets":  "beatsaber/assets",
        # Nothing in this guest reads a library as a FILE, so no ELF goes into
        # the container at all. See steamlink-vr's `qtplugins`.
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        # The library the chain STARTS at — libmain's JNI_OnLoad is the whole
        # entry point for a Unity title — and, on device, the one whose translation being
        # present proves this target's guest was embedded rather than the other
        # app's.
        "entry":   "libmain",
        # Which boot sequence runs. "unity" is libmain -> NativeLoader.load ->
        # libunity -> the Android lifecycle; "steamlink" is kl_slink's doors.
        "kind":    "unity",
        "product": "Klepton",
        # The version is in the name of the OLD builds: 1.40 is the one players
        # install, and on the Home Screen it is simply the game.
        "display": "Beat Saber 1.6",
        # See beatsaber128.
        "hand_matting": False,
    },
    "beatsaber140": {
        # Beat Saber 1.40 beside 1.6.0, as its own app. A version variant of
        # `beatsaber` normally just takes over beatsaber/ (see the trees note in
        # tools/gen_libc_table.py); a second ROW is what gives it a bundle id, a
        # container and a userdata profile of its own, so both stay installed.
        # Split application binary: the OBB is main.<versionCode>.<package>.obb
        # under this target's userdata, not beatsaber's.
        "libs":    None,
        "srcdir":  "beatsaber140/lib/arm64-v8a",
        "tree":    "beatsaber140",
        "apk":     "beatsaber-1.40.apk",
        "assets":  "beatsaber140/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "Klepton",
        "display": "Beat Saber",
        # See beatsaber128.
        "hand_matting": False,
        # The in-app data panel (KleptonGameData.swift): the OBB's file name,
        # and the store's binary ids for the APK and the OBB this build was
        # translated from. Empty ids leave the panel on its no-network path
        # (copy the files in through Files, then Unpack).
        "obb_file":    "main.1716.com.beatgames.beatsaber.obb",
        "meta_apk_id": "30506607092319862",
        "meta_obb_id": "30506602615653643",
        # Foveated rendering is on by default (it is for every row) and a copy
        # that stored "off" gets it back once. For Beat Saber from 1.45 on it is
        # another thing than the setting people switched off: the game renders
        # through the display's own map, a curve per eye, at the display's full
        # size, and the unwarp no longer waves at the edge of the dense region.
        # On device (2026-10-12) that held the panel's 120 Hz for a whole
        # session at 6890x5526 an eye, where the same game unfoveated at 4000
        # fell to 60 Hz inside a level. A choice made after this start stays.
        "foveation_reset": 1,
    },
    "beatsaber128": {
        # Beat Saber 1.28 (Unity 2019.4), a third row for the same reason as
        # beatsaber140: its own bundle id, container and userdata. Not a split
        # binary — the assets are in the APK, so there is no OBB to stage.
        "libs":    None,
        "srcdir":  "beatsaber128/lib/arm64-v8a",
        "tree":    "beatsaber128",
        "apk":     "beatsaber-1.28.apk",
        "assets":  "beatsaber128/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "Klepton",
        "display": "Beat Saber 1.28",
        # Beat Saber draws its own sabers where the hands are, so the system's
        # matte of the user's arms (and the controllers in them) over the
        # picture is a second pair in the same place. False opens the space with
        # .upperLimbVisibility(.hidden); see KLChromaSettings.handMatting.
        "hand_matting": False,
    },
    "superhot": {
        # SUPERHOT VR — Unity + IL2CPP + the Oculus Mobile SDK, i.e. the same
        # shape as Beat Saber and the reason it is the third target: it shares
        # the entire Unity path and exercises it against an APK nothing here was
        # written against. Its own manifest declares the same
        # com.unity3d.player.UnityPlayerActivity and the same VR intent
        # category, so nothing about the front door differs.
        # 
        # Not a split-binary build: the assets are in the APK, so there is no
        # OBB to stage (stage_assets.sh treats one as present-or-absent).
        "libs":    None,
        "srcdir":  "superhot/lib/arm64-v8a",
        "tree":    "superhot",
        "apk":     "superhot.apk",
        "assets":  "superhot/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonSuperhot",
        "display": "SUPERHOT",
    },
    "walkabout": {
        # Walkabout Mini Golf 6.7 (Mighty Coconut), package
        # com.MightyCoconut.WalkaboutMiniGolf — Unity + IL2CPP through the stock
        # com.unity3d.player.UnityPlayerActivity, Beat Saber 1.40's shape:
        # libOVRPlugin + libOculusXRPlugin for the display, libovrplatformloader
        # for the Meta platform.
        #
        # Split application binary (assets/unity_obb_guid is present): the APK
        # carries the first scene only and the courses are in
        # main.60366.com.MightyCoconut.WalkaboutMiniGolf.obb under this target's
        # userdata.
        "libs":    None,
        "srcdir":  "walkabout/lib/arm64-v8a",
        "tree":    "walkabout",
        "apk":     "walkabout.apk",
        "assets":  "walkabout/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonWalkabout",
        "display": "Walkabout Mini Golf",
        # The downloadable courses' `<course>common_assets_.bundle` files live
        # BESIDE the packaged OBB, in Android's own obb directory, and the main
        # menu's Addressables dependencies name all ten: with any missing it
        # retries the load forever on the loading screen. So that directory is
        # staged too (real bundles from the headset, or the placeholders
        # tools/walkabout_stub_bundles.py writes).
        "stage_files": "Android/obb/com.MightyCoconut.WalkaboutMiniGolf",
        # The NAME is load-bearing in runtime/kl_dl.c: libMetaXRAudioUnity, refused
        # on every guest, is served to this one as a no-op library, because its
        # audio setup throws on each call into the absent plugin (KL_METAXR_AUDIO_STUB).
        # See beatsaber128: the game draws its own hands and putter where the
        # user's are, so the system's matte of the real arms is a second pair in
        # the same place. False opens the space with
        # .upperLimbVisibility(.hidden).
        "hand_matting": False,
    },
    "x2582932495064035": {
        # Real VR Fishing 3.06102 (com.Miragesoft.RealVrFishingQ).  Keep an
        # explicit AVP Play target so this game uses the normal launcher and
        # stable bundle/product identity instead of the generic Klepton route.
        "libs":    None,
        "srcdir":  "x2582932495064035/lib/arm64-v8a",
        "tree":    "x2582932495064035",
        "apk":     "x2582932495064035.apk",
        "assets":  "x2582932495064035/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonX2582932495064035",
        "display": "Real VR Fishing",
        "hand_matting": False,
        "stage_files": "Android/obb/com.Miragesoft.RealVrFishingQ",
        "obb_file":    "main.1219.com.Miragesoft.RealVrFishingQ.obb",
        "meta_apk_id": "29566115676319018",
        "meta_obb_id": "29566100019653917",
    },
    "batman": {
        # Batman: Arkham Shadow 1.4.1 (com.camouflaj.manta, versionCode 350961).
        # Unity 2022 + IL2CPP + Burst, the Oculus XR plugin, Wwise for audio,
        # and Vulkan REQUIRED by its manifest — so the same shape as bonelab
        # (MoltenVK), on a title shipped for Quest 3 only. A FEASIBILITY target:
        # added to run the host gates and the shim gap list against it, not
        # because it is known to boot.
        #
        # The NAME is load-bearing: the runtime already special-cases a target
        # called "batman" — the Adreno identity MoltenVK's device is reported
        # under (kl_vulkan.c, KL_VK_SPOOF_ADRENO), the Quest 3 hardware identity
        # (kl_jni.c), and two smaller ones. Under any other name this title pops
        # "Your device does not match the hardware requirements", abandons
        # Vulkan and dies in a GLES2 context — measured, as "arkhamshadow".
        "libs":    None,
        "srcdir":  "batman/lib/arm64-v8a",
        "tree":    "batman",
        "apk":     "batman.apk",
        "assets":  "batman/assets",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        # Its own product, so its generated .xcodeproj sits beside Klepton.xcodeproj
        # instead of replacing whichever Beat Saber build that one holds.
        "product": "KleptonArkham",
        "display": "Arkham Shadow",
        # The game draws its own gauntlets where the hands are; the passthrough
        # hands on top of them are a second, mismatched pair. Default them off
        # (the in-app toggle still turns them back on).
        "hand_matting": False,
        # Fraction of the capped eye-texture size the game is told (the
        # in-app "Render resolution" slider starts here). Measured on an M5 at
        # 1.0 = 3072x2464 per eye: 71 fps in the menus, 50 in play, and in play
        # the compositor itself fell from 100 Hz to 50 — every reprojection
        # shown twice, judder on head turns. The game's own fixed foveation
        # does not exist under MoltenVK, so resolution is the lever. 0.7 is
        # half the pixels.
        # The picture since 2026-10-13: foveated rendering at "high" (kl_fove,
        # taken over from rebelancap's fork) at an eye cap of 4608 (kl_ovrp.c)
        # and full render scale — his settings for this game from avp-play
        # issue 16, without the 90 Hz he also names. Until then it ran unfoveated
        # at 70% of a 3072 cap. On the owner's headset it draws 25.7% of the
        # 4608x3697 eye that way (2144x2048), more picture points than before
        # and far sharper in the middle, with no frame past its deadline; peak
        # footprint 5.6 GB with 2.6 GB free. He looked at a test copy with his
        # own save and made it the default.
        # The 90 Hz pin was tried and taken out: here the panel ran at 50 and at
        # 100 Hz in turn while the game was told 90, where it is otherwise told
        # the panel's own rate. His fork separates the pinned rate from the
        # panel's; this one does not have that yet.
        # picture_reset: a copy installed under the old defaults drops its
        # stored resolution and foveation level once, so everyone gets these.
        "render_scale": 1.0,
        "vk_foveation": "high",
        "picture_reset": 1,
        # In-game video on by default. A Vulkan guest's is otherwise refused
        # (kl_mediandk.c, klm_extractor_enabled), on the assumption that decoded
        # frames reach the renderer as AHardwareBuffers imported into Vulkan —
        # which this title's Unity never does: it decodes with no surface and
        # uploads the NV12 bytes itself, and that path is served. Confirmed on
        # device with its tutorial clips.
        "video": True,
        # Its OBB directory is not one packaged *.obb: 69 Addressables bundles,
        # three audio OBBs, the videos and a manifest sit LOOSE beside the main
        # OBB, and the game opens each by name. Stage the directory wholesale
        # (stage_assets.sh, KLT_OBB_RAW) or the device gets one file of 84.
        "obb_raw": 1,
        # On from the start, as the owner plays it (2026-10-17): every second
        # refresh (an even 50 of 100 rather than 50-70 unevenly), and shaders
        # compiled behind the picture — which took the half-second freezes of
        # first-seen places out of his session.
        "test_on": "KL_GUEST_PACE_DIV,KL_VK_ASYNC_PIPES",
    },
    "bonelab": {
        # BONELAB — Unity 2021.3.16f1 + IL2CPP, and the same front door again
        # (com.unity3d.player.UnityPlayerActivity, the Oculus VR intent
        # category). Two things make it the fourth target rather than a fourth
        # copy of the third:
        # 
        #  - it is a SPLIT APPLICATION BINARY with TWO obbs. Beat Saber 1.40
        #     taught this project that a guest's data can live beside the APK;
        #     this one ships main.2974 AND patch.2974, 6.8 GB together, and the
        #     patch is where the UnitySubsystems manifests and the whole
        #     Addressables catalogue are. Nothing here pins "main" — the obb
        #     directory is staged wholesale, so a guest that reads a patch reads
        #     one (see stage_assets.sh).
        #   - its boot.config takes the XR SDK path (`xrsdk-pre-init-library=
        #     OculusXRPlugin`, `xr-meta-enabled=1`), which is 1.40's path and not
        #     SUPERHOT's legacy `vr-device-list=Oculus`. It also SHIPS
        #     libopenxr_loader and libMicrosoftOpenXRPlugin, which the pre-init
        #     line says it does not use — so a run that ends up in OpenXR is a
        #     run that fell out of the Oculus path, and that is worth reading as
        #     a symptom rather than as a second front door.
        # 
        # libSLZQuestNative and libRF_CNative_andr are the title's own; neither
        # is replaced, so both translate like any other guest library.
        "libs":    None,
        "srcdir":  "bonelab/lib/arm64-v8a",
        "tree":    "bonelab",
        "apk":     "bonelab.apk",
        "assets":  "bonelab/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonBonelab",
        "display": "BONELAB",
    },
    "vrchat": {
        # VRChat — Unity 2022.3.22f2-DWR + IL2CPP, and the FIFTH target. It is
        # the first guest here that is not an Oculus title: this is the STEAM
        # FRAME build, so there is no libOVRPlugin, no libvrapi and no Oculus
        # Platform loader anywhere in it. It speaks OPENXR, through the stack
        # Unity ships for it — libopenxr_loader (the Khronos loader),
        # libUnityOpenXR (the XR SDK provider) and libUnityOpenXRHands — and
        # boot.config says so in one line: `xrsdk-pre-init-library=UnityOpenXR`,
        # where BONELAB's says OculusXRPlugin.
        # 
        # That makes it the target that joins the project's two halves: the
        # Unity/IL2CPP path (Beat Saber, SUPERHOT, BONELAB) and the OpenXR
        # runtime written for Steam Link (runtime/xr/kl_openxr.c), which until now
        # only ever had a non-Unity guest on top of it.
        # 
        # The front door is stock — com.unity3d.player.UnityPlayerActivity and
        # libmain's JNI_OnLoad — even though the manifest names an OBFUSCATED
        # Application class (aGGhd3.kN5sj1.jrtzH2.r3jyW1), which is the title's
        # own and loads libloader.so from Java. We do not run Java, so nothing
        # here depends on that library; if something ends up demanding it, that
        # is a symptom to read rather than a step to pre-empt.
        # 
        # Not a split binary: 317 MB of APK with assets/bin/Data inside it and
        # no OBB beside it. libil2cpp.so alone is 297 MB.
        "libs":    None,
        "srcdir":  "vrchat/lib/arm64-v8a",
        "tree":    "vrchat",
        "apk":     "vrchat.apk",
        "assets":  "vrchat/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonVRChat",
        "display": "VRChat",
    },
    "openbrush": {
        # Open Brush — Unity 2022.3.62f2 + IL2CPP, and the SIXTH target. Two
        # things make it worth adding rather than a sixth copy of the Unity row:
        # 
        #  - it is the **OPENXR build**, deliberately, and not the Oculus store
        #     one. Open Brush ships both; this tree is the former, which
        #     boot.config says in the same line VRChat's does
        #     (`xrsdk-pre-init-library=UnityOpenXR`) and the manifest says again
        #     in the Khronos permissions and the runtime-broker `<queries>`.
        #     There is no libOVRPlugin, no libvrapi and no Oculus Platform
        #     loader — so it takes the Unity + kl_openxr path VRChat opened, and
        #     it is the SECOND guest on it, which is the point: until now that
        #     junction had exactly one title holding it up.
        #   - **the sources are public** (github.com/icosa-foundation/open-brush),
        #     which no other target here has. Everything else in this project is
        #     reverse-engineered from a stripped .so, so when this one does
        #     something inexplicable the answer can be READ instead of derived.
        #     Nothing in the build depends on that; it is a debugging lever.
        # 
        # The front door is stock — com.unity3d.player.UnityPlayerActivity and
        # libmain's JNI_OnLoad — with no obfuscated Application class of the kind
        # VRChat carries.
        # 
        # Not a split binary: 242 MB of APK with assets/bin/Data AND the
        # Addressables catalogue (assets/aa) inside it, so there is no OBB to
        # stage.
        # 
        # UNSETTLED and the first thing a run has to answer: the manifest
        # declares `android.hardware.vulkan.version` as REQUIRED. That is
        # BONELAB's question, and BONELAB's recipe answers it — whether the guest
        # dlopens libvulkan.so, and which EGL entry points it reaches
        # (KL_EGL_TRACE=1). Unlike BONELAB's day, either answer is now a path
        # that exists; which one this is decides whether the eye textures arrive
        # through kl_glfb or kl_vulkan.
        "libs":    None,
        "srcdir":  "openbrush/lib/arm64-v8a",
        "tree":    "openbrush",
        "apk":     "openbrush.apk",
        "assets":  "openbrush/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonOpenBrush",
        "display": "Open Brush",
    },
    "re4": {
        # Resident Evil 4 VR — and the SEVENTH target, which is the first one
        # here that is not a Unity game and not Steam Link. It is **UNREAL
        # ENGINE 4.25.3** (branch ++VR4+VR4), a Quest exclusive, package
        # com.Armature.VR4.
        # 
        # That is the whole reason to add it. Every engine-shaped thing this
        # project knows was learned from Unity: five of the six targets are
        # Unity + IL2CPP, and the sixth (Steam Link) is not a game engine at
        # all. So a whole half of the shim has only ever been exercised in one
        # dialect, and the parts that are genuinely Android rather than Unity
        # have never been asked a second opinion.
        # 
        # Three things differ at the door, all measured from the APK:
        # 
        #  - **the entry is a NativeActivity.** libUE4.so exports
        #     ANativeActivity_onCreate, android_main AND JNI_OnLoad; the
        #     manifest names com.epicgames.ue4.GameActivity with
        #     `android.app.lib_name = UE4`. So the guest is STARTED through the
        #     NDK's activity door and driven by callbacks it fills in, where a
        #     Unity guest is started through libmain's JNI_OnLoad and driven by
        #     calling natives it registered. Only Steam Link's VR door has ever
        #     used this path, and that is one non-Unity library rather than an
        #     engine.
        #   - **input arrives as an AInputQueue.** AInputEvent / AKeyEvent /
        #     AMotionEvent are in the unresolved set, and Beat Saber uses
        #     none of them: Unity takes its input over JNI. This is the NDK
        #     surface nothing has needed yet.
        #   - **assets come through AAssetManager**, not over JNI —
        #     AAsset_getBuffer and AAsset_openFileDescriptor are both
        #     unresolved. The same note records that Unity reads its assets
        #     through Context.getAssets() and a Java InputStream instead.
        # 
        # A split application binary with TWO obbs, BONELAB's shape: main.203
        # and patch.203, 8.5 GB together, staged wholesale.
        # 
        # XR is VrApi/OVRPlugin — both libraries ship — and NEITHER is in
        # libUE4's DT_NEEDED, so both are dlopen'd and kl_ovrp claims them the
        # way it always has. `bSupportsVulkan` is true in the manifest and
        # there is no libvulkan in the link either, so which graphics API this
        # takes is a question for the first run rather than the table.
        # 
        # THE APK CARRIES AN INJECTED PAYLOAD AND IT IS NOT LOADED. libfrda.so
        # is a Frida gadget and libscript.so is its script; libfrda.config.so
        # is plain JSON and says what it is for — `patch_ovrplatformloader`,
        # `patch_vrapi`, `patch_libc`, `hijack_responses`, i.e. an Oculus Store
        # entitlement bypass. None of it is part of the game. It is excluded by
        # the Makefile's GUEST_EXCLUDED rather than here (`libs: None` asks the
        # Makefile, which is the one place those rules live), and it would be
        # INERT regardless: the three libraries it patches are the three this
        # project REPLACES, so there is nothing of the guest's for it to reach.
        # The DRM line is unchanged and is kl_ovrplat's — the app's own
        # entitlement is answered on the premise the user owns the title, and
        # everything that delivers content still refuses.
        "libs":    None,
        "srcdir":  "re4/lib/arm64-v8a",
        "tree":    "re4",
        "apk":     "re4.apk",
        "assets":  "re4/assets",
        "qtplugins": "",
        # Where this guest looks for its OBB, relative to the external-storage
        # root. **UE4 builds the path ITSELF** and it is ANDROID's layout, not
        # the flat one the Unity guests use: <external>/Android/obb/<package>/
        # main.<versionCode>.<package>.obb. Nothing here calls getObbDirs(), so
        # staging into <files>/obb/ puts 8.5 GB somewhere the engine never looks
        # — and it fails SILENTLY, because a UE4 build reports a missing OBB the
        # same way it reports a missing .uproject: it carries on.
        "obb":     "Android/obb/com.Armature.VR4",
        # The library the chain starts at, and the one whose translation being
        # in the bundle proves THIS guest was embedded. There is no libmain
        # here: UE4 links its engine, its game and its plugins into one 172 MB
        # object and the manifest names it by `android.app.lib_name`.
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonRE4",
        "display": "Resident Evil 4",
    },
    "redmatter2": {
        # Red Matter 2 (Vertical Robot), package com.VerticalRobot.RedMatter2 —
        # UE4 through com.epicgames.ue4.GameActivity, RE4/wrath2's shape. Audio
        # is the Oculus spatializer alone (libovraudio64 in DT_NEEDED, no FMOD,
        # no Wwise), which the optional-if-absent chain handles by skipping the
        # FMOD members. No Frida payload; `libs: None` finds nothing to strip.
        "libs":    None,
        "srcdir":  "redmatter2/lib/arm64-v8a",
        "tree":    "redmatter2",
        "apk":     "redmatter2.apk",
        "assets":  "redmatter2/assets",
        "qtplugins": "",
        # UE4 builds <external>/Android/obb/<package>/ itself — RE4's layout.
        "obb":     "Android/obb/com.VerticalRobot.RedMatter2",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonRedMatter2",
        "display": "Red Matter 2",
    },
    "zix": {
        # ZIX (Hidden Gem), package com.HiddenGemIO.ZIX — Unity + IL2CPP, the
        # libmain front door. Two things worth knowing before its first run:
        # audio is Wwise (libAkUnitySoundEngine), and XR ships BOTH stacks —
        # libOVRPlugin next to libUnityOpenXR/libopenxr_loader — so which path
        # the boot takes is worth reading out of the log rather than assuming.
        # EOS (libEOSSDK) is aboard for the online half.
        "libs":    None,
        "srcdir":  "zix/lib/arm64-v8a",
        "tree":    "zix",
        "apk":     "zix.apk",
        "assets":  "zix/assets",
        "qtplugins": "",
        # Flat Unity layout: Context.getObbDirs() answers <files>/obb.
        "obb":     "obb",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonZIX",
        "display": "ZIX",
    },
    "vampire": {
        # Vampire: The Masquerade — Justice (Fast Travel Games), package
        # com.fasttravelgames.lollipop (the codename, not a typo) — UE4,
        # GameActivity door, wrath2's shape exactly: FMOD audio plus the Oculus
        # spatializer, playcore for the OBB half. libbhaptics-native-lib is a
        # vest-haptics SDK; nothing here needs to speak it for the game to run.
        "libs":    None,
        "srcdir":  "vampire/lib/arm64-v8a",
        "tree":    "vampire",
        "apk":     "vampire.apk",
        "assets":  "vampire/assets",
        "qtplugins": "",
        "obb":     "Android/obb/com.fasttravelgames.lollipop",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonVampire",
        "display": "Vampire: The Masquerade - Justice",
    },
    "vicecity": {
        # GTA Vice City VR (GTAVCVR) — a reVC/re3 (open-source re-implementation)
        # VR port. A plain Android NativeActivity + OpenXR native app: the entry is
        # ANativeActivity_onCreate in libmiamivr.so ("Miami" = Vice City) and the
        # game runs its own loop on the android_main thread, so it is the new
        # "native" kind, not Unity/UE4. No OBB and no assets/ inside the APK — the
        # whole data set is the loose files/gamedata tree (gta3.img, cuts.img,
        # txd, TEXT, Audio, reVC.ini, vr_settings.ini), staged through files/.
        "libs":    None,
        "srcdir":  "vicecity/lib/arm64-v8a",
        "tree":    "vicecity",
        "apk":     "vicecity.apk",
        # The APK ships no assets/; stage_assets.sh stages it present-or-absent.
        "assets":  "vicecity/assets",
        "qtplugins": "",
        # No OBB — the shared "nothing here" answer, as jkxr uses.
        "obb":     "obb",
        # The game data: userdata/vicecity/files/gamedata -> android-files/files.
        "stage_files": "files",
        "entry":   "libmiamivr",
        "kind":    "native",
        "product": "KleptonViceCity",
        "display": "Vice City",
    },
    "intotheradius": {
        # Into the Radius (CM Games), package com.CMGames.IntoTheRadius — UE4
        # through com.epicgames.ue4.GameActivity, RE4/wrath2's shape. Split
        # binary (a DownloaderActivity fronts the OBB). Ships bhaptics haptics
        # and Sentry crash reporting (libsentry/libsentry-android) beside the
        # engine; neither is needed to run, and Sentry's native uploader is the
        # kind of thing to watch if a run dies right after it initialises.
        "libs":    None,
        "srcdir":  "intotheradius/lib/arm64-v8a",
        "tree":    "intotheradius",
        "apk":     "intotheradius.apk",
        "assets":  "intotheradius/assets",
        "qtplugins": "",
        # UE4 builds <external>/Android/obb/<package>/ itself — the Android
        # layout, like RE4/wrath2.
        "obb":     "Android/obb/com.CMGames.IntoTheRadius",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonIntoTheRadius",
        "display": "Into the Radius",
    },
    "wrath2": {
        # Asgard's Wrath 2 (Sanzaru), package com.Sanzaru.Wrath2, versionCode
        # 5513 (12.1.1689309) — UE 4.27.2 on Sanzaru's branch, GameActivity door,
        # the shape vampire's row calls "wrath2's": FMOD Studio audio plus the
        # Oculus spatializer, playcore, and libFbnsPlugin (Meta push, excluded by
        # kl_dl.c). Vulkan only.
        #
        # The NAME is load-bearing, more than any other row's: the runtime
        # already carries this title's measured workarounds under it — two guest
        # patches keyed on THIS build's instruction words (IsVulkanAvailable and
        # FShaderPipelineCache::NumPrecompilesRemaining, kl_guestpatch.c; all
        # four expect-words verified against this APK), the no-async-loading-
        # thread command line (kl_ue4.c), the MoltenVK compile timeout
        # (kl_vulkan.c), the FMOD-poll usleep cap and guest QoS (kl_driver.c),
        # and the ICU directory synthesis (kl_obbmap.c).
        #
        # One OBB, main.5513, tens of GB, read through in place.
        "libs":    None,
        "srcdir":  "wrath2/lib/arm64-v8a",
        "tree":    "wrath2",
        "apk":     "wrath2.apk",
        "assets":  "wrath2/assets",
        "qtplugins": "",
        "obb":     "Android/obb/com.Sanzaru.Wrath2",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonWrath2",
        "display": "Asgard's Wrath 2",
        "autoboot": False,
        "hand_matting": False,
    },
    "moss": {
        # Moss (Polyarc), package com.polyarc.MossGame, versionCode 151047 —
        # UE 4.27.2 through com.epicgames.ue4.GameActivity, RE4's shape, built
        # for the original Quest (com.oculus.supportedDevices = quest). Audio is
        # Wwise, linked into libUE4 with its effect plugins beside it as loose
        # libAk*.so; XR is OVRPlugin 1.70 (the real plugin ships and is
        # replaced, as always). bSupportsVulkan is true and both RHIs are in the
        # binary, so which one it takes is read off the first run. A FEASIBILITY
        # target until it has booted.
        "libs":    None,
        "srcdir":  "moss/lib/arm64-v8a",
        "tree":    "moss",
        "apk":     "moss.apk",
        "assets":  "moss/assets",
        "qtplugins": "",
        # UE4 builds <external>/Android/obb/<package>/ itself — RE4's layout.
        "obb":     "Android/obb/com.polyarc.MossGame",
        "entry":   "libUE4",
        "kind":    "ue4",
        "product": "KleptonMoss",
        "display": "Moss",
        # Wait for the Start button instead of booting as soon as the window
        # is up, so the settings below it can be changed first.
        "autoboot": False,
        # Moss draws its own controllers; the passthrough hands on top of them
        # are a second pair. Off by default, the toggle turns them back on.
        "hand_matting": False,
        # The "Foveated rendering" setting starts ON for this target. Note what
        # that setting reaches: the rate map is attached to render passes
        # Klepton creates, i.e. an OpenGL guest's. A Vulkan guest's passes are
        # MoltenVK's, so for Moss this is a stored preference with no effect on
        # the picture until a Vulkan path for it exists.
        "foveation": True,
        # The picture since 2026-10-13, the owner's setting on the headset: the
        # display's own eye size (4608, kl_ovrp.c) with the game's own 1.5x held to
        # it, foveated rendering at "high" (kl_fove), 16x anisotropic filtering
        # (kl_vulkan.c) and a frame on every second refresh (kl_app.c). Before, it
        # took 4608x3697 an eye by its own scale over a 3072 cap, unfoveated – 45
        # frames a second. picture_reset: an installed copy drops its stored
        # resolution and foveation level once, so everyone gets these.
        "vk_foveation": "high",
        "picture_reset": 1,
        # On an ordinary gamepad (see KleptonControllers.pollGamepad) Quill's
        # attack belongs on Square, where the game's first release had it and
        # where a thumb expects it. Moss reads attack from Touch B, which the
        # default layout puts on Circle — right for the upper button of a Touch
        # controller, wrong for a pad. So on a pad, and only there: Square is B
        # and Circle takes Square's X. Touch and Sense controllers are untouched.
        "gamepad_map": "square=b,circle=x",
        # On from the start, as the owner tested it (2026-10-17): shaders
        # compiled behind the picture. The first session of a fresh copy
        # compiled 392 pipelines, 46 s in the compiler, with no freeze left
        # but one of 170 ms at the start. And for the Steam build only, the
        # pipeline list the game ships compiled at start, as the Meta build
        # does (kl_ue4.c, KL_UE4_PSO_LIST) – the owner's word: on for every
        # player, off only where it makes trouble.
        "test_on": "KL_VK_ASYNC_PIPES,KL_UE4_PSO_LIST",
    },
    # ---- JKXR: ONE APK, TWO GAMES, TWO ROWS ----
    #
    # JKXR is Team Beef's VR port of Jedi Knight II: Jedi Outcast and Jedi
    # Knight: Jedi Academy, built on OpenJK. It is the EIGHTH target and the
    # smallest surface any of them has presented: ten guest libraries, 22 MB of
    # engine, and NINE unresolved imports across the whole tree (eight libc
    # names and one that resolves inside the chain).
    #
    # It is here because it is a THIRD engine and a second shape of front door,
    # and both are cheap:
    #
    #  - **the natives are static exports.** The manifest names
    #     com.drbeef.jkxr.GLES3JNIActivity, a plain Activity that is also a
    #     SurfaceHolder.Callback, and its nine natives are declared on
    #     com.drbeef.jkxr.GLES3JNILib and exported by both engines as
    #     `Java_com_drbeef_jkxr_GLES3JNILib_*`. So there is no RegisterNatives
    #     census to read and no NativeActivity glue: the driver stands in for
    #     the Java by calling those exports in the order the Activity calls
    #     them, and the handle onCreate returns is the argument to every one
    #     after it.
    #   - **OpenXR over GLES**, which is the pairing this project has only ever
    #     had under Steam Link. The engine's forty-odd xr* imports bind at
    #     relocation time through kl_shim's OpenXR tier, and the pixels go
    #     through libgl4es — a GL 1.x-to-GLES translator the port carries
    #     because the Quake 3 renderer is fixed-function. That makes gl4es, not
    #     the game, the thing on the far side of kl_egl.
    #
    # TWO ROWS OVER ONE TREE, which no other target here does. The APK ships
    # BOTH engines and both games' VR pk3s; the Java picks between them by
    # reading `/sdcard/JKXR/commandline.txt` and appending the result to
    # "openjk_", so which library is the entry is a property of a FILE rather
    # than of the APK. Splitting it into two rows is what gives each game its
    # own userdata directory, bundle id, .app and translated-guest directory —
    # which matters here more than usual, because the two games' data
    # directories are DIFFERENT (JK3/base vs JK2/base) and each holds retail
    # assets the other cannot read. Everything a row differs in is derived from
    # `entry`: kl_jkxr takes the token after "libopenjk_" and it names the
    # renderer, the game module, the data directory and the command line, so
    # the row and the file the guest reads cannot disagree.
    #
    # One consequence worth knowing: the two rows share a libdir, and
    # kl_target_resolve takes the FIRST row matching a path, so the path form
    # (`jkxr/lib/arm64-v8a`, which is what the Makefile gates pass) means
    # Academy. Name the target — `TARGET=jkxr_jo` — to get the other one.
    #
    # THE ASSETS ARE NOT IN THE APK and are not an OBB either. This is an engine
    # port, so it ships the VR-specific pk3s (28 MB, in assets/) and expects the
    # RETAIL GAME DATA beside them — assets0.pk3 and up, which the user owns and
    # supplies. The Java copies its own pk3s to <ext>/JKXR/JK3/base on first
    # run; kl_jkxr does that half, and KL_JKXR_DATA points once at an existing
    # install so it can link the retail half in beside them. Nothing here can
    # fabricate them, and the engine reports them missing the way Quake 3 always
    # has: it refuses to start.
    "jkxr_ja": {
        "libs":    None,
        "srcdir":  "jkxr/lib/arm64-v8a",
        "tree":    "jkxr",
        "apk":     "jkxr.apk",
        "assets":  "jkxr/assets",
        "qtplugins": "",
        # No OBB. The field is not optional and "obb" is what every non-UE4
        # target says, so it stays the shared answer rather than becoming a
        # third layout that means "there is nothing here".
        "obb":     "obb",
        # Jedi Academy. The token after "libopenjk_" is the whole of what this
        # row differs in — see the block comment above.
        "entry":   "libopenjk_ja",
        "kind":    "jkxr",
        "product": "KleptonJKXRAcademy",
        "display": "JK Academy",
    },
    "jkxr_jo": {
        "libs":    None,
        "srcdir":  "jkxr/lib/arm64-v8a",
        "tree":    "jkxr",
        "apk":     "jkxr.apk",
        "assets":  "jkxr/assets",
        "qtplugins": "",
        "obb":     "obb",
        # Jedi Outcast.
        "entry":   "libopenjk_jo",
        "kind":    "jkxr",
        "product": "KleptonJKXROutcast",
        "display": "JK Outcast",
    },
    "steamlink-vr": {
        # BOTH front doors, because the app runs both: the 2D shell pairs in a
        # WindowGroup and hands off to the OpenXR half in an ImmersiveSpace, in
        # one process (an app bundle cannot re-exec the way `build/m_boot` does).
        # 
        # Three groups, and the third is the one that is easy to leave out:
        #  libvrlink_scene   the VR door. ONE library — its DT_NEEDED is
        #                     entirely Android system libraries we shim.
        #   the shell chain   fourteen, dependencies first, off libshell's own
        #                     DT_NEEDED (runtime/guest/kl_slink.c's CHAIN_SHELL).
        #   the Qt plugins    six, in NOBODY's DT_NEEDED. Qt dlopens them by
        #                     path at runtime — the platform QPA first, and
        #                     libshell aborts without it. A dlopen that finds no
        #                     translation falls through to the mmap ELF loader,
        #                     which is exactly the RWX-from-an-unsigned-file
        #                     shape AMFI exists to refuse.
        "libs":    "libvrlink_scene "
                   "libc++_shared libSDL3 libSDL3_image libSDL3_mixer libSDL3_ttf "
                   "libQt6Core_arm64-v8a libQt6Network_arm64-v8a libQt6Gui_arm64-v8a "
                   "libQt6Widgets_arm64-v8a libQt6Svg_arm64-v8a "
                   "libh264bitstream libhevcbitstream libsteamwebrtc "
                   "libshell_arm64-v8a "
                   "libplugins_platforms_qvirtual_arm64-v8a "
                   "libplugins_iconengines_qsvgicon_arm64-v8a "
                   "libplugins_imageformats_qgif_arm64-v8a "
                   "libplugins_imageformats_qico_arm64-v8a "
                   "libplugins_imageformats_qjpeg_arm64-v8a "
                   "libplugins_imageformats_qsvg_arm64-v8a",
        "srcdir":  "steamlink-vr/lib/arm64-v8a",
        "tree":    "steamlink-vr",
        "apk":     "steamlink-vr.apk",
        "assets":  "steamlink-vr/assets",
        # The six plugin .so files ALSO go into the container, as ELF, and not
        # as a loader path — kl_load_auto still resolves each of them to its
        # signed framework by basename, so nothing maps guest text from here.
        # 
        # **Qt reads a plugin as a FILE before it will load it.** libQt6Core's
        # search is a glob (`libplugins_%1_*.so`), so it lists the directory and
        # then parses each candidate's ELF metadata for the IID and the Qt
        # version. A directory of names the loader resolves is enough for
        # everything else in this project and is not enough for that: with no
        # real files nothing is ever a candidate, and libshell aborts with
        # `Could not find the Qt platform plugin "virtual"`.
        # 
        # Six files, 544 KB — not the whole 75 MB tree, because these are the
        # only libraries anything reads rather than loads.
        "qtplugins": "steamlink-vr/lib/arm64-v8a",
        # The VR door's library. The shell's fourteen load on top of it in the
        # same process; this is the one whose translation being in the bundle
        # says the Steam Link guest was embedded.
        # Where this guest looks for its OBB, relative to the external-storage
        # root (kl_jni_files_dir()). Unity asks Java for it —
        # Context.getObbDirs() answers <files>/obb — so every Unity target here
        # shares one layout and only the version code in the NAME differs.
        "obb":     "obb",
        "entry":   "libvrlink_scene",
        "kind":    "steamlink",
        # The userdata KEY, which is the row name everywhere else. It is spelled
        # out here because this directory holds the PAIRING CREDENTIAL: it is
        # named "steamlink" on disk, and keying it on the row instead would point
        # a working install at an empty directory — which presents as the Steam
        # host having deauthorized us and costs a re-pair to disbelieve.
        "userdata": "steamlink",
        "product": "KleptonSteamLink",
        "display": "Steam Link",
    },


    # ---- SDL2 + OpenXR VR engine ports (SCAFFOLD) ----
    #
    # Four targets over two engines that Klepton has not run before, added as
    # rows so each BUILDS, stages and boots to a first log; the engine drivers
    # (kl_xash / kl_source) are not written yet, so the "xash"/"source" kinds
    # currently share the NativeActivity harness (kl_native) — enough to load the
    # guest libraries and print DT_NEEDED, the shim gap and where the real entry
    # is (SDL_main / drbeef's GLES3JNILib), which is what the drivers get built
    # against. None boots the game yet; that is the per-engine work these rows set
    # up. No OBB (the shared "obb" = nothing-here answer); the whole data set is
    # the loose tree the user staged under userdata/<name>/<stage_files>.
    #
    #   Xash3D / GoldSrc:
    "cs1": {
        # Counter-Strike VR (com.lvonasek.csvr) — Xash3D + SDL2, entry libxash
        # (SDL_main). Data: userdata/cs1/xash (valve + cstrike) -> android-files/xash.
        "libs":    None,
        "srcdir":  "cs1/lib/arm64-v8a",
        "tree":    "cs1",
        "apk":     "cs1.apk",
        "assets":  "cs1/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "xash",
        "entry":   "libxash",
        "kind":    "sdl2",
        "product": "KleptonCSVR",
        "display": "Counter Strike",
    },
    "hl1": {
        # Half-Life VR / Lambda1VR (com.drbeef.lambda1vr) — Xash3D, drbeef's
        # GLES3JNILib exports (same shape as jkxr), entry libxash. Data:
        # userdata/hl1/xash (valve + HLGOLD + commandline.txt) -> android-files/xash.
        "libs":    None,
        "srcdir":  "hl1/lib/arm64-v8a",
        "tree":    "hl1",
        "apk":     "hl1.apk",
        "assets":  "hl1/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "xash",
        "entry":   "libxash",
        "kind":    "gles3jni",
        "product": "KleptonHalfLifeVR",
        "display": "Half Life",
    },
    "doom3quest": {
        # Doom3Quest 1.4.8 (Team Beef, com.drbeef.doom3quest) — id Tech 4 (d3es)
        # behind drbeef's GLES3JNILib door, entry libdoom3, OpenXR. The same
        # family as hl1/jkxr with one difference the driver carries: these
        # natives take no handle (onCreate returns void).
        #
        # The APK ships the original Doom 3 DEMO (assets/demo0N.pk4) beside the
        # port's own pak399; the Activity copies both into
        # /sdcard/Doom3Quest/base, and kl_gles3jni does the same here. Retail
        # pak000-008.pk4 from a copy the user owns go in
        # userdata/doom3quest/Doom3Quest/base and take over from the demo.
        "libs":    None,
        "srcdir":  "doom3quest/lib/arm64-v8a",
        "tree":    "doom3quest",
        "apk":     "doom3quest.apk",
        "assets":  "doom3quest/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "Doom3Quest",
        "entry":   "libdoom3",
        "kind":    "gles3jni",
        "product": "KleptonDoom3",
        "display": "Doom 3",
        # The game draws its own arms and weapons where the hands are; the
        # system's matte of the user's real arms over the picture is a second
        # pair in the same place (see beatsaber128).
        "hand_matting": False,
        # The whole of the eye-size ceiling kl_ovrp.c gives this target (4000):
        # 4400x4400 per eye. Affordable because the eyes are foveated (below) —
        # on device, 2026-10-08, 87-90 frames a second at this size with the
        # map against 31-55 without it. The "Render resolution" setting lowers
        # it (50% is the old picture).
        "render_scale": 1.0,
        # Fixed foveation on by default: this guest shades every pixel once per
        # light, and its eyes can take the map although they are mirrored
        # (kl_glfb.c, klfb_mirror_vrr).
        "foveation": True,
    },
    #   Source engine:
    "hl2": {
        # Half-Life 2 VR (com.valvesoftware.source.vrtest) — Source + SDL2
        # (org.libsdl.app.SDLActivity), entry liblauncher (SDL_main), libsourcevr
        # for the XR seam. Data: userdata/hl2/srceng (hl2, ep2, episodic, platform)
        # -> android-files/srceng.
        "libs":    None,
        "srcdir":  "hl2/lib/arm64-v8a",
        "tree":    "hl2",
        "apk":     "hl2.apk",
        "assets":  "hl2/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "srceng",
        "entry":   "liblauncher",
        "kind":    "sdl2",
        "product": "KleptonHalfLife2VR",
        "display": "Half Life 2",
    },
    "portal": {
        # Portal VR (com.portalVR.source) — Source + SDL2, entry liblauncher.
        # Data: userdata/portal/Source (portal, hl2, platform) -> android-files/Source.
        "libs":    None,
        "srcdir":  "portal/lib/arm64-v8a",
        "tree":    "portal",
        "apk":     "portal.apk",
        "assets":  "portal/assets",
        "qtplugins": "",
        "obb":     "obb",
        "stage_files": "Source",
        "entry":   "liblauncher",
        "kind":    "sdl2",
        "product": "KleptonPortalVR",
        "display": "Portal",
    },


    # ---- A Linux (glibc) process, not an Android app ----
    "alyx": {
        # Half-Life: Alyx — Valve's own Linux ARM64 build (Steam app 546560,
        # game/bin/linuxsteamrtarm64), run as a whole Linux process on the Steam
        # Linux Runtime's ld.so and glibc (runtime/linux/, kl_linuxproc). Nothing
        # of it is in this repository: the game and the runtime are the user's
        # own Steam files beside the checkout (../alyx, ../steamrt).
        #
        # No APK, no assets/, no OBB — those fields are the Android shape and are
        # empty here. The translated libraries are not a property of one
        # directory either: they come from the game AND the sysroot, and the set
        # is the one the process really loads (visionos/lx/alyx.loaded, recorded
        # by a host run with KL_LX_LOADLOG). mklxguest.sh builds them and writes
        # the list that `libs` is then read from — see lx_libs() below.
        "libs":    "",
        "srcdir":  "",
        "tree":    "lx",
        "apk":     "",
        "assets":  "",
        "qtplugins": "",
        "obb":     "obb",
        "entry":   "hlvr",
        "kind":    "linux",
        "product": "KleptonAlyx",
        "display": "Half-Life Alyx",
        # The Steam app whose public store artwork gives this target its icon
        # and start picture (visionos/steam_art.py): there is no APK to take
        # an icon from, and the installer only looks for Steam's pictures in a
        # Steam client's cache on the Mac, which most Macs do not have.
        "steam_app": "546560",
    },
    "x7527722310622065": {
        # Cave Crave 1.1.4 (com.rrrgames.CaveCrave, versionCode 739), Meta Store
        # app 7527722310622065. Unity 6 + IL2CPP. Otherwise exactly the generic
        # store row (_generic_row) — this row exists for ONE field:
        #
        # The game's Addressables content ships as separate store asset files
        # (scenematerials_*.bundle, catalog_1.1.4.*) that land BESIDE the main
        # OBB, in Android's own obb directory, and at boot the game looks for
        # them in getObbDir() ("[ADD] Searching for new asset files in ...")
        # before filing them under <obb>/Bundles/. With the generic Unity "obb"
        # the main OBB is in <files>/obb but the asset files are staged into
        # <files>/Android/obb/<package> — two directories where Android has
        # one — so every bundle "doesn't exist", the game reports corrupted
        # files and loads scenes without their materials. Android's layout puts
        # @obb and the asset files in the same place, and Unity is told that
        # place through getObbDirs().
        "libs":    None,
        "srcdir":  "x7527722310622065/lib/arm64-v8a",
        "tree":    "x7527722310622065",
        "apk":     "x7527722310622065.apk",
        "assets":  "x7527722310622065/assets",
        "qtplugins": "",
        "obb":     "Android/obb/com.rrrgames.CaveCrave",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonX7527722310622065",
        "display": "Cave Crave",
        "autoboot": False,
        "generic": True,
    },
}

DEFAULT = "beatsaber"


# ---- an experimental target for a game this table has never met ------------
#
# Every row above was written by someone who had the game in front of them. An
# installer that lets people TRY a game nobody has ported yet has no such row to
# point at, and inventing one per game by hand is exactly the work the attempt
# is meant to find out is worth doing.
#
# So one more target can be named from outside, for one build:
#
#     KLEPTON_GENERIC_TARGET=x<store app id>     the row's name
#     KLEPTON_GENERIC_DISPLAY=<title>            what the app is called
#
# Everything else is read off the unpacked APK, which the installer puts at
# <root>/<name>/ before anything here runs: which engine it is decides the entry
# library, the boot sequence and where the game looks for its OBB. That is all
# a row holds — none of the per-game adjustments the rows above carry. A game
# that needs one will not run until it has a row of its own; this only gets it
# as far as it gets on the engine's common path, which is the thing to report.
#
# The name is `x` and digits and nothing else, so it can never collide with a
# row above or be a path. It is also the tree, the APK name and — through
# bundle_for() — the app identifier, which keeps two such games apart on the
# device.
GENERIC_ENV = "KLEPTON_GENERIC_TARGET"

# Where a generic target's guest looks for its OBB, where the engine guess in
# _generic_row() is wrong for that one game. Keyed on the target name, so no
# other title changes.
#
# Sniper Elite VR (com.JustAddWater.SniperEliteVR, x2544573735586536) is Unity,
# but its levels are not one main.<version>.<package>.obb: they are ~27 loose
# AssetBundles (common_assets, bridge_scenes, quest2.hd, ...) that the store
# delivers into Android's own obb directory, <external>/Android/obb/<package>/,
# and that is where they are staged. The game asks Context.getObbDir() for that
# directory and opens the bundles from it; answering the Unity default
# <files>/obb gave it an empty directory, every "Unable to open archive file",
# and a loading screen that never ends.
GENERIC_OBB = {
    "x3635172946605196": "Android/obb/com.brink.vr.oculus.quest",
    "x2544573735586536": "Android/obb/com.JustAddWater.SniperEliteVR",
    # Ghost Town (com.FireproofStudios.GhostTown, x9531494193591052) is Unity,
    # and its scenes are loose AssetBundles the store delivers into Android's
    # obb directory, not a main.<version>.<package>.obb. At the fade-in it
    # asks Context.getObbDir() ("OBB Path: ...") and then reports every bundle
    # missing. The Unity default <files>/obb is empty; the bundles are staged
    # at <files>/Android/obb/<package>, which is the directory Android's
    # getObbDir() names.
    "x9531494193591052": "Android/obb/com.FireproofStudios.GhostTown",
    # iB Cricket (com.ProYuga.iBC, x5437449896288644) is Unity with a main OBB
    # AND ~30 store AssetBundles (stadium_*, jerseys_*, ...) that are staged
    # into <files>/Android/obb/<package>. At boot the game does not ask Java
    # for its OBB: it stat()s the hardcoded
    # /sdcard/Android/obb/<package>/main.<code>.<package>.obb, finds nothing
    # there with the Unity default <files>/obb and throws OBB_FILE_MISSING.
    # Android's layout puts @obb and the bundles in that one directory, as for
    # Cave Crave.
    "x5437449896288644": "Android/obb/com.ProYuga.iBC",
}


def ovrp_guest_version(tree):
    """The OVRPlugin version a game ships, read from <tree>/lib/arm64-v8a/
    libOVRPlugin.so: the one NUL-delimited "1.<minor>.<patch>" with a minor of 20
    or more in it (the plugin's own number; the 1.5.x beside it in most builds
    belongs to something else). "" when there is no such library or not exactly
    one candidate. The same rule as kl_ovrp_guest_version_in in kl_ovrp.c;
    checked against the games whose version was set by hand (1.98.0, 1.101.1,
    1.108.0, 1.109.0, 1.115.0)."""
    import os, re
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    path = os.path.join(root, tree, "lib", "arm64-v8a", "libOVRPlugin.so")
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return ""
    found = {m.group(1).decode() for m in re.finditer(rb"\x00(1\.(\d{1,3})\.\d{1,3})\x00", data)
             if int(m.group(2)) >= 20}
    return found.pop() if len(found) == 1 else ""


def _generic_row(name):
    import os, re
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    libdir = os.path.join(root, name, "lib", "arm64-v8a")
    try:
        libs = set(os.listdir(libdir))
    except OSError:
        libs = set()
    package, native_lib = "", ""
    try:
        with open(os.path.join(root, name, "AndroidManifest.xml"), errors="replace") as f:
            manifest = f.read(400_000)
        m = re.search(r'<manifest[^>]*\spackage="([A-Za-z0-9_.]+)"', manifest)
        package = m.group(1) if m else ""
        m = re.search(r'android:name="android\.app\.lib_name"\s+android:value="([A-Za-z0-9_.+-]+)"', manifest)
        native_lib = m.group(1) if m else ""
    except OSError:
        pass
    android_obb = f"Android/obb/{package}" if package else "obb"
    if "libUE4.so" in libs:
        kind, entry, obb = "ue4", "libUE4", android_obb
    elif "libUnreal.so" in libs:
        kind, entry, obb = "ue4", "libUnreal", android_obb
    elif libs & {"libil2cpp.so", "libunity.so", "libmono.so", "libmonobdwgc-2.0.so"}:
        # Unity asks Java for its OBB directory and gets the app's own (see the
        # note on "obb" at the superhot row).
        kind, entry, obb = "unity", "libmain", "obb"
    elif "libSDL2.so" in libs:
        kind, entry, obb = "sdl2", "libmain", "obb"
    else:
        # A NativeActivity names its library in the manifest; without that there
        # is nothing to go on, and libmain is the least surprising guess.
        kind, entry, obb = "native", ("lib" + native_lib) if native_lib else "libmain", android_obb
    obb = GENERIC_OBB.get(name, obb)
    display = os.environ.get("KLEPTON_GENERIC_DISPLAY", "") or name
    display = "".join(c for c in display if c.isprintable())[:60].strip() or name
    return {
        "libs": None,
        "srcdir": f"{name}/lib/arm64-v8a",
        "tree": name,
        "apk": f"{name}.apk",
        "assets": f"{name}/assets",
        "qtplugins": "",
        "obb": obb,
        "entry": entry,
        "kind": kind,
        "product": "Klepton" + name.upper(),
        "display": display,
        # Wait for Start, like every installed game; and say what this is.
        "autoboot": False,
        "generic": True,
    }


# What a store game is known to need on top of the generic row, where that is a
# DEFAULT of a setting and nothing about how it is built — so the row stays the
# one read off its APK and only the field named here differs.
STORE_DEFAULTS = {
    # Assassin's Creed Nexus VR. In-game video on: its loading screens play a
    # clip through Unity's VideoPlayer, and with the Vulkan refusal
    # (kl_mediandk.c, klm_extractor_enabled) the game opened that clip again on
    # every frame of the loading screen, each failed attempt keeping about
    # 0.6 MB — 50 MB a second, 2.4 GB over one long load, and the world's own
    # textures then went over the process limit (the app was killed on arrival).
    # Its Unity decodes with no surface and uploads the bytes itself, like
    # Arkham Shadow's, which is the path that is served.
    "x5812519008825194": {"video": True},
    # Marvel's Deadpool VR. Edge smoothing at 4x: the game renders 2x, its
    # settings are out of reach in an encrypted pak, and on device (Vision Pro
    # M5, 2026-10-12) 4x cost no frame time that could be told from the spread
    # between runs, and about 0.2 GB at 5 GB.
    # ...and foveated rendering at "medium" (kl_fove, taken over from rebelancap's
    # fork, who runs this game at "high" with a smaller box): the game ran
    # unfoveated here until then, the owner looked at a test copy with the
    # levels on a headset and chose medium as where it starts (2026-10-13).
    "x3240283705987790": {"vk_msaa": 4, "vk_foveation": "medium"},
    # Eye of the Temple. Foveated rendering off by default: in this game the
    # fixed sharp patch is very plainly visible and buys hardly any frame time
    # (the author on a headset, 2026-10-12), so the picture wins.
    # "foveation_reset" calls back
    # a choice a player's copy has already stored, once, so that the new default
    # reaches the people who play it now and not only new installs; see
    # KleptonRendering. They can turn it on again afterwards and it stays on.
    "x5361030930653377": {"foveation": False, "foveation_reset": 1},
    # Premium Bowling. Foveation off: it draws each eye into a texture of its
    # own, without multisampling, and copies that into the eye texture. Only
    # the copy is foveated (kl_glfb foveates the eye textures and a
    # MULTISAMPLED scene target), so nothing is saved — the scene is shaded in
    # full at 4915x3942 either way — and the copy thins the finished picture
    # to a third of its pixels with one tap each: stair-stepped edges
    # everywhere but the middle (on device, 2026-10-12; with it off the author
    # found the picture "deutlich besser").
    "x2773034772778845": {"foveation": False},
    # Synth Riders 3.6.6 (Unity 6000.3.13f1). Foveation off: it draws the scene
    # into a 4x MULTISAMPLED texture of the eye size and resolves that into the
    # eye texture with glBlitFramebuffer. The size rule foveates the scene
    # target, the blit does not rasterize through the map, so the eye received
    # the warped picture as it lay in the physical corner: shifted to the lower
    # left, distorted, and pink where nothing had been written (on device,
    # 2026-10-15). "foveation_reset" calls back a stored "on" once. (Fix 42.)
    "x2436558143118760": {"foveation": False, "foveation_reset": 1},
    # Moss: Book II (Polyarc, UE 4.27.2 like Moss). Moss's picture and its
    # shaders as the owner set them for Moss (see the moss row): foveated
    # rendering on at "high", reset once, the passthrough hands off (the game
    # draws its own), shaders compiled behind the picture from the start. The
    # runtime side is kl_driver_target_is_moss. Moss's pad layout is not copied:
    # this game's buttons are not looked at yet.
    "x4395292760584049": {"foveation": True, "vk_foveation": "high", "picture_reset": 1,
                          "hand_matting": False, "test_on": "KL_VK_ASYNC_PIPES"},
    # PowerWash Simulator VR 2.5.0 (Unity 2020.3, Vulkan). A frame on every
    # second refresh from the start, as its owner asked when calling it
    # verified (2026-10-19), with the eye cap of 4000 in kl_ovrp.c: an even 50
    # of 100 rather than a game that needs 8 to 11 ms of GPU a frame landing on
    # the next refresh or the one after. A player turns it off in the start
    # window (Advanced > Test), and that choice stays.
    "x3713270985434472": {"test_on": "KL_GUEST_PACE_DIV"},
    # Floor Plan 2 (Unity 2022.3, OVRPlugin 1.88, Vulkan). The owner's own
    # choices in the start window, made the default when it was verified
    # (2026-10-18): 120 Hz with a frame on every second refresh — an even 60 —
    # and 4096 an eye (the eye cap is its row in kl_ovrp.c, klovrp_title_cap).
    # Its physics step follows the refresh rate it is told (KL_OVRP_HZ_FOLLOW,
    # kl_app.c, on for it by default), or what it holds shakes in the hand.
    "x2401648309868679": {"max_rate": 120, "test_on": "KL_GUEST_PACE_DIV"},
    # Star Wars Pinball VR (UE 4.25, GLES). Its owner's choices in the start
    # window, made the default (2026-10-19): 120 Hz with a frame on every
    # second refresh — an even 60 — and 4608 an eye (klovrp_title_cap in
    # kl_ovrp.c). Both stay a player's choice in the start window.
    "x3180994211970184": {"max_rate": 120, "test_on": "KL_GUEST_PACE_DIV"},
    # Vader Immortal: Episode I (UE 4.19, GLES). Its owner's choices, made the
    # default when it was verified (2026-10-19): 120 Hz with a frame on every
    # second refresh — an even 60 — and 4096 an eye (klovrp_title_cap in
    # kl_ovrp.c). Episodes II and III are the same engine and get the same,
    # not yet run on a headset. All stay a player's choice in the start window.
    # Since 2026-10-20 all three have the owner's later picture settings, read
    # off his Episode I: 4608 an eye (kl_ovrp.c), rendered at 95 % and
    # upscaled to the full eye size by MetalFX (KL_METALFX_UPSCALE).
    "x2108775495884888": {"max_rate": 120, "render_scale": 0.95,
                          "test_on": "KL_GUEST_PACE_DIV,KL_METALFX_UPSCALE"},
    "x2223615721048141": {"max_rate": 120, "render_scale": 0.95,
                          "test_on": "KL_GUEST_PACE_DIV,KL_METALFX_UPSCALE"},
    "x2426206484098337": {"max_rate": 120, "render_scale": 0.95,
                          "test_on": "KL_GUEST_PACE_DIV,KL_METALFX_UPSCALE"},
    # Gods of Gravity (Unity 2021.3.45f2, OpenGL ES). Foveation off, Synth
    # Riders' case one step further: the scene goes into a MULTISAMPLED target
    # of the eye size, which the size rule foveates; its resolve (a blit, no
    # rasterization through the map) copies the squeezed picture as it lies
    # into a plain texture, and the final pass samples that by screen position
    # into the eye texture — through the map a second time. The compositor
    # undoes one squeeze, so the picture filled only part of the view and bent
    # when the head turned (on device, 2026-10-18; the double squeeze measured
    # on the Mac with KL_GLFB_MTL=1 KL_VRR=1 KL_GLFB_DUMP_FBOS=1).
    "x3919415921458774": {"foveation": False, "foveation_reset": 1},
    # HITMAN 3 VR: Reloaded (Glacier, Vulkan, OpenXR). Its owner's choices in
    # the start window, made the default for new players (2026-10-20): 100 Hz
    # (the mixed space, the default anyway), a frame on every second refresh —
    # an even 50, the half rate the Quest build draws at with space warp — 3584
    # an eye (klovrp_title_cap in kl_ovrp.c; 4096 ran out of memory at level
    # changes) and foveated rendering at "high".
    # No reset: a player who has chosen otherwise keeps it. Shaders compiled
    # behind the picture as well (KL_VK_ASYNC_PIPES, with KL_VK_ASYNC_RP): each
    # new one held the picture 0.3-0.9 s on the headset (2026-10-20); the Mac
    # mission looks the same with it once they are there.
    "x8012387035448196": {"foveation": True, "vk_foveation": "high",
                          "test_on": "KL_GUEST_PACE_DIV,KL_VK_ASYNC_PIPES"},
}


def _add_generic():
    import os, re
    name = os.environ.get(GENERIC_ENV, "")
    if not name or name in TARGETS:
        return
    if not re.fullmatch(r"x[0-9]{5,24}", name):
        print(f"!! {GENERIC_ENV}={name!r} is not a name of the form x<digits>", file=sys.stderr)
        sys.exit(1)
    TARGETS[name] = {**_generic_row(name), **STORE_DEFAULTS.get(name, {})}


_add_generic()


        # `"libs": None` means "whatever this tree would translate for that srcdir",
        # which the Makefile already decides — the .so files that are really ELF, minus
# the ones we REPLACE (libOVRPlugin, libovrplatformloader, libvrapi) and the
# ones that are not part of the application (libfrda, libscript). Asking it
# rather than restating it keeps one answer: a second copy of those rules here
# would drift on the next guest, and the failure mode is a bundle missing a
# library, which on device is a dlopen that cannot fall back to anything.
# 
# It is a hard failure rather than a fallback to a pinned list, for the same
# reason: a quietly incomplete bundle is worse than a build that stops.
def libs_for(srcdir):
    import subprocess, os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = subprocess.run(["make", "-s", "guestlibs-list", f"LIBS={srcdir}"],
                         cwd=root, capture_output=True, text=True)
    libs = out.stdout.strip()
    if out.returncode != 0 or not libs:
        print(f"!! could not discover the guest libraries in {srcdir!r} "
              f"(make guestlibs-list said: {out.stderr.strip() or 'nothing'})",
              file=sys.stderr)
        sys.exit(1)
    return libs


# A bundle id is GLOBAL, so the one in the table is not usable as it stands.
# 
# An App ID can be registered to exactly one team, and automatic signing tries to
# register one on the first build. So the second person to build this tree does
# not get a warning, they get a signing failure that names neither the cause nor
# the fix:
# 
#   error: Failed Registering Bundle Identifier: the app identifier
#         "dev.klepton.app" cannot be registered to your development team
# 
# ...and because both memory entitlements need an EXPLICIT App ID, that failure
# takes them down with it — which is the thing that stopped the
# loading-transition kills on device (see ENTITLEMENTS in gen_xcodeproj.py). One
# person's registration is enough to block everyone else's build of the same
# commit.
# 
# So the account building it supplies the front of the id. $USER is the right key
# because it is per-machine-account, always set in a login shell, and needs no
# configuration at all to be different for different people.
# 
# The rest is DERIVED from the target name rather than stored per target:
#
#     <user>.dev.klepton.target.<target>
#
# One shape for every target, so a new one needs no id invented for it and the
# table cannot hold an id that disagrees with the key above it. (The three that
# used to be there did exactly that — `dev.klepton.app`, `dev.klepton.steamlink`
# and `dev.klepton.target.superhot` were three conventions for three targets.)
# 
# Sanitised, because a bundle id may hold only alphanumerics, hyphens and
# periods: anything else becomes a hyphen, and a scope that sanitises to nothing
# (or an unset USER, as in some CI) leaves the id unscoped rather than emitting
# a leading dot. KLEPTON_BUNDLE_SCOPE overrides the key; KLEPTON_BUNDLE_ID still
# overrides the whole id, everywhere, and is how an ALREADY-INSTALLED app's
# identity is kept — changing this orphans the container the assets were staged
# into, so the first build after it re-stages (run.sh's stamp is keyed on the id,
# which is what makes that automatic rather than a silent empty container).
# A Linux target's frameworks, as mklxguest.sh last built them. Read from the
# list it writes rather than derived here, because the names carry a content
# fingerprint (runtime/linux/kl_lx_name.h) and only the translator knows it.
# Empty before the first mklxguest.sh run, which is what lets run.sh resolve the
# target in order to run it.
def lx_libs(name):
    import os
    p = os.path.join(os.path.dirname(os.path.abspath(__file__)), "Frameworks", name, "LIBS")
    try:
        return " ".join(open(p).read().split())
    except OSError:
        return ""


BUNDLE_PREFIX = "dev.klepton.target"


def bundle_for(name):
    import os, re
    scope = os.environ.get("KLEPTON_BUNDLE_SCOPE", os.environ.get("USER", ""))
    scope = re.sub(r"[^A-Za-z0-9-]", "-", scope).strip("-").lower()
    ident = re.sub(r"[^A-Za-z0-9.-]", "-", f"{BUNDLE_PREFIX}.{name}")
    return f"{scope}.{ident}" if scope else ident


# The name the headset shows under the icon (CFBundleDisplayName), for EVERY
# target — the rows above and the generic one alike. A parallel copy of a game
# beside the one already installed (a test build next to the player's own, with
# KLEPTON_BUNDLE_ID set so it is a second app rather than a replacement) needs a
# name of its own, or the two icons cannot be told apart.
#
#     KLEPTON_DISPLAY_OVERRIDE=<name>   the whole name, over the row's (and over
#                                       KLEPTON_GENERIC_DISPLAY / the launcher's)
#     KLEPTON_DISPLAY_SUFFIX=<text>     appended to whatever name that leaves,
#                                       e.g. " (Test)" -> "Beat Saber (Test)"
#
# KLEPTON_DISPLAY_NAME is the older spelling of the override (gen_xcodeproj.py
# read it on its own) and still works; KLEPTON_DISPLAY_OVERRIDE wins over it.
# Unset or empty, both leave the name exactly as the row has it. Non-printable
# characters are dropped, as for the generic display; a name that strips to
# nothing falls back to `base`.
def display_for(base):
    import os
    override = (os.environ.get("KLEPTON_DISPLAY_OVERRIDE", "")
                or os.environ.get("KLEPTON_DISPLAY_NAME", ""))
    suffix = os.environ.get("KLEPTON_DISPLAY_SUFFIX", "")
    if not override and not suffix:
        return base
    clean = lambda v: "".join(c for c in v if c.isprintable())
    name = clean(override).strip() or base
    return (name + clean(suffix)).strip() or base


def _follow_unpacked_build(t):
    """The data panel's OBB name and store ids are a property of the BUILD.

    A row states them for the build it was written against (Beat Saber 1.40.8:
    main.1716...obb). The same row is built from a newer APK once the store
    carries one — the installer unpacks whatever the player's recipe names into
    <root>/<tree>/ — and the game then opens main.<its own version code>...obb.
    Left as written, the app would look for 1716's file beside a 1.45 build,
    find it missing and offer to download 1.40's data.

    So where the unpacked APK is another build than the row's, the name follows
    the APK and the ids are dropped: they name the other build's files, and no
    download is better than the wrong one. Nothing changes for the build the
    row was written against, or while nothing is unpacked.
    """
    import os, re
    m = re.match(r"main\.(\d+)\.(.+)\.obb$", t.get("obb_file") or "")
    if not m:
        return
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        with open(os.path.join(root, t["tree"], "AndroidManifest.xml"), errors="replace") as f:
            head = f.read(4000)
    except OSError:
        return
    tag = re.search(r"<manifest[^>]*>", head)
    code = re.search(r'\sandroid:versionCode="(\d+)"', tag.group(0)) if tag else None
    if code and code.group(1) != m.group(1):
        t["obb_file"] = f"main.{code.group(1)}.{m.group(2)}.obb"
        t["meta_apk_id"] = t["meta_obb_id"] = ""


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    t["bundle"] = bundle_for(name)
    t["display"] = display_for(t["display"])
    # The DEFAULTS only: the settings panel, KL_HAND_MATTING and KL_VRR still
    # decide. Both are what a player wants from a game they just installed:
    #
    #  - hands hidden: nearly every guest draws its own hands or controllers
    #    where the player's are, and the system's cut-out of the real arms on
    #    top of them is a second pair in the same place.
    #  - foveation on: it is the cheaper picture wherever it applies (an OpenGL
    #    guest's passes; see KleptonRendering) and a stored preference with no
    #    effect elsewhere. A target that must not have it says False.
    t.setdefault("hand_matting", False)
    t.setdefault("render_scale", 1.0)
    t.setdefault("video", False)
    t.setdefault("autoboot", True)
    t.setdefault("foveation", True)
    # A number a target raises to drop the stored foveation choice once, when
    # its default changes and the players it already has should get the new
    # one. 0: a stored choice is never touched.
    t.setdefault("foveation_reset", 0)
    # Test switches (KleptonTuning.swift, Advanced > Test) this game starts
    # with: comma-separated variable names, added to the installer's list
    # (KLEPTON_TEST_ON -> KLTestOn). A player still turns each off in the
    # start window, and that choice stays. "" for every game not named here.
    t.setdefault("test_on", "")
    # Edge smoothing for a Vulkan guest that multisamples by itself with fewer
    # samples (KL_VK_FORCE_SAMPLES): 0 leaves the game's own count, 4 raises it.
    # 0 unless a game is named below, and it is named once it has been played
    # that way: a multisampled target is real memory here, and a game near the
    # process limit is ended at its first load by the difference (AC Nexus at
    # 4x was, which is why it is told the device offers two).
    t.setdefault("vk_msaa", 0)
    # Foveated rendering for a Vulkan guest (KL_FOVEATION, runtime/gfx/kl_fove.m):
    # "off", "low", "medium" or "high". Off unless a game is named above, and it
    # is named once it has been looked at on a headset: the edge of the picture
    # is drawn visibly coarser, and whether that is worth the frame time is a
    # judgement per game. Half-Life: Alyx is not on this path (kl_mtl_foveate).
    t.setdefault("vk_foveation", "off")
    # Raised when a target's picture defaults (render_scale, vk_foveation) change
    # and installed copies should take the new ones: a copy drops its stored
    # resolution and Vulkan foveation level once per number (KLPictureReset).
    t.setdefault("picture_reset", 0)
    # The start window's "Frame rate" before a player chooses one: 100 (the
    # mixed space, every game's start) or 120 (the full space). A choice made
    # in the start window stays; a mixed-reality game is shown mixed anyway.
    t.setdefault("max_rate", 100)
    # The start window's "Frame generation" before a player chooses one: "off",
    # "ahead" (a picture made where the game has none) or "between" (the middle
    # between the game's last two, a display frame later). Half-Life: Alyx is
    # "between": below its 50 most of the time, and its owner found it
    # "fühlt sich auch besser an als ohne" (2026-10-28). A choice made in the
    # start window stays; a copy that never chose takes a changed default.
    t.setdefault("framegen", "between" if name == "alyx" else "off")
    # How finely the compositor puts the finished picture on the display
    # (KL_CP_QUALITY): "light" 0.40, "standard" 0.52, "full" 1.0. Light for
    # everything whose picture is capped below what the display asks for even
    # then (4633 px across at 0.40, against caps of 4000 and less) — for those
    # it only makes the compositor's own pass cheaper. The games that render at
    # the display's size get a smaller picture with it, so they stay at full,
    # and so does Steam Link, whose picture is as large as the computer sends it.
    t.setdefault("output", "full" if name in ("beatsaber", "beatsaber140", "doom3quest")
                                     or name.startswith("steamlink") else "light")
    # How a plain gamepad's four face buttons are laid over Touch A/B/X/Y where
    # the default (cross=a, circle=b, square=x, triangle=y) puts an action under
    # the wrong thumb for this game. "" keeps the default.
    t.setdefault("gamepad_map", "")
    t.setdefault("generic", False)
    # The Steam app id whose store artwork stands in for an icon when the
    # installer left none (gen_xcodeproj.py, steam_art.py). "" for the rest:
    # their icon comes out of the APK.
    t.setdefault("steam_app", "")
    # Absent means "no data panel for this target" (obb_file) or "no download"
    # (the ids); a Unity target that wants the panel states obb_file, even "".
    for k in ("obb_file", "meta_apk_id", "meta_obb_id"):
        t.setdefault(k, "")
    t["data_panel"] = "obb_file" in TARGETS[name]
    _follow_unpacked_build(t)
    if t["kind"] == "linux":
        t["libs"] = lx_libs(name)
    elif t["libs"] is None:
        t["libs"] = libs_for(t["srcdir"])
    return t


# The runtime's copy of the table, as an X-macro list. Only the fields C needs
# are here: the visionOS product/bundle/display are the BUILD's business and the
# app already knows which one it is by the time it runs, and `libs` is discovered
# per build rather than being a property anything at runtime should believe.
# 
# The DEFAULT is emitted too, so `build/m_boot` with no argument and an app built
# with no -DKL_TARGET_DEFAULT agree about which guest that means.
def c_table():
    out = [
        "// GENERATED by visionos/targets.py — do not edit. Regenerate: make targets",
        "//",
        "// The target table, as the runtime sees it. visionos/targets.py is the",
        "// authority (it also carries the bundle id, product and display name, which",
        "// only the build needs); this is its C half, generated so that the two",
        "// cannot disagree about which tree, which APK or which boot sequence a",
        "// target means. See runtime/kl_target.h for the struct these expand into.",
        "",
        "// KL_TARGET_ROW(name, tree, apk, assets, libdir, entry_lib, userdata, obb, kind)",
        "#ifndef KL_TARGET_ROW",
        "#error \"include this through runtime/kl_target.c\"",
        "#endif",
        "",
    ]
    for name in sorted(TARGETS):
        t = TARGETS[name]
        kind = "KL_GUEST_" + t["kind"].upper()
        # `userdata` defaults to the row name — one profile per target is the
        # rule, and a row only states it to KEEP a directory a previous naming
        # created (steamlink-vr).
        out.append('KL_TARGET_ROW("%s", "%s", "%s", "%s", "%s", "%s", "%s", "%s", %s)'
                   % (name, t["tree"], t["apk"], t["assets"], t["srcdir"],
                      t["entry"], t.get("userdata", name), t["obb"], kind))
    out += ["", '#define KL_TARGET_DEFAULT_NAME "%s"' % DEFAULT, ""]
    return "\n".join(out)


def main(argv):
    if len(argv) > 1 and argv[1] == "--list":
        for k in sorted(TARGETS):
            print(k)
        return 0
    if len(argv) > 1 and argv[1] == "--c-table":
        print(c_table())
        return 0
    name = argv[1] if len(argv) > 1 and argv[1] else DEFAULT
    t = resolve(name)
    if len(argv) > 2:
        if argv[2] not in t:
            print(f"!! no field {argv[2]!r}", file=sys.stderr)
            return 1
        print(t[argv[2]])
        return 0
# Shell-sourceable. shlex.quote so a value with spaces (`libs`) or a shell
# metacharacter (an apostrophe in a display name like "Assassin's Creed Nexus")
# survives `eval` intact instead of splitting the assignment or leaving an
# unbalanced quote.
    for k, v in t.items():
        print(f"KLT_{k.upper()}={shlex.quote(str(v))}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
