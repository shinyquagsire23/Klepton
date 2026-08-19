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
        "display": "Klepton",
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
        "display": "Klepton SUPERHOT",
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
        "display": "Klepton BONELAB",
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
        "display": "Klepton VRChat",
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
        "display": "Klepton Open Brush",
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
        "display": "Klepton RE4",
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
        "display": "Klepton JK Academy",
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
        "display": "Klepton JK Outcast",
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
        "display": "Klepton Steam Link",
    },
}

DEFAULT = "beatsaber"


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
BUNDLE_PREFIX = "dev.klepton.target"


def bundle_for(name):
    import os, re
    scope = os.environ.get("KLEPTON_BUNDLE_SCOPE", os.environ.get("USER", ""))
    scope = re.sub(r"[^A-Za-z0-9-]", "-", scope).strip("-").lower()
    ident = re.sub(r"[^A-Za-z0-9.-]", "-", f"{BUNDLE_PREFIX}.{name}")
    return f"{scope}.{ident}" if scope else ident


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    t["bundle"] = bundle_for(name)
    if t["libs"] is None:
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
# Shell-sourceable. Quoted because `libs` has spaces in it and an unquoted
# eval would turn one assignment into a command.
    for k, v in t.items():
        print(f"KLT_{k.upper()}='{v}'")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
