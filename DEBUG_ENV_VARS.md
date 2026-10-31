# Environment variables

Every environment variable the tooling reads — the C/Swift sources, the mains,
and the wrapper scripts — found by grepping for `getenv`/`kl_env_*`/
`ProcessInfo` and reading each usage site. Grouped by area. The source is the
authority; the orientation manual keeps no knob table of its own and points here. Unless an entry
says otherwise, presence of the variable (any value, even empty-but-set for the
C `getenv` checks) turns the knob on, the default is off, and the reader is the
`m_boot` binary via the runtime it links.

## Boot / recon (`mains/m_boot.c`, the `m_boot` binary)

- `KL_RECON_CHILD=1` — marks the re-exec'd recon child, so `main` skips the
  guard test and the fork and goes straight to the guest run. Set by m_boot on
  itself across the `execl`; not meant to be set by hand.
- `KL_NOFORK=1` — run the recon in-process instead of in the re-exec'd child.
  Required under lldb (macOS lldb follows neither fork nor exec), at the cost
  of the AGX abort being masked by the debugger's signal traffic.
- `KL_SKIP_GUARD_TEST=1` — skip the DRM-guard self-test, which misreads under
  a debugger (the forked child's abort is intercepted and never reads as
  SIGABRT to `waitpid`). The guard in `kl_ovrplat.c` itself is unaffected.
- `KL_FAULT_WAIT=1` — park in m_boot's fault handler on SIGSEGV/SIGBUS/
  SIGABRT/SIGALRM and print the pid, for post-mortem `lldb -p` attach.
- `KL_LIFECYCLE=1` — drive the UnityPlayer lifecycle sequence
  (nativeRecreateGfxState, nativeResume, nativeRender) instead of stopping
  after initJni.
- `KL_FRAMES=N` — after the lifecycle, pump N `nativeRender` calls, ticking
  the Choreographer before each frame and draining the posted-task queue
  between them. One frame is engine setup and draws nothing. Default 0.
- `KL_ALARM=<s>` — the watchdog around each lifecycle call (default 20s in
  `m_boot`) and around the whole frame pump (default 60s). The visionOS app
  (`kl_app.c`) uses one value for every phase and defaults it to 120s, because a
  device run pays staging and first-frame costs the host does not. Widen it when
  the question is what the guest is waiting on.
- `KL_PERMISSIVE=1` — unimplemented JNI/GL/OVRPlugin/OVRPlatform calls return
  0 instead of aborting. Read by `kl_jni.c` (via `kl_jni_set_permissive`),
  `kl_egl.c`, `kl_ovrp.c`, `kl_ovrplat.c`. Scouting only — the guest carries
  on with answers we invented. Does not apply to the DRM entitlement guard.
- `KL_JNI_DROP_ANALYTICS=0` — (`runtime/kl_jni.c`) restore the abort for
  unimplemented calls into third-party analytics SDKs (`com/gameanalytics/…`, `com/amplitude/…`).
  Default 1: such calls are dropped and answer zero/NULL/void, since there is
  no host to report to. Max Mustard aborted on `GameAnalytics.addErrorEvent`.
  Not this switch, and not a drop: for BattleGroupVR2 (`x9386827918099701`)
  alone, the Firebase SDK's start (`runtime/jni/kl_jni_firebase.c`) and
  `VivoxNative.init(Context)` (`runtime/jni/kl_jni_vivox.c`) are answered with
  the exception Java throws for a backend that is not there —
  `UnsupportedOperationException`, `IllegalStateException` for
  `FirebaseApp.getInstance` — so the SDK reports it to the game instead of the
  process stopping on the name. No Firebase feature and no voice chat; every
  other Firebase or Vivox call still aborts by name.
- `KL_JNI_DROP_BHAPTICS=0|1` — (`runtime/kl_jni.c`) drop the calls into
  `com/bhaptics/…` that the bHaptics table (`runtime/jni/kl_jni_haptics.c`)
  has no row for: they answer zero/NULL/void, as a vest that is not there, and
  the first 16 are named in the log. The table is one version of the plugin's
  wrapper; a game with another version otherwise stops at the first entry it
  lacks. Default 1 for `x2582932495064035` (Real VR Fishing, whose files nobody
  here has to read the missing rows out of), else 0 — the stop by name.
- `KL_JNI_DROP_SENTRY=0|1` — (`runtime/kl_jni.c`) drop the calls into
  `io/sentry/…` and its field reads and writes, the Sentry crash reporter's
  Java SDK: they answer zero/NULL/void (writes are not kept), as an SDK that
  was never started, and the first 16 are named in the log. Default 1 for
  `x5245041552210029` (Arizona Sunshine 2, whose sentry-unity asks for
  `Sentry.getCurrentHub` after its splash), else 0 — the stop by name.
- `KL_MP14_TRACE=1` — (`runtime/jni/kl_jni_ue4.c`) name the first few hundred
  calls a guest makes on UE4's `MediaPlayer14` (position, frame fetch, playing,
  complete, resolution change, start/pause/seek), each with the thread it came
  from. The order of an engine's calls on its player is its playback logic;
  this is how the GLES route's standing clock was found.
- `KL_PLAT_USER=0|1` — (`runtime/xr/kl_ovrplat.c`) hand out a synthetic
  OFFLINE user for `ovr_GetLoggedInUserID` / `ovr_User_GetLoggedInUser`.
  "Who is playing", not an ownership answer. Default on for the titles listed
  in `klplat_user_listed` — the ones that quit or wait without a user,
  BattleGroupVR2 (`x9386827918099701`), whose start waits for player data,
  and Cubism (`x2264524423619421`) among them — and off for every other.
- `KL_JNI_DEFER_STRING_DELETE=0|1` — (`runtime/kl_jni.c`) a String the guest
  `DeleteLocalRef`s is retired by age (the frameless-string ring) instead of at
  once, so a name it stored in a String[] and then deleted is still readable
  where the array is read. Default on for The Walking Dead: Saints & Sinners -
  Chapter 2 (`x5190288877703616`), whose permission request arrived with every
  name retired; off for every other.
- `KL_UE4_PERMISSION_EXPORT=0|1` — (`runtime/jni/kl_jni_window.c`) deliver
  `PermissionHelper.acquirePermissions`' answer through the guest's exported
  `Java_com_google_vr_sdk_samples_permission_PermissionHelper_onAcquirePermissions`
  when it registered none (UE4 4.27's AndroidPermission). Default on for
  `x5190288877703616`, whose loading waits on that answer; off for every other.
- `KL_PLAT_IAP_EMPTY=0|1` — (`runtime/xr/kl_ovrplat.c`) answer
  `ovr_IAP_GetProductsBySKU` and `ovr_IAP_GetViewerPurchases` with an EMPTY
  list instead of refusing the request. For a title whose start waits for the
  answer; nothing is reported as for sale or as bought. Default on for Tales
  from the Galaxy's Edge (`x3484270071659289`), Shave & Stuff
  (`x7532973430077887`), and Beat Saber 1.40 (`beatsaber140`). Beat Saber asks
  for its purchases when its add-on files do not cover its songs (an
  installation without them); refused, the health warning waits about 5 s for
  "owned songs" after OK, with the warning scene still up, before it loads the
  menu.
- `KL_PLAT_IAP_CONFIRMED=0|1` — (`runtime/xr/kl_ovrplat.c`) answer
  `ovr_IAP_GetViewerPurchases` with the SKUs in `klepton-entitlements.txt`,
  as they are — the purchases the store confirmed, written on the Mac. For a
  title that unlocks its add-ons on the purchase list alone; with no file the
  list is empty. Default on for Maestro (`x6963344583787383`) alone.
- `KL_PLAT_OBB_ASSETS=0|1` — (`runtime/xr/kl_ovrplat.c`) report the loose
  container files in the OBB directory (`*.pak`, `*.utoc`, `*.ucas`) to
  `ovr_AssetFile_GetList` as installed asset files. They are the store's asset
  files for such a title, on disk because the store delivered them; a file
  that is not there is not listed. Default on for `x3484270071659289` and
  Maestro (`x6963344583787383`).
- `KL_GL_MSAA_MV=0|1` — (`runtime/gfx/kl_glfb.c`, `kl_glfb_multiview_ms`) make
  a guest's multisampled multiview attachment
  (`glFramebufferTextureMultisampleMultiviewOVR`) the real thing: both views are
  drawn into a multisampled array and resolved into the eye texture's slices
  when the pass ends. Needs the ANGLE of `angle-patches/klepton.patch` from this
  change on (the Metal backend's layered multisampled pass); an older ANGLE
  refuses the call, which the first attachment notices, says once and falls back
  to plain multiview. Default off: the attachment is single-sampled as before.
  In the app the start window's "Edge smoothing" setting (off, 2x, 4x;
  `kl_glfb_set_msaa_default`; shown for Beat Saber's two rows, off unless
  chosen) decides where this switch is not set. On the owner's headset 4x at
  the display's full resolution (foveated) dropped a song to 60 Hz halfway.
  The setting is offered to two guests (`klfb_msaa_guest`): Beat Saber from
  1.45 on and Maestro, which ask for the multisampled attachment themselves.
  DreamSpace (`x9449316705178989`) asks for it too and has it on by default
  without the setting: its foliage is cut out by alpha-to-coverage, which
  single-sampled leaves brown patches.
  `KL_GL_MSAA_PLAIN=1` (with `KL_GL_MSAA_MV=1`) also upgrades a guest's plain
  multiview attachments, colour and depth-stencil — what Doom 3 had in 1.1.7,
  where its picture flickered black now and then.
- `KL_FULL=1` / `KL_MIXED_BLACK=0` — (`KleptonApp.swift`, `Immersive`) every
  game opens a mixed immersive space (the panel runs at up to 100 Hz there and
  stays; a full space runs at up to 120 and visionOS halves that to 60 once the
  game cannot keep up), and the compositor draws black behind every frame that
  has no eye picture, so the room does not show. `KL_FULL=1` opens the full
  space instead; `KL_MIXED_BLACK=0` leaves the room visible in the mixed one.
  The full space is also the start window's "Frame rate: up to 120" setting
  (`KleptonRendering.maxRate`, default up to 100), offered to every game but
  the mixed-reality ones (TRIANGLE STRATEGY, Demeo); `KL_FULL` overrides it.
- `KL_OVRP_PASSTHROUGH=0|1` — (`kl_ovrp.c`, "Insight Passthrough") tell an
  OVRPlugin guest that passthrough exists: capability flags, init, its state,
  the layer style and the surface meshes all succeed. Nothing is drawn for the
  passthrough layer; while one is in the guest's submit list the eye picture is
  no longer forced opaque — its alpha says where the room shows — and the black
  of `KL_MIXED_BLACK` is left out. Default 1 for TRIANGLE STRATEGY
  (`x5340130476095884`), Demeo (`x3634830803298285`), Richie's Plank Experience
  (`x1642239225880682`), Synth Riders (`x2436558143118760`), LES MILLS BODYCOMBAT
  (`x4015163475201433`), DreamSpace (`x9449316705178989`), Cubism
  (`x2264524423619421`) and Spatial Zombies (`x24151641901194680`), 0 for
  everyone else,
  who are told "unsupported" as before. A layer filed through
  `ovrp_EnqueueSubmitLayer2` (Unity) counts like one in `ovrp_EndFrame4`'s
  list; for a guest on Meta's OpenXR backend (frames end in `xrEndFrame`,
  never in `ovrp_EndFrame4`) it is counted at `xrEndFrame`
  (`kl_ovrp_openxr_frame_end`), the eye alpha read as coverage unless
  `KL_OVRP_PT_ALPHA` says otherwise. Log lines start `[ovrp] passthrough:`.
- `KL_OVRP_PT_LUT=0|1` — (`kl_ovrp.c`) accept passthrough colour look-up
  tables (`ovrp_Create/Update/DestroyPassthroughColorLut`, and a
  MaxColorLutResolution of 64 in the capabilities) without applying them:
  visionOS's passthrough takes no colour table, so the room keeps its own
  colours. Only where `KL_OVRP_PASSTHROUGH` is on. Default 1 for DreamSpace
  (`x9449316705178989`), whose themes refuse to load without one, else 0
  (refused, as before).
- `KL_OVRP_SCENE_ROOM=0|1` — (`kl_ovrp.c`) offer an emulated room (a box
  around the head, or visionOS's planes with `KL_OVRP_SCENE_PLANES=1`) to the
  ovrp_* scene calls and, for a guest on Meta's OpenXR backend, to the OpenXR
  scene entry points (`xrDiscoverSpacesMETA`, `xrGetSpaceRoomLayoutFB`, the
  FB spatial-entity component queries; `kl_openxr.c`), which are "not served"
  where it is off. Default 1 for Synth Riders (`x2436558143118760`),
  DreamSpace (`x9449316705178989`, Meta's MR Utility Kit) and Spatial Zombies
  (`x24151641901194680`, the same kit on Unity's OpenXR plugin), else 0.
- `KL_PLAT_USER_NAME=<name>` / `KL_PLAT_USER_ID=<number>` — (`kl_ovrplat.c`)
  the offline user's name and id (`KL_PLAT_USER`). The name is what the start
  window's "Player name" holds: the display name as typed, the Oculus ID the
  same with only letters, digits, `.`, `_` and `-` kept; unset, "Klepton
  Player" / "kleptonplayer". The id is 1000000000000042 for every player unless
  given; for a game that keys an account of its own on it (Demeo, Walkabout
  Mini Golf) the app makes one random number per installation and passes it
  here.
- `KL_STEAM_ID=<64-bit id>` / `KL_STEAM_NAME=<name>` / `KL_STEAM_APPID=<id>` /
  `KL_STEAM_LANGUAGE=<name>` — (`kl_steamapi.c`, the stand-in for Valve's
  `libsteam_api.so` in the Android build of a game that Steam ships). The id is
  what `ISteamUser::GetSteamID` answers; only an individual account in the
  public universe is taken, anything else leaves the fixed
  76561197960265729. The app passes one per installation for the same games as
  `KL_PLAT_USER_ID`, and for the same reason: Walkabout signs in to its own
  backend with "<steam id>Steam". The name is the persona name (default
  "Player"; the start window's "Player name"), the app id is the game's by
  target, the language is Steam's word for the device's (`german`, `english`,
  …). Add-on ownership comes from `klepton-entitlements.txt` only (`app_id` and
  one `sku <Steam app id>` per add-on); every function without an answer of its
  own names itself once (`[steam] …: not implemented — answering 0`) and the
  end-of-run report lists them.
- `KL_OVRP_UNANSWERED=unsupported|abort` — what an OVRPlugin entry point with no
  answer in `kl_ovrp.c` does when the guest calls it: end the run with its name
  (`abort`, the default) or say `UNSUPPORTED` and go on, named once in the log
  (`[ovrp] no answer for …`). Default `unsupported` for Demeo, which is told its
  own plugin version 1.109.0 (`KL_OVRP_VERSION`) so that its C# asks for
  passthrough at all, and can then reach some two hundred entry points nothing
  here has seen called.

  The entry points of Meta's OpenXR backend (OVRPlugin 1.203.0: native OpenXR
  handles and `xrGetInstanceProcAddr`, `ovrp_RegisterOpenXREventHandler` and
  its forwarding from `xrPollEvent`, `ovrp_GetAppSpace`,
  `ovrp_GetUnityInterfaces`, `ovrp_GetRuntimeName`, the telemetry setters and
  the shutdown-handler pair) have answers for every title, not behind a
  version or a knob: the plugin version here is the number this runtime TELLS
  the guest (`KL_OVRP_VERSION`, per target), not one read from the APK, and
  native code such as Meta's MR Utility Kit calls them whatever the C# was
  told. Without an answer each of them ended the run (or said UNSUPPORTED
  where this switch says so); the answers are the real plugin's, and the
  OpenXR handles fail with NotInitialized for a guest on the OVRPlugin path,
  which has no OpenXR instance or session here.
- `KL_OVRP_PT_ALPHA=opaque|coverage|inverse` — what the eye picture's alpha
  means while a passthrough layer is submitted: `coverage` 1 = the game's
  picture, `inverse` 0 = the game's picture, `opaque` ignore it. Unset, it is
  `inverse` when the eye submit carries flag 0x10 (InverseAlpha, as remembered
  from the public header) and `coverage` otherwise; the log line names the
  flags it read. If the game's picture is missing and the room shows where the
  game should be, the other one is right.
- `KL_GL_MSAA_MV_SAMPLES=2|4` — override the sample count the guest asked for
  (Beat Saber asks for 4) and the app's setting.
- `KL_ANGLE_MSMV_MEMORYLESS=0` — read by ANGLE itself (`TextureMtl.mm`): keep the
  samples of a multisampled multiview target in memory instead of memoryless.
  Memoryless is the default; a pass reopened over its own earlier content then
  gets it back by one draw per view. Kept samples cost four times the eye
  textures (gigabytes at headset sizes) and exist for comparison. ANGLE counts
  the reopenings in the log: `[angle] layered multisampled pass: opened N
  times, M of them over kept content`.
- `KL_GL_MSRTT=0|1` — (`runtime/gfx/kl_egl.c`) show the guest
  `GL_OVR_multiview`, `GL_OVR_multiview_multisampled_render_to_texture` and
  `GL_EXT_multisampled_render_to_texture` together, and attach its multisampled
  multiview target single-sampled. For a Unity 6 title on Unity's OpenXR plugin
  that renders SinglePassInstanced with anti-aliasing: without the first name
  its eye framebuffer is incomplete, with the first two but not the third its
  render pass never begins ("Missing resolve surface"). Default on for Beat
  Saber from 1.45 on (`kl_guest_beatsaber_openxr`: the row is `beatsaber140` or
  `beatsaber` and the APK ships `libUnityOpenXR.so`), off for every other guest
  and for Beat Saber 1.40. The same test turns on that build's offline user,
  its OVRPlugin version (1.106.0), its eye-size cap (4000) and `.full`. Also on
  by default for DreamSpace (`x9449316705178989`, Unity 6 + OpenXR, the same
  case), which is told OVRPlugin 1.203.0 and gets the GLES 3.0 capability
  profile (as Space Channel does) so Unity does not skin on the GPU. On by
  default for Fluid (`x6946182742093178`, Unity 6000.3 + OpenXR) as well, for
  Sweet Surrender (`x4723352327707414`, Unity 6000.1 + OpenXR,
  SinglePassMultiview with 4x anti-aliasing: every frame "Framebuffer is not
  complete", a black picture with its music playing), and
  for iB Cricket (`x5437449896288644`, Unity 6000.0 + the Oculus XR plugin),
  whose eye framebuffers were incomplete on every frame (a plain pink screen).
  On by default for Spatial Zombies (`x24151641901194680`, Unity 6000.6 +
  OpenXR, SinglePassInstanced) as well: without the names Unity looked for
  `STEREO_INSTANCING_ON` shader variants the game does not ship.
- `KL_NET_NO_META=0|1` — (`runtime/libc/kl_shim.c`) names under oculus.com,
  oculuscdn.com, facebook.com, facebook.net, fbcdn.net and meta.com do not
  resolve (`EAI_NONAME`, as on a headset without a network); every other host
  resolves as before. For a guest that holds the synthetic offline user and
  would send its made-up token to Meta's servers with every analytics,
  feature-flag and leaderboard request. Default on for Beat Saber from 1.45 on
  (`kl_guest_beatsaber_openxr`), off for every other guest. The first four
  refusals are logged by name.
- `KL_VROS_SDK=<n>` — (`runtime/jni/kl_jni_android.c`) what
  `vros.os.VrosBuild.getSdkVersion()` answers, the Horizon OS release as a
  number. Default 76.
- `KL_HZ_ENTITLEMENT=0` — (`runtime/jni/kl_jni_net.c`) leave the Horizon
  Platform request `entitlements` / `get_is_viewer_entitled` without a reply,
  as every other Horizon request is. Default 1: that one request — the
  application's own licence check, the Horizon-SDK form of
  `ovr_Entitlement_GetIsViewerEntitled` in `kl_ovrplat.c` — is answered with
  success. Add-on, purchase and every other ownership request never is.
  HARD BULLET waited for it behind a black picture.
- `KL_DYLIB_DIR=<dir>` — (`runtime/kl_image.c`) prefer `<dir>/<name>.dylib`
  (klepton-ld output) over the ELF when loading each guest library — how
  `make bootdylib*` runs the translated images. Unset, the runtime ELF loader
  maps the `.so` directly.

## GL / null driver (`runtime/gfx/kl_egl.c`)

- `KL_DUMP_SHADERS=<dir>` — after the frame pump, write every captured
  `glShaderSource` text into `<dir>`. Read by m_boot; the dump runs in
  `kl_egl.c`. The only place Unity's GLSL ES exists in plain text.
- `KL_DUMP_TEXTURES=<dir>` — write every uncompressed 8-bit
  `glTexSubImage2D` upload as a PNG under `<dir>`. Armed before the guest runs
  (uploads happen all through init). Compressed uploads (ETC2/ASTC) are
  skipped rather than guessed at.

- `KL_GL_ES31_LIMITS=1` — answer the ES 3.1 storage limits (shader storage
  blocks and bindings, image units, compute invocations) with the spec minimums
  for ANY target, as Sniper Elite VR gets them by name. An experiment: on its
  own it makes Unity send compute kernels to GL, where
  `glCreateShader(GL_COMPUTE_SHADER)` fails on the ES 3.0 ANGLE (Beat Saber 1.45
  then throws `IndexOutOfRangeException: Invalid kernelIndex (0)`, which is
  worse than its kernel being refused). `KL_GLFB_COMPUTE_TF` answers the same
  limits and serves the kernel.

## ANGLE reference renderer (`runtime/gfx/kl_glfb.c`)

All of these require `KL_GLFB=1` to mean anything; without it the null driver
answers GL and kl_glfb never initializes.

- `KL_GLFB=1` — enable the one-eye reference renderer on ANGLE/Metal.
  Host-only; it exists to produce the known-good frame a real backend gets
  diffed against.
- `KL_EGL_EXTENSIONS=<space-separated list>` — override what
  `eglQueryString(EGL_EXTENSIONS)` answers. The default names the four we
  actually implement (`EGL_KHR_image_base`,
  `EGL_ANDROID_image_native_buffer`, `EGL_ANDROID_get_native_client_buffer`,
  and `EGL_KHR_create_context`, without which SDL2 refuses to request a GLES 3
  context at all);
  set it EMPTY to restore the pre-VRChat behaviour. Worth having as an A/B
  because a guest may branch on a name here — and because an empty list, though
  legal, is what no Android driver actually ships: AVPro Video walks this string
  with `strtok` in a bottom-tested loop and `strlen(NULL)`s the first token

- `KL_EGL_TRACE=1` — log every EGL entry point the guest actually REACHES, in
  order, with the interesting arguments. A census, not a diagnostic: it says
  nothing about what we answered. It exists because every EGL symbol binds at
  LOAD time (libEGL.so is a DT_NEEDED of libunity), so "resolved" says nothing
  about "called" — and a guest that fails a graphics capability check *before*
  it touches EGL is otherwise indistinguishable from one whose call we answered
  wrongly. That is how BONELAB's renderer was identified: Beat Saber's per-API
  probe runs `eglChooseConfig` -> `eglCreateContext` (GLES 3) ->
  `eglDestroyContext`, and BONELAB's stops after `eglInitialize` because the
  only API in its list is Vulkan.
- `KL_ANGLE_DIR=<dir>` — where to load `libEGL.dylib`/`libGLESv2.dylib` from.
  Default: `vendor/out/Debug` when its libEGL is present, else Google Chrome's
  framework Libraries dir. Also read by spikes s09, s10, s11, s12, s13.
- `KL_ANGLE_BACKEND=gl` — select ANGLE's OpenGL backend instead of Metal. Only
  the exact string `gl` switches, and only spikes s09 and s10 read it: the
  runtime asks for Metal by name and has no GL path.
- `KL_GLFB_SIZE=WxH` — override the eye/pbuffer size (default 1832x1920, the
  Quest 2 per-eye size). Applied both at init and in `kl_glfb_set_size`.
- `KL_GLFB_SHARED=1` — per-thread ANGLE contexts sharing one object namespace,
  instead of the default single migrating root context. FBOs are not shared
  between contexts, so this mode renders nothing meaningful with this guest;
  it stays for the instruments built on it.
- `KL_GLFB_NOSHARE_OBJ=1` — with `KL_GLFB_SHARED`, give each thread a context
  that shares nothing. Answers whether a bug needs object sharing at all; the
  picture is meaningless.
- `KL_GLFB_PROBE=1` — run a trivial clear+readback on each guest thread as it
  takes a context, once. Answers whether the guest thread environment is
  poisoned. It clears the guest's framebuffer, so it is a probe, not a mode.
- `KL_GLFB_SELFTEST=1` — the same trivial GL on a plain host thread right
  after init. Answers "is the process poisoned".
- `KL_GLFB_DEBUG_CB=1` — register a KHR_debug callback (synchronous) on the
  root context and on each per-thread context, and print every message.
- `KL_GLFB_TRACE_TEX=1` — log every `glTexParameteri`/`glTexStorage*`/
  `glTexSubImage2D` call with arguments and a sequence number.
- `KL_GLFB_TRACE_FBO=1` — log the FBO lifecycle (`glGenFramebuffers`,
  `glBindFramebuffer`, `glFramebufferTexture2D`, `glFramebufferTextureLayer`)
  with thread ids, the attached texture's size and format, and — for binds —
  **the guest's own call site** as `<image>+0x<off>`. That last part is what
  separates two binds issued by one helper from two issued by two, which the
  arguments alone cannot say.
- `KL_GL_CENSUS=<swaps>` — the GL object census, printed every N
  `eglSwapBuffers`, at every eye-swapchain rebuild, and once in the exit
  report. Per class (textures, framebuffers, renderbuffers, buffers, vertex
  arrays, shaders, programs): how many the guest created and how many it
  deleted, plus the immutable texture and renderbuffer storage still live and
  a line naming every release of 16 MiB or more. Every gen/delete goes
  straight to ANGLE, so this is the only thing at the seam that can say
  whether the guest's GL objects come back — the first question when memory
  climbs across scene loads. Unset = off, and costs nothing.
- `KL_EYE_RELEASE=0` — do *not* release an eye texture when
  `ovrp_DestroyEyeTexture` says to (the pre-2026-08-09 behaviour, which leaked
  a whole swapchain — 250-380 MiB — per loading transition). The A/B for that
  fix; with it off, watch `KL_GL_CENSUS`'s live texture MiB climb at every
  rebuild and never come down.
- `KL_GLFB_NOSRGB=1` — allocate `GL_RGBA8` where the guest asked for
  `GL_SRGB8_ALPHA8`. A probe; a captured frame is then wrong (un-decoded
  sRGB).
- `KL_GLFB_NOSWIZZLE=1` — drop the four `GL_TEXTURE_SWIZZLE_*` parameters.
  Single-channel textures then sample as red; a probe only.
- `KL_EGL_CREATE_CONTEXT_EXT=1` — add `EGL_KHR_create_context` to the EGL
  extension string. It is the `eglCreateContext` attribute set, and SDL2 will
  not ask for a GLES 3 context on a display that does not advertise it. **Off
  by default in this fork** (upstream of here it is in the default list): with
  the name present Asgard's Wrath 2 creates its EGL contexts as GLES 3.2 where
  it took GLES 2, and Beat Saber asks for 3.2/3.0 where it asked for plain 3
  (Mac host), so it waits for a headset run. `KL_EGL_EXTENSIONS` still replaces
  the whole string.
- `KL_GL_BACKEND_EXT=1` — also name what ANGLE's own extension string has.
  The GL description then repeats a short candidate list
  (`GL_EXT_texture_format_BGRA8888`, the DXT/BC family,
  `GL_EXT_texture_filter_anisotropic` and `GL_KHR_texture_compression_astc_hdr`)
  when, and only when, ANGLE names them;
  `[glfb] backend extensions adopted:` lists what was taken. Once anything is
  adopted, `MAX_TEXTURE_MAX_ANISOTROPY` and the compressed-format pair are
  answered by ANGLE instead of the table. **Off by default in this fork**
  (upstream of here it is on, with `KL_GL_NO_BACKEND_EXT=1` as the way back):
  every verified game was verified against the core-only description, and a
  Unity guest that is shown anisotropic filtering starts using it. On by
  default for Warhammer 40,000: Battle Sister (`x3231001163584154`), whose
  lightmaps are HDR ASTC.
- `KL_GLFB_DEPTH_LINEAR=0` — rewrite a LINEAR filter on a depth texture whose
  compare mode is NONE to NEAREST (ES leaves that sample undefined and Metal
  returns 0, which reads as "everything is in shadow"); restored if the guest
  later turns compare mode on. **Off by default in this fork** (`=1`, the
  guest's write is left alone; upstream of here the rewrite is the default):
  both Beat Saber builds make that write at start, so the rewrite changes what
  a verified game samples. A switch in the start window's test group.
- `KL_GLFB_NO_INT_PRECISION=1` — do not insert `precision highp int;` into
  shaders that leave int precision to the stage default (which differs between
  vertex and fragment, so a uniform declared in both fails to link).
- `KL_GLFB_NOMIP=1` — demote every mipmapped min filter to its level-0 form,
  on textures and sampler objects. Default off; a per-title workaround for a
  guest that samples a mip level it should not (Tetris Effect's backdrop).
- `KL_GLFB_PROGRAM_BINARY=0` — refuse `glProgramBinary` outright, leaving the
  program unlinked so the guest recompiles from source. By default the binary
  is forwarded and a rejected one (LINK_STATUS 0) is named in the log and
  counted in the exit report. Default 0 for Warhammer 40,000: Battle Sister
  (`x3231001163584154`), whose shaders are rewritten at `glShaderSource`: a
  cached binary would keep an earlier toolchain's version of them.
- `KL_GLFB_ERRPROBE=1` — bracket draws and blits with `glGetError` and print
  the first 20 with the state around them (draw FBO, program, viewport;
  `glCheckFramebufferStatus` on 0x506). Consumes the pending error. Also
  enables the blit-state log line.
- `KL_GLFB_DRAW_PROBE=1` — after each of the first 12 scene-sized draws (32+
  vertices), read back the centre 64x64 and report lit-pixel count and mean
  luma. `KL_GLFB_DRAW_PROBE_N` overrides the quota (0 = unlimited),
  `KL_GLFB_DRAW_PROBE_MIN` lowers the vert floor (the frame's last draws are
  small, and one of them may be the one you're hunting).
- The draw probe also prints, per probed draw, the state it ran with: vertex
  count, program, viewport, scissor, colour mask, blend factors, depth
  function/write/attachment, cull mode and the 2D/external textures on units
  0–5. "Nothing lit" has a dozen causes that look alike from the framebuffer.
- `KL_GLFB_X_NO_DEPTH=1`, `KL_GLFB_X_NO_CULL=1` — experiments, not fixes:
  switch the depth test / face culling off before every draw. Separates "every
  fragment was rejected" from the other reasons a scene leaves nothing behind.
- `KL_GLFB_X_CLEAR_MAGENTA=1` — experiment: every `glClearColor` becomes opaque
  magenta. Does nothing for a guest that clears with `glClearBuffer*` (UE4).
- `KL_GLFB_DRAW_PROBE_SKIP=<n>` — skip the first `n` eligible draws before
  spending the quota. The other end of `_N`'s problem: a guest whose STARTUP
  renders and whose steady state does not can only be caught after the
  transition, and "unlimited from draw 0" is a full readback per draw for the
  whole run. Calibrate against the run's own `draws per framebuffer` totals.
- `KL_GLFB_BLIT_PROBE=1` — read back the source before and the destination
  after each `glBlitFramebuffer`, and put `glClear`/`glClearColor`/
  `glInvalidateFramebuffer`/renderbuffer-storage on the same timeline as the
  draws (one line each, FBO/program/viewport/texture named). The instrument
  that answered "draws lit, blit reads black". Each `glClear` also reports the
  five pieces of state that make a clear write nothing without raising
  anything — framebuffer completeness, scissor box, colour write mask,
  rasterizer discard and `DRAW_BUFFER0` — plus the clear value as GL holds it.
- `KL_GLFB_CLEAR_PROBE_N=<n>` — which `glClear` (under `KL_GLFB_BLIT_PROBE`)
  gets read straight back, default 200. **Not the first**: a run's first clear
  happens before the guest has ever called `glClearColor`, so it reports GL's
  own `(0,0,0,1)` and an opaque black that is entirely correct.
- `KL_GLFB_CLEAR_CONTROL=1` — **destructive**, and that is the point: at the
  same clear, clear the guest's framebuffer to magenta ourselves and read it
  straight back. If ours reads back and the guest's does not, the readback path
  is sound and the guest's clear is being lost; if neither does, the instrument
  is the liar. Costs one corrupted frame.
  Every readback here goes through one function, and three things it does are
  worth knowing because each of them used to make it report a working pipeline
  as a black one: it `glFinish`es for **every** caller (the draw probe did and
  the blit probe did not, and the two then disagreed about the same texture in
  the same run); it names the attachment's **level** as well as its layer, and
  reports and neutralises the framebuffer's **READ_BUFFER**, because a name
  alone is not an identification; and when it cannot establish the attachment's
  SIZE it now says `SIZE UNKNOWN, not read` instead of returning 0 lit. Callers
  that know a size from the call they are bracketing pass it — a blit names its
  own source and destination rectangles — and the note then says
  `(size from the caller)`.
- `KL_GLFB_PROBE_TEX=<name>` — read texture `<name>` back at array layers 0
  AND 1 at every `glBlitFramebuffer` (needs `KL_GLFB_BLIT_PROBE`). A guest that
  renders into its own target and copies from it has two failures with one
  symptom — "nothing was drawn" and "the copy read the wrong source" — and
  probing only the blit's own source cannot separate them, because the source
  is whatever the guest bound. This is what found VRChat's eye array lit while
  every blit read black. Layer 1 of a non-array texture answers `incomplete`.
- `KL_GLFB_READ_ATTACH_FIX=0` — the A/B for the eye-copy repair, ON by default.
  VRChat's Unity attaches its blit source with
  `glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, …)` while the read
  framebuffer is 0, which is `GL_INVALID_OPERATION` (the default framebuffer
  cannot take an attachment) — so the attach is dropped, the blit reads the
  window, and the eye receives black. The attach STATES its source completely,
  so the same attachment is carried on a framebuffer of ours and bound as READ;
  the guest's next read bind takes it away. `=0` restores the failing
  configuration.
- `KL_GLFB_NO_INVALIDATE=1` — don't forward `glInvalidateFramebuffer`.
  ANGLE/Metal actually discards (memoryless attachments), so this tests
  whether an invalidate is the eraser. Diagnostic only.
- `KL_GLFB_DUMP_PROGRAM=N` — at link time, print the captured sources of the
  shaders attached to program N (turns the timeline's `program=N` into text).
- `KL_GLFB_TEX_LIMIT=N` — perform only the first N `glTexSubImage2D` uploads
  and drop the rest; storage allocations still happen, so only texel contents
  go missing. Bisects the upload stream in situ. Default unlimited.
- `KL_GLFB_VS_FINITE=0|1` — (`runtime/gfx/kl_glfb.c`) every ES 3 vertex shader
  gets a guard: a clip position that is not finite (checked by its bits:
  infinity or NaN) is moved outside the view. Where a Quest's GPU drops such a
  triangle, Apple's draws it. Default 0: tried for Synth Riders' green line
  (its pooled rails at song start) and it did not remove it (2026-10-15).
- `KL_GLFB_DUMP_MATCH=<text>` — print the guest's own source of every shader
  containing `<text>` to the log (at most 8). With `KL_GLFB_PROGRAM_BINARY=0`
  for a guest that loads its programs from a binary cache.
- `KL_GLFB_SKIP_VFX=0|1` — (`runtime/gfx/kl_glfb.c`) leave out the draws of
  Unity VFX Graph output shaders (a shader source, active uniform or uniform
  block naming `attributeBuffer`, `deadListIn/Out`, `elementToVFXBuffer` or
  `graphValuesBuffer`). VFX Graph fills its particle buffers with compute
  kernels, which ES 3.0 ANGLE cannot run, so its effects draw from buffers
  nothing initialised: stray lines and triangles. Default 1 for Synth Riders
  (`x2436558143118760`), else 0. Log lines start `[glfb] program … is a VFX
  Graph shader`.
- `KL_GLFB_SKIP=<names>` — comma-separated GL entry points handed back to the
  null driver, so the ANGLE path is bisectable by family without a rebuild.
  Diagnostic only; the frame is wrong by construction.
- `KL_GLFB_TRACE=1` — log every GL call by name through the forwarding
  trampoline (`kl_gl_trace.S`).
- `KL_GLFB_TRACE_FROM=<n>` — start the `KL_GLFB_TRACE` printout at the nth
  call (default 0), so a crash deep in a long run stays readable.
- `KL_GLFB_LOCK=1` — route every resolved GL entry point through a
  process-wide lock held across the call (`kl_gl_lock.S`). Serialises all
  guest GL; a probe, not a mode.
- `KL_GLFB_COMPUTE_TF=0|1` — run compute kernels as transform feedback
  (default on for Beat Saber from 1.45 — `kl_guest_beatsaber_openxr`, rows
  `beatsaber140` and `beatsaber` — and off elsewhere). Beat Saber 1.45 builds
  the quads of its tube lights in a kernel (`FillMeshData`, dispatched by
  `BloomPrePassRendererSO.RenderAllLights`) that writes straight into a mesh's
  vertex buffer; ANGLE on Metal is ES 3.0 and has no compute stage. With this
  on, `glCreateShader(GL_COMPUTE_SHADER)` makes a vertex shader, the kernel's
  source is rewritten at `glShaderSource` (input buffer -> vertex attributes,
  output buffer -> captured varyings), and `glDispatchCompute` is one
  `GL_POINTS` draw under `RASTERIZER_DISCARD`. `kl_egl.c` answers the ES 3.1
  storage limits to match, or Unity refuses the kernel before GL sees it. Look
  for `[glfb] compute-tf:` — one line per rewritten shader, per linked program
  and for the first dispatch; a kernel of another shape says why it was not
  rewritten. `=0` is the state before: the kernel is refused, Unity logs
  "requires features ... 'randomWrite' 'uav'" every frame and the lights are
  missing from the bloom pre-pass.
- `KL_GLFB_COMPUTE_TF_LOG=1` — a `compute-tf: dispatch` line every 300th
  dispatch as well, with the invocation count (the number of lights, rounded
  up to work groups).
- `KL_GLFB_COMPUTE_TF_SELFTEST=1` — run each kernel once, right after it
  links, on three made-up lights and print the 4 vertices it wrote for each
  (screen position, view position, colour, uv). For a run that never reaches a
  scene with lights: the Mac run ends at the language screen and dispatches
  nothing. A fourth element beyond the light count must come back as zeros.
- `KL_GLFB_OUT=<dir>` — request a PNG capture on each `eglSwapBuffers`. Read
  by `kl_egl.c`'s SwapBuffers; the readback is serviced from the next call the
  context-owning thread makes (or immediately when the swap arrives on that
  thread), because the swap itself does not. **Also on `xrEndFrame`** (SL-13):
  an OpenXR guest never calls `eglSwapBuffers` at all — measured 0 across a 45 s
  streaming run — so this knob, and both frontend seams with it, were silently
  inert on the whole VR path.
  It also runs in the app now, beside the compositor's GPU fence (it used to
  be skipped whenever a fence was registered, which in the app is always).
  On the HOST an OVRPlugin guest's frame loop only advances with this set —
  without a capture, a sink or a fence nothing presents — so give it a
  directory and a large `KL_GLFB_OUT_EVERY` even when no picture is wanted.
- `KL_GLFB_OUT_EVERY=N` — throttle the PNG capture to every Nth swap (default
  1). Applies to the file path only; a registered frame sink gets every swap.
- `KL_GLFB_DUMP_FBOS=1` — alongside each `KL_GLFB_OUT` capture, write one PNG
  per live FBO (`frame_NNN_fbM.png`, same tone map). The intermediates — MSAA
  scene target, eye textures, the R11F bloom pyramid (blitted to RGBA16F for
  the read) — become inspectable, not just lit-counts. Reads are clipped to
  the `KL_GLFB_SIZE` buffers, so oversized attachments dump their top-left.
- `KL_GLFB_DUMP_SINK=<dir>` — with a frame sink registered (the viewer), write
  every Nth sink buffer to `<dir>/sink_NNNNN.png`. A black window with
  content in these files is an SDL-side problem; black files are capture-side.
- `KL_GLFB_DUMP_SINK_EVERY=N` — the interval above (default 100). The default is
  ~17 s of a 2D UI at the shell's frame rate, which is far too coarse to see
  what a `KL_VIEW_POKE` click did; 10 is the useful value when driving a
  frontend.
- `KL_GLFB_RAWSTATS=0` — silence the per-60-captures line reporting the eye
  texture's raw (pre-tone-map) min/max/mean. On by default; it is how the
  "very dark" picture was shown to be 1e-3 linear content, not a bad curve.
- `KL_GLFB_EXPOSURE=<x>` — multiply the linear eye-texture value by <x> before
  the debug tone map (default 1). Makes the loading screen watchable; it is a
  viewing aid, not a fix — the content really is ~1e-3 while loading runs.
- `KL_GLFB_GAMMA=<g>` — override the tone map's encode exponent (default
  1/2.2). Lower brightens mid-tones.
- `KL_GLFB_MTL=1` — P5: the guest's eye textures become **MTLTextures we
  allocate** on ANGLE's own device, via
  `eglCreateImageKHR(EGL_METAL_TEXTURE_ANGLE)` — the host stand-in for
  Compositor Services (`tests/t_mtl_provider.m`). With `KL_GLFB_OUT` set it
  also writes `mtl_eye0.png`/`mtl_eye1.png` from the textures, tone-mapped like
  the reference frames so the two are directly comparable.
  On the Steam Link target's **VR** front door it does the same for the OpenXR
  swapchains (SL-19) and prints, at the end of the run, the last complete frame
  record's stage and how many texels of each eye are lit. That is the host arm
  of "the immersive space is black": a stage of -1 means no frame record, `0
  lit` means the guest drew nothing there, and anything else means the picture
  reached the texture the compositor samples.
- `KL_ANGLE_VRR_TRACE=1` — read by **our patched ANGLE**, not by the runtime
  (`angle-patches/klepton.patch`, `vendor/src/libANGLE/renderer/metal/`). Logs
  the variable-rasterization-rate seam: every rate map bound or unbound through
  `ANGLEMetalSetRasterizationRateMap`, the per-framebuffer lookup in
  `FramebufferMtl::prepareRenderPass` (with the render target's *actual*
  `id<MTLTexture>`, which is a texture VIEW of the one that was registered), and
  the map as it reaches `MTLRenderPassDescriptor`. Read once and cached, so it
  costs nothing when unset. The three lines together answer the only question
  that matters when foveation silently does nothing: was the map registered, was
  it found, and did it reach Metal. `make vrr` is the standalone gate.
- `KL_GLFB_ERRSCAN=<code>` — name the call that **generates** a GL error
  anywhere in the stream, not just the few ERRPROBE hand-wraps: the trace
  trampoline logs before each call, so a non-zero glGetError entering call N
  was produced by call N-1. `=1` reports every code; `=0x502` reports only that
  one — necessary, because the sticky `TEXTURE_SRGB_DECODE_EXT` INVALID_ENUM
  otherwise spends the whole budget thousands of calls early. This is what
  found the base-vertex bug (40/40 votes). Note it *clears* the error flag, so
  the guest's own glGetError sees nothing and Unity's spam stops — that
  silence is the confirmation it is seeing the same errors.
- `KL_GLFB_PROBE_CTEX=1` — log every **compressed** upload with the byte count
  its dimensions imply, and check the region and format against what
  `glTexStorage2D` allocated. A disagreement prints `SIZE MISMATCH` /
  `REGION OVERRUNS LEVEL` / `FORMAT != ALLOCATED` with or without the knob —
  the knob adds the *clean* lines, which is what makes two platforms diffable.
  Costs a `glGetIntegerv` per upload, hence opt-in. This is what exonerated
  the guest over the Simulator's ETC2 fault.
- `KL_GLFB_RESOLVE_STAGE=0` — turn off the staged MSAA resolve. On by
  default: when a guest's `glBlitFramebuffer` from a multisampled renderbuffer
  fails with `GL_INVALID_OPERATION` (ES 3.0 forbids a resolve into a different
  format; the Quest's driver allows it), the resolve is redone through a
  same-format staging texture and then blitted to the guest's destination.
  Keep Talking and Nobody Explodes resolves RGBA8 into its SRGB8_ALPHA8 eye
  texture and showed solid pink without it.
- `KL_GLFB_SHARED_MIGRATE=0|1` — a guest that moves its one GL context from
  one thread to another (the old render thread releases it, the new one makes
  it current) gets the SAME context on the new thread, with its framebuffers
  and vertex arrays, instead of a fresh one. Unreal does this; the parked
  context is handed to the next thread that asks, which is only right while
  the guest has one context. Default 1 for `x2897337400373711` (The Walking
  Dead: Saints & Sinners), `x3484270071659289` (Tales from the Galaxy's
  Edge), `x5190288877703616` (The Walking Dead: Saints & Sinners -
  Chapter 2: Retribution) and `x2108775495884888` (Vader Immortal I: two of
  its three eye stages were drawn into framebuffers the fresh context did not
  have, and showed black — flickering lights and holograms; Episodes II and
  III, `x2223615721048141` and `x2426206484098337`, likewise), else 0.
- `KL_GLFB_COPYIMAGE_EMU=0|1` — emulate `glCopyImageSubData`, which ANGLE on
  Metal has under no name: compressed 2D uploads are kept on the CPU per
  texture and level and a copy re-uploads from there; uncompressed copies go
  through a read framebuffer. Costs memory equal to the compressed textures
  resident. Default 1 for the same two titles and the other Unreal titles
  named in `klfb_cimg_on`, among them `x5190288877703616` (Saints & Sinners -
  Chapter 2, whose textures were all purple without it) — Unreal's texture
  streaming needs it — and `x3231001163584154` (Warhammer 40,000: Battle
  Sister, a Unity title that copies textures into 2D arrays while loading a
  level), else 0. Copies between 2D textures and 2D-array slices are handled;
  a compressed source must be a 2D texture (the shadow is kept per 2D
  level); the call itself always goes through a thunk that reads its stack
  arguments the way the guest pushed them.
- `KL_GLFB_TB_MANY=0|1` — keep up to 8192 textures over buffers (and 8192
  buffers) fresh in the texel-buffer emulation instead of 1024. Past the
  limit a texel buffer's 2D stand-in keeps its first contents, and what is
  drawn from it (skinned meshes, particles) goes wrong. Default 1 for
  `x5190288877703616` (Saints & Sinners - Chapter 2, which passes 1024 while
  loading), else 0.
- `KL_GL_TB_ALIGN=<bytes>` — (`runtime/gfx/kl_egl.c`) the answer to
  `glGetIntegerv(GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT)`, which ANGLE's ES 3.0
  context does not know. Unanswered, the guest reads 0, and Unreal aligns
  every texel-buffer window in its pooled read buffer to that — `Align(x, 0)`
  is 0 — so every allocation in the pool lands at byte 0 and each one
  overwrites the last. Default 16 for `x5190288877703616` (Saints & Sinners -
  Chapter 2, whose Niagara effects all read the last effect's particle data:
  sparks drawn as coloured streaks), else unanswered (0). The texel-buffer
  emulation takes a window at any byte offset.
- `KL_GLFB_ES32_ALIAS=0|1` — the ES 3.2 core names of the indexed blend and
  enable calls (`glEnablei`, `glBlendFuncSeparatei`, `glColorMaski`, …) are
  answered with ANGLE's OES/EXT functions; under their core names ANGLE's
  ES 3.0 context refuses them. Default 1 for Unreal guests, else 0.
- `KL_GLFB_ZERO_STRIDE=0|1` — a `glBindVertexBuffer` binding with stride 0 is
  one element for every vertex, as ES 3.1 says, not a tightly packed array.
  Default 1.
- `KL_GLFB_MV_DIVISOR=0|1` — a per-instance attribute set with
  `glVertexAttribDivisor` on the default VAO steps once per view-count
  instances while a multiview framebuffer is bound, as the ES 3.1 binding
  translation already does. ANGLE draws two views as twice the instances; left
  unscaled, the left eye drew the even instances and the right eye the odd
  ones (Vader Immortal I, UE 4.19: meteors in one eye, flickering effects).
  Default 1; 0 passes the guest's divisor through unchanged.
- `KL_GLFB_EYE_CLEAR=0|1` — a provider-backed eye texture is cleared to opaque
  black once when it is bound, so a first frame cannot show what the memory
  held. Default 1 for Unreal guests, else 0.
- `KL_GLFB_LOG_UNITS=1` — log texture-unit traffic (the "Invalid texture
  unit!" neighbourhood — see `KL_POKE_CAP` under Viewer).

## Networking (`runtime/libc/kl_shim.c`)

- `KL_NET_BCAST_FANOUT=0` — stop delivering a broadcast as unicast to every host
  on the subnet. Default ON. **This is what makes Steam Link's host discovery
  work on visionOS**, where an app may not broadcast without
  `com.apple.developer.networking.multicast` — an entitlement Apple grants by
  REQUEST, not by enabling a capability, and one this project deliberately does
  **not** ship (`visionos/Klepton.entitlements` says why). Measured shape of the thing being
  worked around (SL-19): the guest opens an **IPv6** socket, binds
  `[::]:27036`, and sends its 31-byte probe to `[::ffff:255.255.255.255]` and
  `[ff02::1]`, both of which return *No route to host*. The fan-out replays it
  as unicast — as IPv4-mapped addresses, since the socket is IPv6 — and the
  host answers: `recvfrom -> 213 from [::ffff:192.168.4.28]:27036`. It covers
  both `sendto` and `sendmsg`, because this guest imports both. `make bcast` is
  the gate.
- `KL_SLINK_HOST=<ip>[,<ip>…]` — replace the subnet sweep with an explicit
  list, one packet per address. The direct-connect case, and the answer when
  the subnet is too wide to sweep (anything past a /22 is refused by name, and
  a truncated sweep says how many addresses it dropped — a sweep that quietly
  covers half a /22 finds the machines at the bottom of the range and not the
  ones above them).
- `KL_NET_DGRAM_NOSIGPIPE=0` — stop setting `SO_NOSIGPIPE` on the guest's
  non-stream (datagram/raw) sockets. Default ON: Linux never raises SIGPIPE for
  a datagram send, Darwin does on EPIPE, and the guest's SIGPIPE handler
  (Unity's crash reporter) runs on a Darwin ucontext and faults. Beat Saber
  1.40's Unity Ping hit exactly that at boot ("Ping: Error sending ICMP packet:
  Broken pipe", then signal 13, then a fault at 0x0 in libunity).
- `KL_GUEST_JIT=1` — allow the guest to map anonymous `PROT_EXEC` memory.
  Default REFUSED, loudly: AMFI will not execute an unsigned page, so a mapping
  that succeeds is a SIGKILL on the first branch into it, with no signal and no
  log line (trap 27). A JIT that is told no takes its interpreter. On a host run
  there is no AMFI and this is safe to set.
- `KL_TRACE_NET=1` — every socket call the guest makes, with addresses and
  errors. Note the name: it is `KL_TRACE_NET`, not `KL_NET_TRACE`, and the
  wrong one costs a device round trip that looks like "the guest does no
  networking".

The socket layer translates bionic→Darwin: `SOL_SOCKET` level + option numbers
in setsockopt/getsockopt, the `sa_len` byte and `AF_INET6` (10 vs 30) in every
sockaddr carrier, and the addrinfo list (same struct layout on both — only the
sockaddrs inside need converting). Without all three, Unity's Ping and the
gamelift region probes failed EINVAL in a retry storm.

- `KL_TRACE_NET=1` — log `socket`/`bind`/`getaddrinfo`/`connect` with
  arguments and durations, and every `send`/`recv`/`sendto`/`recvfrom`/
  `sendmsg`/`recvmsg` with its byte count, its peer, and its MSG_* flag word
  both before and after translation. Read by `kl_shim.c`.
- `KL_TRACE_NET_HEX=<n>` — alongside each of those, the first `n` bytes of the
  payload in hex and ASCII. Counts alone cannot tell a discovery probe from its
  own broadcast echo (identical lengths), nor say which TLS record a handshake
  died on; both questions came up in one session and both needed the bytes.
- `KL_NET_RATE=<seconds>` — every `<seconds>`, one line per receiving socket:
  datagrams, bytes, the rate those imply, the datagram size range, the socket's
  effective `SO_RCVBUF`, and the change in the kernel's system-wide count of
  datagrams thrown away for a full socket buffer (macOS only — the header that
  names that counter is not in the xrOS SDK, and the line says so instead of
  printing a zero). Written for the streaming question a client cannot answer
  about itself: a received rate that starts high and collapses is a host rate
  controller reacting to loss, while a rate flat from the first interval was
  decided before any packet moved. Cheap enough to leave on for a whole
  session; `1` is the useful value.
- `KL_SVL_TRACE=1` — Steam Link only: print every value the streaming client
  publishes into the key/value tree it shares with the Steam host
  (`cRXpps`, `cRXerr`, `cRXpass`, `cStandoff`, `cFramePhase`, the `cAud*`
  family). The client does not choose its own video bitrate — the host does,
  from these numbers — so this is the whole channel by which a stall on this
  side becomes a lower bitrate from the other. The first write of a key always
  prints; after that only a changed value, at most once a second, which leaves
  each key as a readable per-second series. Answers the guest's own function at
  relocation time, so it costs nothing when unset.
- `KL_NET_BIND_REMAP=<from>:<to>[,...]` — bind a guest listening port
  somewhere else. A **host-development** knob, off by default: on the headset
  the guest is alone, but on a Mac that is *also* a Steam host, Steam owns UDP
  `*:27036` and Steam Link's discovery socket wants the same port. Darwin lets
  the two wildcard sockets coexist, then delivers arriving IPv4 packets to the
  AF_INET socket as the more specific match — so every discovery reply is lost
  and the app reports no computers on the network. `kl_bind` names that
  collision when it sees it. Steam answers to whatever source port asked
  (measured), so `27036:27136` restores discovery with Steam still running.
- `KL_NET_OFFLINE=1` — getaddrinfo fails `EAI_NONAME` and connect fails
  `ENETUNREACH` immediately: a headset with no network. The guest takes its
  own offline path (curl reports "Could not resolve host", the GameLift
  region probe retries 4× per region and gives up) and the run completes.
  The abort this used to cause was *not* a DNS bug — it was the empty
  `dl_iterate_phdr` breaking the guest's unwinder, so the `SocketException`
  could never reach its handler. See trap 8.

## TLS trust store (`runtime/kl_cacerts.c`)

The guest's HTTPS stack (unitytls, under both UnityWebRequest and Mono) builds
its CA bundle from the Android system trust store over JNI:
`TrustManagerFactory` → `X509TrustManager.getAcceptedIssuers()` →
`Certificate.getEncoded()`. We serve the host's own root anchors there, baked
into `runtime/kl_cacert_table.h` by `make cacerts`.

Baked rather than read live because `SecTrustCopyAnchorCertificates` is
`__IPHONE_NA`: macOS can enumerate its trust store and visionOS cannot, so
reading it where it exists would leave the device trusting a different set of
roots from the host. Validation itself is untouched — libunity asks for
`getAcceptedIssuers` and never `checkServerTrusted`, so the guest takes a trust
*set* from us and reaches its own verdict. A trust-all manager is refused, by
the same rule as the DRM guard.

- `KL_CA_ANCHORS=0` — present an EMPTY trust store, which is what this runtime
  did before the table existed. Every guest TLS chain then fails as
  `UNITYTLS_X509VERIFY_FLAG_NOT_TRUSTED` (VRChat: `Curl error 60`,
  `Connection to API Failed: SSL CA certificate error`, and login cannot
  complete). The A/B for "is the trust store what is blocking this?", and it
  restores the failing configuration exactly.

## Loader diagnostics (`runtime/libc/kl_shim.c`, `runtime/libc/kl_libc.c`)

Built for the loading-pace investigation; all default off.

- `KL_TRACE_IO=1` — once a second: cumulative guest `fread`/`read` bytes,
  fread attributed per open file. This is how the "slow asset loading"
  hypothesis died: bulk reads stop within seconds; the only continuing trickle
  is libunity re-reading `/proc/cpuinfo` once per frame.
- `KL_TRACE_SLEEP=1` — once a second: usleep count/mean/max.
- `KL_TRACE_SYSCONF=1` — every `sysconf()` the guest asks and the answer given.
  What to reach for when a guest sizes a thread pool or a heap to zero.
- `KL_TRACE_THREADS=1` — one line per guest `pthread_create`: entry, arg, stack
  size, detached, result. The entry pointer symbolises against the load
  addresses phase 1 prints, so it names which subsystem the thread belongs to.
- `KL_TRACE_TSD=1` — one line per guest `pthread_key_create`: index handed out,
  destructor, and how many keys are live. The guest TSD table is Klepton's own
  (512 keys, indices recycled on delete) rather than one Darwin key apiece, and
  this is how you see a guest approaching the ceiling BEFORE it gets EAGAIN —
  which guests do not handle: Asgard's Wrath 2 stored the failed key as -1 and
  faulted on the resulting NULL `getspecific` a map load later. The high-water
  number is in every fault dump without setting this.
- `KL_TRACE_FUTEX=1` — once a second: futex waits split timeout-vs-woken, and
  wakes. The loading crawl came with zero timeouts — the wake path is healthy.
- `KL_USLEEP_CAP=<usec>` — clamp every guest usleep. Proved the ~5 ms polling
  loop is a watcher, not the loading pacer.
- `KL_DLOPEN_REFUSE=<substr>[,<substr>]` — refuse these guest libraries by name,
  as if the file were not installed. Matched against the whole path, so
  `phonon` catches `libphonon` and `libaudioplugin_phonon` both. Not a way to
  hide a gap: a guest that dlopens an optional plugin already has a path for it
  being absent, and a NULL here is what a device gives it. What it buys is
  **isolation** — `KL_DLOPEN_REFUSE=phonon` takes Steam Audio out of a VRChat
  run, which is the difference between reaching the frame loop and not.
  Nothing is refused by default and every refusal is named.
- `KL_METAXR_AUDIO_STUB=0|1` — serve `libMetaXRAudioUnity` (refused on every
  guest: it trips AMFI) as a library that is there, whose every function
  returns 0 (ovrSuccess) and whose version calls answer the shipped plugin's
  version. On by default for `walkabout` only: its audio setup calls the SDK's
  managed interface and each P/Invoke behind it threw DllNotFoundException,
  every frame, cutting those methods short. The engine registers no
  spatializer either way. `0` is the plain refusal again; `1` serves the stub
  on any target.
- `KL_TRACE_DLOPEN_FRAMES=1` — on a FAILED guest `dlopen`, walk the guest stack
  and print it. A P/Invoke that cannot resolve throws from IL2CPP's resolver and
  the managed method that declared it is named nowhere in the exception; the
  resolver runs on the guest's own stack, so the frame walk reaches the
  generated wrapper and `tools/vrc_code.py --whois` turns that address into a
  method. Prints the conservative all-stack-words version too, because an
  obfuscated `.text` keeps no frame chain.
- `KL_X18_MAP=<file>` — append "veneer_addr site_addr" per patched x18 site,
  so a guest pc captured in a shim maps back to its call site.
- `KL_TRACE_IMAGES=1` — per-image base/span; needed to turn a guest pc into a
  file offset (`pc - image base`, e.g. the connect-NULL site at
  libunity+0x966964).

## Guest patches (`runtime/guest/kl_guestpatch.c`)

Measured one-instruction rewrites of a guest image, applied at load while it is
still writable. There are two, both VRChat's, and both are named in the log
every time they fire, with the address and the instruction each replaces.

- `KL_GUEST_PATCH=0` — apply none of them. The A/B for every row: with it VRChat
  stops on its "Under Construction" screen exactly as it did before.
- `KL_GUEST_PATCH_OFF=<name>[,<name>]` — turn off one by name.
  `unity-texunit-mask` is found in any libunity from 2018.4 to 2021.3 rather
  than measured: it takes the texture unit `& 31` at the bind, so samplers above
  31 stop being refused ("Invalid texture unit!", black surfaces, tofu text), and
  `glUniform1i` masks sampler uniforms to match, only when the loaded image
  carries the patch. On a headset klepton-ld writes it at the build, so the
  variable has to be set there. The measured names are
  `vrchat-under-construction`, `vrchat-min-client-version`, `vrchat-multipass`
  and `vrchat-fbo-context-guard`; `fluid-keyboard-warmup` (Fluid,
  `x6946182742093178`) lets its startup keyboard warmup complete without the
  Java keyboard's surface. `vrchat-min-client-version` is the one to
  reach for, because it is a real version check against VRChat's own API (the
  tree's APK is build 1862 and the service asks for 1865) and the honest fix for
  it is a newer APK, not a patch. `vrchat-fbo-context-guard` is the A/B for
  Unity's cross-context framebuffer guard: off, VRChat binds `(GLuint)-1` as its
  draw framebuffer and loses whole render passes to
  `GL_INVALID_FRAMEBUFFER_OPERATION` (322 a run vs 0), including what it does
  *not* fix.
- `KL_TRACE_HRTF=1` — interpose Steam Audio's `iplHRTFCreate` and print the
  `IPLAudioSettings` and `IPLHRTFSettings` the guest passes, plus the result.
  Managed code resolves a P/Invoke by name through `klb_dlsym`, so a wrapper
  handed back there sits on the seam with the real function one call away. This
  is what showed that `size == 1` is not the frame size we supply.
- `KL_PHONON_HRTF=0` — do not substitute an HRTF. On by default, and only for a
  guest whose `libphonon.so` cannot answer its own default: VRChat's ships a
  40-byte `gDefaultHrtfData` stub instead of Steam Audio's ~1 MiB table, so
  type=DEFAULT yields `numSamples == 1` and PFFFT refuses it (`Unable to create
  PFFFT setup (size == 1)`). `iplHRTFCreate`'s settings are rewritten to
  type=SOFA pointing at CIPIC subject 124, vendored from Valve's steam-audio
  tree and baked in by `runtime/media/kl_phonon_hrtf.S`. `KL_TRACE_HRTF=1`
  prints the arguments either way.

- `KL_TRACE_WMEMCHR=1` — print every `wmemchr`, with the haystack on a miss.
  Steam Audio picks its HRIR length by `std::find` over an int array, which
  lowers to `wmemchr`; a miss there has no error surface at all.
- `KL_TRACE_PATCH=1` — print each word: address, the value that was there, the
  value written, and what the new instruction is. Also prints the rows that were
  considered and left alone because the address is past the end of the image,
  which is the ordinary answer for every other guest's `libil2cpp.so`.

A row is skipped, loudly and by name, when the word at its address is not the
word it was measured against — that is a build fingerprint, so these cannot
corrupt an APK they were not measured on. **Not yet wired for device**: a
klepton-ld dylib's text is signed and cannot be written at run time, so a device
run currently behaves as `KL_GUEST_PATCH=0`.

## Memory: decommit, budget and pressure (`runtime/libc/kl_libc.c`)

The guest's own memory management, and the three seams that used to disable it.

- `KL_CPUS=<n>` — how many CPUs the guest is told it has. Default is this
  machine's real count, and it is answered through **one** function so the two
  doors that ask cannot disagree: `sysconf(_SC_NPROCESSORS_ONLN/_CONF)` and the
  synthetic `/proc/cpuinfo` + `/sys/devices/system/cpu/possible`. An engine
  sizes its worker pool from one and its affinity masks from the other.
  This is the cheapest A/B for "is this a race?", because it changes the
  GUEST's own concurrency without touching the guest: UE4 sizes `GThreadPool`
  from the core count, so `KL_CPUS=1` collapses the pak-decompression fan-out.
  It earned its keep by **exonerating** one — RE4's Oodle crash reproduced
  identically at 1 and at 32, which is what moved the search off concurrency.
- `KL_GUEST_FRESH_POSE=0|1` — start each guest frame on a freshly offered
  pose: one offered while the previous frame was still running is dropped
  (unless that frame ended within 2.5 ms of it) and the loop waits for the
  next. A guest frame then always lasts a whole number of display frames
  instead of beating against them.
  Default 1 for `x5812519008825194`, else 0 (start at once on any pending pose).
- `KL_NEXUS_FRAME_BUDGET=0|1` — AC Nexus spins every frame out to a fixed
  length for Application SpaceWarp, a per-headset constant (21.25 ms as a
  Quest 2 at 80 Hz), which held it at 46.7 fps here where nothing synthesises
  the frames between. With this on, that one managed value is rewritten to
  `KL_NEXUS_FRAME_BUDGET_MS` and the game runs as fast as it can (100 fps in
  menus, 70-98 in a level on device). Default 1 for `x5812519008825194`, else 0.
- `KL_NEXUS_FRAME_BUDGET_MS=<ms>` — the length to spin to instead, default 0
  (no spin). 17.0 is the game's own rule (85% of two display periods) at
  100 Hz and, with `KL_GUEST_FRESH_POSE`, gives exactly half the display rate.
- `KL_FS_REL_SDCARD=0|1` — resolve a relative path that begins with `sdcard/`
  (or another spelling of external storage) as the absolute one, as it is on a
  device where the app's working directory is `/`. AC Nexus opens its animator
  controller bundle that way. Default 1 for `x5812519008825194`,
  `x2773034772778845` (Premium Bowling: every hall's scene bundle) and
  `x3231001163584154` (Battle Sister: its five store bundles), else 0.
- `KL_GLFB_PROBE_CUBE_ARRAY=0|1` — (`runtime/gfx/kl_glfb.c`) rewrite the
  reflection-probe part of PixelToys' BLAST shaders, a `samplerCubeArray` and
  a read-only storage buffer (ES 3.2 / 3.1, refused by the ES 3.0 backend), as
  an unused `samplerCube` (so it holds no texture unit) whose every sample is
  the scene's ambient light (`unity_SHA*`) in the reflected direction, and a
  constant of the buffer's type that leaves it undecoded: the level draws with
  approximate reflections instead of not at all, and its metal is not black.
  Default 1 for Warhammer 40,000: Battle Sister (`x3231001163584154`), whose every level
  surface was black, else 0.
- `KL_GLFB_NO_GRADING_LUT=0|1` — (`runtime/gfx/kl_glfb.c`) read a shader's
  `_GradingLut` lookup (a 3D colour-grading table filled by a compute kernel,
  which this ES 3.0 context cannot run, so it stays black) as the identity:
  colours as they are, without the game's grading. Default 1 for Warhammer
  40,000: Battle Sister (`x3231001163584154`), whose level surfaces were black,
  else 0.
- `KL_GLFB_BIND_PINS=0|1` — (`runtime/gfx/kl_glfb.c`) hand the uniform
  locations a shader pins (`layout(location = N) uniform …`,
  `UNITY_LOCATION(N)`) to the linker with `glBindUniformLocationCHROMIUM`
  instead of only translating them at `glUniform*`. A Unity built for ES 3.1
  uses the location it is told for a sampler as the texture unit; with the
  linker's own numbering that can be 32 or more, which the engine refuses
  ("Invalid texture unit!") — text as filled boxes. Default 1 for
  `x2773034772778845` (Premium Bowling, Unity 6000.3.22f1) and
  `x2582932495064035` (Real VR Fishing, as its fix was sent in),
  `x2436558143118760` (Synth Riders), `x4015163475201433` (LES MILLS
  BODYCOMBAT, every TextMeshPro glyph a block) and
  `x2285803881494677` (Gun Club VR: black gun materials and repeated
  invalid texture-unit errors, surface detail with the pins, as its fix was
  sent in) and `x9863010250399155` (Surviving Mars: Pioneer, Unity
  2022.3.62f1: samplers at 99..104, its button text unreadable) and
  `x438100` (VRChat from Steam, Unity 6000.0.67f1: menu text as wrong
  shapes), else 0.
- `KL_GLES_VERSION=<major>.<minor>` — (`runtime/guest/kl_driver.c`) the GLES
  level described to the guest, over the per-title choice (3.2 by default; 3.0
  for Space Channel, DreamSpace, Catan VR and Gods of Gravity). For trying a
  title on the 3.0 profile without a build: told 3.0, Unity stays off the ES
  3.1/3.2 paths this backend only translates (compute kernels, the separate
  vertex-format calls, base-vertex draws). Gods of Gravity: under 3.2 its wand
  models were drawn as large flat surfaces; under 3.0 they are right.
- `KL_GLFB_TEX_MS_ANGLE=0|1` — (`runtime/gfx/kl_glfb.c`) serve the guest's
  `glTexStorage2DMultisample` (ES 3.1) through ANGLE's
  `glTexStorage2DMultisampleANGLE`, which an ES 3.0 context accepts. Without
  it the core call is refused, the multisampled texture has no storage, and a
  framebuffer that renders into it is `INCOMPLETE_ATTACHMENT`: every draw is
  dropped. Default 1 for `x2436558143118760` (Synth Riders, Unity
  6000.3.13f1: its camera colour target, both eyes black), else 0. (Fix 42.)
- `KL_FS_JAR_FILE=0|1` — a path of the form `jar:file:///…` with no `!/`
  entry part is read as the plain path it wraps; Unity's Android file layer
  strips that scheme itself. Default 1 for `x3635172946605196` (BRINK
  Traveler: every Addressables bundle location is written that way), else 0.
- `KL_ELF_TLSDESC=0|1` — the loader serves `R_AARCH64_TLSDESC` relocations
  (ELF thread-local storage, dynamic model) for a library's own TLS: a block
  per thread and module, made at first use. Off, such a library does not
  load, as before. Default 1 for `x3635172946605196`, `x4360608937312293`
  (Wooorld, whose sign-in needs `libovravatar2`), `x2773034772778845`
  (Premium Bowling, same library), `x3634830803298285` (Demeo,
  `libInteractionSdk`) and `x9386827918099701` (BattleGroupVR2, the
  `LibRuntimeOptimizer` it ships) and `x9449316705178989` (DreamSpace, Meta's
  `libmrutilitykitshared`), else 0 — a title that works may ship an
  optional library it has been running without.
- `KL_MADV_DONTNEED=remap|reusable|zero|passthrough` — how a guest
  `madvise(MADV_DONTNEED)` is served. Default `remap`. **`MADV_DONTNEED` is 4 on
  Linux and Darwin and means different things**: Linux frees the pages, Darwin
  merely deactivates them, so Unity's `mprotect(PROT_NONE)` + `madvise(4)`
  decommit returned *nothing* and the footprint became a high-water mark.
  `passthrough` is the old behaviour and the A/B.
- `KL_TRACE_MADV=1` — every decommit, and **what it actually released** measured
  against `phys_footprint` either side of the call. That second number is the
  one that matters: "asked 16384 KiB" and "released 0 KiB" is the bug, and
  `passthrough` prints exactly that on every call while `remap` prints the full
  amount. Rate-limited to the first 8 calls, then every 256 MiB.
- `KL_MEM_TOTAL_MB=<n>` — what `/proc/meminfo` reports as `MemTotal`, capped at
  the host's real memory. Default **6144**, the Quest 2's RAM: this is part of
  the device identity we already present, and a 6 GB title sizes its streaming
  budgets and pool ceilings from it. It used to report the host's — 16 GB on a
  Vision Pro, 32–128 on a development Mac. `MemFree`/`MemAvailable` are now
  recomputed on every read against our own footprint (they were a startup
  snapshot), and `/proc/self/statm` and `/proc/self/status` are served at all.
- `KL_LOWMEM_AT=<percent>` — footprint at which the guest is told memory is
  short, through `UnityPlayer.nativeLowMemory` (where a title runs
  `Resources.UnloadUnusedAssets()`). Default 80; **0 disables**, which is the
  A/B. Nothing delivered this notification before, so every cache a Quest build
  drops under pressure was held here forever.
- `KL_LOWMEM_CLEAR=<percent>` — re-arm threshold, default 70. A guest that
  cannot free anything is asked once, not once a frame. The OS's own
  memory-pressure signal ignores both and always fires.
- `KL_LOWMEM_FREE_MB=<MiB>` — where the OS says how much this process may
  still allocate (`os_proc_available_memory`, so on a headset), the guest is
  told when less than this is left, and the two percentages above are not
  looked at; told again only after 1.5 times as much was free. Default 384.
  1536 for BattleGroupVR2 (`x9386827918099701`): what it asks for at its start
  came in steps larger than the smaller reserve.
- `KL_LOWMEM_USAGE_CHANGED=0|1` — (`runtime/kl_jni.c`) a guest with no
  `nativeLowMemory` is told through Unity 6.2's
  `UnityPlayer.nativeMemoryUsageChanged(Critical)` instead, on the player the
  lifecycle runs on. Default 1 for BattleGroupVR2 (`x9386827918099701`), else
  0: another Unity 6.2 title is told nothing under pressure, as before, and
  what each does when told has not been looked at.

## Guest storage (`runtime/kl_jni.c`)

- `KL_FILES_DIR=<dir>` — the writable root behind `getFilesDir` /
  `getExternalFilesDir` / `getCacheDir` / `shared_prefs`, i.e. where the guest's
  saves, PlayerPrefs and Steam Link's pairing credentials live.

  Default is **`~/Library/Application Support/Klepton/userdata/<guest>`**, where
  `<guest>` is the TARGET's name (`beatsaber`, `superhot`, `steamlink`) — the
  host driver applies it from the target table (`runtime/kl_target.c`), so a run
  cannot write one title's saves into another's. Two properties are deliberate.
  It is
  **outside `build/`** — it used to be `build/android-files`, so `make clean`
  cost a Beat Saber first-run setup and a Steam Link re-pairing every time it
  ran. And it is keyed on the **guest, not the APK**, so swapping Beat Saber
  1.28 for 1.6.0 keeps one profile instead of repeating first setup, which is
  the case the move exists to survive.

  Set this when two versions must *not* share — a save format that changed
  under them — or to give a run a scratch profile without disturbing the real
  one. visionOS ignores it: the app container is the only writable location
  there and `kl_app.c` passes it in explicitly.

- `KL_PLAT_CLOUD_DIR=<dir>` (`runtime/xr/kl_ovrplat.c`) — where
  `ovr_CloudStorage2_GetUserDirectoryPath` says the user's cloud-save directory
  is. Default `<files-dir>/cloudstorage`, a sibling of the `files` directory
  that is `Application.persistentDataPath`, and created if it is not there —
  the guest runs its own writability check on the answer before it uses it, and
  a path that does not exist fails that check.

  It is the one member of the CloudStorage family that is answered rather than
  refused, and the split is the DRM line's: `_Load`/`_Save` move data to and
  from a service that is absent, while this call moves nothing and only names a
  local directory. Refusing it is what a black launch screen looks like —
  SUPERHOT's save loader chains its whole load off this one request, so a
  refusal leaves the SDK waiting on a completion that never comes, behind the
  launch precache's own black clear.

- `KL_APK=<path>` — the APK behind `getPackageCodePath()` and
  `ApplicationInfo.sourceDir`, which Unity opens as a zip for its streaming
  assets. Host default is `beatsaber.apk`; set it (with `KL_FILES_DIR`) when
  host-booting another title. visionOS ignores it — `kl_app.c` passes the
  staged APK explicitly.

## JNI surface (`runtime/jni/`)

Not a switch, for orientation: an outgoing Android intent is answered for
every title (`kl_jni_android.c`). `new Intent()`, `setAction`, `setData`
(with `Uri.parse`), `setPackage`, `setComponent` (with `new ComponentName`),
`setClassName`, `setFlags`, `putExtra` and `Intent.ACTION_VIEW` build it;
`Context`/`Activity.startActivity` logs what it names and refuses it, as
there is no other app here. Before these were bound, each such call ended
the run as an unimplemented JNI call.

- `KL_UE_KEYBOARD=1` — hold UE's GameActivity virtual keyboard open
  (`runtime/jni/kl_jni_ue4.c`) so a frontend can type into it through
  `kl_jni_soft_input_*` (`runtime/kl_jni.h`), the same four calls Unity's soft
  input answers. The default, with no keyboard on the host, ends every entry
  immediately by committing the text the guest opened it with — the engine's
  only end-of-entry native is an accept, and an entry nobody closes leaves the
  widget holding input for the rest of the session.
- `KL_JNI_OPEN_URL=0|1` — (`runtime/jni/kl_jni_android.c`,
  `Context.startActivity`) an intent to VIEW a web page (or a Quest app link
  with a web counterpart: Spotify, YouTube) is handed to the app, which opens
  the page in a web window of its own beside the immersive space (not Safari,
  which would send the app to the background and end it). Default 1 for
  DreamSpace (`x9449316705178989`), else 0: the intent is logged and refused,
  as before. One rule (`kl_mono_web_url`, `runtime/guest/kl_mono.c`) decides
  both the page an intent opens and every navigation inside the window
  afterwards — links, server redirects, frames, pages that ask for a new
  window (the app's navigation delegate calls the same function): https only
  (an `http://` address is upgraded); the host must be spotify.com,
  youtube.com, youtu.be, meta.com, oculus.com or a subdomain of one, or
  exactly facebook.com or accounts.google.com; it must be a plain DNS name
  (no user part, no IPv4 or IPv6 literal, no raw IDN, labels of 1-63
  letters, digits and inner dashes, 253 characters in all). Anything else is refused and logged with the
  reason. accounts.google.com is allowed (exactly) so a YouTube sign-in can
  finish; the rest of Google is not, so the Meta browser's own button (which
  had no web page but www.google.com) opens nothing. The window also
  downloads nothing, gives a page no camera or microphone, and shows no link
  previews. The log shows only the scheme and host of an address. `make check`
  runs `tests/t_jni_url.c`.
- `KL_UE_KEYBOARD_TEXT=<s>` — the default auto-close commits `<s>` instead of
  the seed (initials on a score screen). Never applied to a password field.
- `KL_TRACE_DLSYM=1` — also covers the synthetic `libandroid.so` handle
  (`runtime/guest/kl_ndk.c`): every `dlsym` a guest makes on it prints
  `[ndk] dlsym("…") -> served|NULL`. That handle answers before kl_dl's own
  tracer runs, so FMOD's asset-layer probes would otherwise be invisible.

## Device identity (`runtime/kl_jni.c`)

- `KL_BUILD_<FIELD>=<value>` — override one `android/os/Build` (or
  `Build$VERSION`) String constant, e.g. `KL_BUILD_MODEL="Pixel 6"`,
  `KL_BUILD_MANUFACTURER=Google`. The default is a Quest 2 and that is a
  settled decision — this is the A/B, not a new answer. Two targets present a
  Quest 3 (eureka) instead: batman, which refuses older hardware, and
  steamlink-vr, whose host sizes the stream for the headset it is told of
  (`KL_XR_SYSTEM_NAME` defaults to `Meta Quest 3` there to match).
  `KL_BUILD_PRODUCT=hollywood KL_BUILD_DEVICE=hollywood KL_BUILD_MODEL="Quest 2"
  KL_BUILD_FINGERPRINT=oculus/hollywood/hollywood:10/SQ3A.220605.009.A1/1:user/release-keys
  KL_XR_SYSTEM_NAME="Oculus Quest2"` is the way back for Steam Link (a Quest 2
  streamed 2304x2304 an eye, a Quest 3 2560x2560).

  The override is applied at the single source both readers go through, so
  `Build.MODEL` over JNI and `ro.product.model` through
  `__system_property_get` cannot disagree; making them disagree is the bug the
  `g_sysprops` mapping in `kl_libc.c` exists to prevent.

  It matters more than "Oculus branches": libshell's `BIsVRHeadset()` is
  literally `"<ro.product.manufacturer> <ro.product.model>"` matched against
  `Oculus Quest` / `Pico ` / `HTC VIVE `. (Measured: on `steamlink-vr.apk` it
  does *not* change the 2D→VR handoff, SL-7.)

- `KL_BATTERY_CHARGING=1` / `KL_BATTERY_LEVEL=<0..100>` — what
  `BatteryManager.isCharging()` and `getIntProperty(BATTERY_PROPERTY_CAPACITY)`
  answer. Defaults: not charging, 95.

  These are **telemetry, not control**: Steam Link reads them through
  `IsHmdBatteryCharging()`/`GetHmdBatteryLevel()` and publishes them to the host
  as device properties, which the Steam client shows beside the headset. Nothing
  in the streaming path branches on them, so a wrong answer costs a wrong number
  on someone's desktop. The visionOS frontend publishes UIDevice's real level and
  charging state, and each Sense controller's GCController battery; these knobs
  override those readings for diagnostics. A host run, which has no device
  battery, keeps the 95% / not-charging fallback.

## Video decode (`runtime/media/kl_vtdec.c`, `runtime/media/kl_mediandk.c`)

The decoder is VideoToolbox and it is either there or it is not; what would
otherwise be a knob is a gate instead (`make hevc`, in `make check`). Worth
knowing rather than setting: output is the decoder's **native biplanar YCbCr**
— no pixel format is requested at all, because requesting one costs both the
conversion (BGRA is 2.67× the bytes of NV12) and a copy VideoToolbox performs
on visionOS 2 merely because a format was named. The guest still samples RGB:
`kl_glfb.c` wraps the frame in one of Apple's private single-plane YCbCr
`MTLPixelFormat`s and the SAMPLER converts, which is `samplerExternalOES`'s
whole promise kept in hardware. See `runtime/media/kl_vtdec.h`; the sampled
result is gated against the BGRA path by `make vidtex`.

- `KL_MEDIA_SOUND=0|1` — (`kl_mediandk.c`, `kl_avdec.m`) offer a clip's sound
  to a Unity VideoPlayer as a second extractor track: AVFoundation decodes the
  first audio track to 16-bit PCM (at most stereo), the track says
  `audio/raw`, and the codec behind it passes each buffer through. Default on
  for LES MILLS BODYCOMBAT (x4015163475201433), whose workout clips carry
  their trainer's voice; off elsewhere, where every clip stays silent as
  before. Each codec says `[media] sound codec summary` when it is deleted.
- `KL_VTDEC_REORDER=0|1`, `KL_VTDEC_REORDER_DEPTH=<n>` — (`kl_vtdec.c`) hand
  decoded frames out in presentation order, as Android's MediaCodec does, by
  holding the newest `n` (default 4, at most 7) back sorted on pts. In decode
  order a file with B-frames shows a frame before one that precedes it — a
  picture that jumps back every few seconds. VideoToolbox's own temporal
  processing did not reorder (55 of 120 frames out of order on a host test
  clip; 0 with the hold). Default on for LES MILLS BODYCOMBAT
  (x4015163475201433), whose workout and tip clips carry B-frames.
- `KL_VIDEO_EVEN=0|1` — (`kl_mediandk.c`, `klm_even_cadence`) a clip whose
  frame rate lies strictly between half the display's and the display's own
  (60 fps on a 100 Hz panel, 30 on 50) is put on a grid of two refreshes: each
  frame takes its nearest slot's presentation time and a frame whose slot is
  taken is left out (one in six at 60 on 100). Every frame then stands equally
  long instead of 20-20-10 ms. Says `[media] even cadence: …` when it engages;
  the codec summary counts what it left out. Default on for LES MILLS
  BODYCOMBAT (x4015163475201433); at 120 Hz it never engages.
- `KL_VIDEO_RETIRE=<n>` — (`kl_mediandk.c`, `klm_retire`) keep the last `n`
  video frames a SurfaceTexture showed alive after the next one is latched
  (0..8). Destroying one at once returned its pixel buffer to VideoToolbox's
  pool, which wrote a later frame into it while guest frames already submitted
  still sampled it — a picture that jumped forward and back for a refresh.
  Default 6 for LES MILLS BODYCOMBAT (x4015163475201433), where it was found
  (2026-10-15), and 0 — the old immediate destroy — for everyone else until
  another title is seen with it. The reader heartbeat also counts latches
  whose pts `went back`.
- `KL_UE4_MEDIA_FRAME_LOCK=0|1` — (`kl_jni_ue4.c`,
  `klj_MP14_getVideoLastFrame`) hold `kl_avdec_frame_lock` while the UE4
  MediaPlayer14 path uploads the decoder's frame into the guest's texture, so
  the decode thread cannot write the next frame into the buffer mid-copy (a
  later frame, or half of one, shown early). The same kind of fault as
  `KL_VIDEO_RETIRE`, read off the code rather than seen on a UE4 title; the
  decode thread waits on the upload while it is held. Default 0.
- `KL_JNI_PIN_LOOPER=0|1` — (`kl_jni_looper.c`, `Handler.<init>`) a Handler
  keeps its Looper alive, as in Java. The Looper a HandlerThread hands out is
  a local reference the guest drops once its Handler is built; unpinned, its
  slot was recycled and later messages went to the main queue instead of the
  HandlerThread. Default 1 for LES MILLS BODYCOMBAT (x4015163475201433), whose
  level-start clip showed nothing that way (2026-10-15), else 0.
- `KL_VTDEC_BGRA=1` — request `kCVPixelFormatType_32BGRA` from VideoToolbox
  and take the IOSurface-pbuffer route into ANGLE instead. The A/B for the
  native path, and the escape hatch should VideoToolbox ever hand back a
  format `kl_glfb.c` cannot wrap (it names the fourcc when it refuses). The
  Simulator forces this mode — the private pixel formats do not exist there.
  Host and device. Default 0.

- `KL_VTDEC_DUMP=<path>` — write the elementary stream exactly as the guest
  queued it, as plain Annex-B that `ffprobe` reads directly, plus a sidecar
  `<path>.idx` of `access_unit pts_us bytes` per line. Off by default.

  This is the instrument for every remaining question about the picture, all of
  which are questions about the *bitstream*: what bitrate the host chose, how
  often it sends an IRAP, why the parameter sets change. Without it they can
  only be asked during a live streaming run, which costs a fresh Steam pairing
  and takes the answer with it when it ends. The `.idx` is what makes
  instantaneous bitrate measurable without parsing anything —
  `awk '{b+=$3} END{print b*8/1e6}'` over a window of lines.

  It is also how `make hevc` grows a corpus recorded from a real host instead of
  from ffmpeg. The stream is the guest's own bytes; nothing is re-framed.

## Clock + condvar (bionic→Darwin translation, always on) and their diagnostics

These are fixes, not knobs: bionic clock ids differ from Darwin's, and bionic
condition variables default to CLOCK_MONOTONIC while Darwin conds speak only
CLOCK_REALTIME. Forwarding either verbatim poisons guest time:

- `klb_clock_gettime`/`klb_clock_getres` (`runtime/libc/kl_libc.c`) translate clock
  ids (1/6→6, 4→4, 7/9→8, 2/3→12, 0/5/8→0). Before this, the guest's
  `clock_gettime(CLOCK_MONOTONIC=1)` failed EINVAL and libunity's time
  functions (libunity+0x883c58, +0x883c8c — no return-value check) computed
  with the unwritten buffer: stack-garbage "now", hours to days off.
- `klb_pthread_cond_timedwait` (`runtime/libc/kl_pthread.c`) rebases
  monotonic-based abstimes (ts->tv_sec < 1e8) onto realtime before forwarding.
  Before this, libil2cpp's ConditionVariableImpl built abstimes from a
  monotonic tick source and Darwin compared them against realtime — class-init
  waiters slept until now+hours and never re-checked the class state (the
  loading-pace deadlock: Dns waiters parked on fully-initialised classes).

- `KL_TRACE_TIME=1` — log the first 40 `clock_gettime`/`gettimeofday` calls
  with caller address and result. How the unchecked-failure sites were found.
- `KL_CLOCK_RES_LINUX=1` — `clock_getres` answers 1 ns for the realtime,
  monotonic, raw and boottime clocks, as a Linux kernel with high-resolution
  timers does, instead of Darwin's 1000 ns / 42 ns. For a guest that turns the
  resolution into a clock frequency. Read by `kl_libc.c`. Default on for
  `x5812519008825194` (AC Nexus, whose UbiServices clock otherwise runs 1000×
  fast), off elsewhere; `=0` turns it off there too.
- `KL_TRACE_CONDWAIT=1` — log the first 60 guest `pthread_cond_timedwait`
  calls: tid, return address, ts and delta-from-now. A delta of hours is the
  signature of the poisoned abstime; sane runs read 1 ms – 1 s.
- `KL_CONDWAIT_CAP_MS=<ms>` — clamp every guest condvar abstime to now+ms.
  Diagnostic only (proves a stuck waiter just needs to re-check its
  predicate); the rebase above is the fix.
- `KL_NO_REBASE=1` — disable the monotonic→realtime abstime rebase in
  `klb_pthread_cond_timedwait` entirely. Diagnostic for the capture deadlock;
  breaks class-init waiters.

## Mutex map (`runtime/libc/kl_pthread.c`) — deadlock instruments

Mutexes are keyed by guest *address* (a stale slot index in reused or
memcpy'd storage used to alias two logical mutexes onto one host object —
the capture-path deadlock). Owner tracking is always on; these print from
m_boot's fault handler on every fatal path:

- mutex owners: guest address, holder tid (+thread name, ** EXITED ** if the
  holder died holding it), lock return address, and the holder's current
  backtrace — a hang names the parked owner, not just the waiters.
- mutex waiters: who is blocked on which guest mutex and from where.

Mutexes and rwlocks are keyed by guest address (`mtx_entry_for`,
`rwl_entry_for`): Unreal memcpy's arrays holding locks, and most of its lock
sites never call `pthread_rwlock_init`, so a slot index in guest storage was
believed after a move. A destroyed mutex's entry stays in its bucket and is
taken over by the next one there. Barriers keep a slot index in guest storage;
that side table grows in 4096-slot chunks and logs
`[klb] sync side table g_bar grew past N slots` at each power of two past
65536.

- `KL_TRACE_MUTEX=1` — log the first 200 mutex init/destroy events (guest
  address, slot/map index, tid, return address). How the slot-23 double-init
  was caught.
- `KL_MUTEX_POLICY=fairshare` — create every guest mutex with Darwin's
  `_PTHREAD_MUTEX_POLICY_FAIRSHARE` (hand the mutex to a waiter on unlock)
  instead of the default first-fit, where an unlocking thread that re-locks at
  once nearly always wins. Engines that pass ONE mutex between two threads as a
  baton (`Unlock(); Sleep(0); Lock();`) starve under first-fit: The Simpsons:
  Hit & Run's streaming loader took 2+ s per 1.4 MB zone. Prints
  `[thr] guest mutexes use the FAIRSHARE policy` once. Default off; any other
  value (`firstfit`) is the A/B.

## Guest-thread sampler (`runtime/diag/kl_sample.c`, `runtime/guest/kl_il2cpp.c`)

Built to name the loop the loading-pace arc kept measuring: samples every
guest thread's pc and x29-chained backtrace with thread_get_state, resolves
pcs to module+offset (host frames to host symbols — a thread parked in
`__psynch_cvwait` reads as itself), and resolves libil2cpp generated-code pcs
to managed method names via global-metadata.dat plus the Il2CppCodeGenModule
table recovered from the loaded image (metadata v24.0 only, verified against
Beat Saber 1.28). Read by m_boot; sampling wraps the frame pump, and the
report prints at pump end (or on the watchdog's SIGALRM, via the fault
handler).

- `KL_SAMPLE_MS=<ms>` — sampling interval; presence enables the sampler.
  Small values cost resolution, not correctness: a sample is whatever each
  thread was doing when caught, so more samples just tightens the
  distribution.
- `KL_SAMPLE_REPORT_S=<s>` — print the aggregate report every <s> seconds
  from the sampler thread itself (default 30; 0 disables). Periodic printing
  exists because the runs that matter die by guest abort — kl_fatal_prepare
  resets the signal handlers, so the pump-end and fault-handler reports never
  run on exactly those runs.
- `KL_SAMPLE_STACKS=0` — disable the parked-thread chain dumps. When a thread
  has been sampled 50+ times with exactly one distinct managed frame (i.e.
  parked in one method), the sampler prints its full resolved chain, at most
  3 times per thread. This is what named the Dns::GetHostByName pile-up's
  callers.
- `KL_IL2CPP_METADATA=<path>` — host path to global-metadata.dat. Default is
  derived from the libdir argument (`<apk>/lib/arm64-v8a` →
  `<apk>/assets/bin/Data/Managed/Metadata/global-metadata.dat`).
- `KL_RESOLVE=0xoff,0xoff,...` — (`tests/t_il2cpp.c`) init the managed
  method-name resolver and print the resolution of each given libil2cpp
  offset; the standalone probe for the sampler's resolver.

## Managed-side probe (`runtime/diag/kl_mprobe.c`)

Calls Unity's **own C#** from the host, through the IL2CPP embedding API that
libil2cpp exports (domain → assembly → image → class → `MethodInfo` →
`il2cpp_runtime_invoke`). Every other instrument measures the native side of the
boundary; this one measures the far side, which is where a wrongly-encoded
status answer actually shows up. It is what found the `ovrpResult`-vs-`ovrpBool`
trap (trap 10) and named
`MenuControllers` as the disabled object
behind "no controllers render". Diagnostic only, off unless asked for.

- `KL_PROBE_INPUT=1` — turn it on. Each tick prints:
  - **axes** — `Input.GetAxis` for the eight axes this title binds
    (`TriggerLeftHand`/`Right`, the two sticks, the menu buttons).
  - **OVRInput** — `GetConnectedControllers`, and per hand
    `GetControllerPositionValid` / `PositionTracked` /
    `GetLocalControllerPosition` / `Rotation`. This is the game's own pose
    seam: `VRController::Update` → `IVRPlatformHelper::GetNodePose` → OVRInput
    → `InputTracking` node states → libunity → our ovrp answers.
  - **focus** — `OVRPlugin.hasInputFocus` / `hasVrFocus` / `userPresent`. The
    first two disagreeing is the tell for the trap: `hasVrFocus` is a bare
    `ovrpBool`, `hasInputFocus` is `ovrpResult` + out-param.
  - **xr** — `XRSettings.loadedDeviceName`, `XRDevice.model`, `enabled`,
    `isPresent`. Expected: `device="Oculus" model="Oculus Quest 2" 1 1`. This
    is how the game picks between its four `IVRPlatformHelper` implementations
    — a wrong answer here selects `DevicelessVRHelper`, whose `GetNodePose`
    reports no pose *by design*.
  - **scene** — active/total counts per type (see `KL_PROBE_TYPES`).
  - **VRController** — each controller's `active`, node, position, and its
    **parent chain** with `activeSelf`/`activeInHierarchy` at every level. An
    object can be invisible because a *parent* is off, and only the ancestor
    actually switched off is worth explaining.
- `KL_PROBE_FROM=<frame>` — first frame to probe. **Default 1200, and not
  cosmetic:** the first call into a managed class runs its `cctor`, and touching
  `OVRInput`/`OVRPlugin` before the guest has loaded the plugin drags the
  runtime down paths it would not take on its own. One such run aborted on an
  ovrp entry point the game only reaches from there. A probe that perturbs the
  run it measures is worse than no probe.
- `KL_PROBE_EVERY=<frames>` — interval, default 120.
- `KL_PROBE_ENUM=<Ns.Type[,Ns.Type...]>` — print an enum's **values**, read out
  of the running IL2CPP runtime through `Enum.GetNames`/`GetValues`. Nested
  types with `/` (`Oculus.Platform.Message/MessageType`); printed once, at the
  first probe tick, so it needs `KL_PROBE_INPUT=1` and a `KL_PROBE_FROM` the
  run actually reaches — **and a `KL_PROBE_EVERY` that divides a frame the run
  reaches**, because the tick is `frame % every`, so a large interval on a short
  run simply never fires and reads as the probe being broken.
  Exists because a C# enum's values are the one part of the guest's ABI that is
  in neither its exported symbols nor its strings — `global-metadata.dat` holds
  the names and the numbers live in a constant table whose layout is a
  metadata-version detail. `kl_ovrplat` needs `Message.MessageType` exactly
  right: `Message.ParseMessageHandle` switches on it and its `default` arm logs
  "Unrecognized message type" and produces **no message at all**, so a guessed
  number is indistinguishable from sending nothing. Same argument as
  `OVRP_HEADSET_OCULUS_QUEST_2` — read the value out of the guest rather than
  out of a header we do not have.
- `KL_PROBE_FIND=<part>[,<part>...]` — once, from frame 600: every managed
  method whose name contains one of the parts, as
  `Namespace.Class::Method(types) [static]`. A part that starts with `@` is
  matched against `Namespace.Class` instead (`@Mighty.Tester.` lists a whole
  namespace). How a guest's own functions are found when all there is of them
  is names in global-metadata.dat. Independent of `KL_PROBE_INPUT`.
  Each method line ends in `@libil2cpp+0x...`, its methodPointer as an offset
  into the file: where to start `llvm-objdump -d --start-address=` on it.
- `KL_PROBE_CALL=<Namespace.Class>::<Method>` — call one of the guest's own
  parameterless methods once, at frame `KL_PROBE_CALL_FROM` (default 3000). A
  static one as it is; an instance one on the class's `Instance`, else on the
  object `FindObjectOfType` finds, else (`KL_PROBE_CALL_CREATE=1`) on a new
  GameObject given the component. Walkabout Mini Golf carries its developers'
  test driver: `Mighty.Tester.MightyTestManager::StartFullPerformanceSequence`
  loads the courses one after another without anybody playing.
  `KL_PROBE_TIMESCALE=<x>` sets `UnityEngine.Time.timeScale` from the same
  frame on (again every 120 frames), for a sequence that waits in game seconds.
- `KL_PROBE_KEEP=<Namespace.Class>::<field>=<part>[|<part>...]` — just before
  `KL_PROBE_CALL`: the field (on the class's `Instance`) is a List of Unity
  objects; keep those whose name contains one of the parts and drop the rest.
  `KL_PROBE_KEEP_FIELD=<string field>` matches that field of each element too
  (as a whole). No parts: only print the list. Walkabout's test manager walks
  `coursesToTest`; `...::coursesToTest=Vegas` makes it two courses instead of 86.
- `KL_PROBE_STRINGS=<Namespace.Class>::<field>` — a List of strings on the
  class's `Instance`, printed as it grows (a test driver's own results).
- `KL_PROBE_FIND` with an `@Class` part lists that class's fields as well.
- `KL_PROBE_SCRIPT=<frame>:<Namespace.Class>::<Method>[(args)][#n];<frame>:...`
  — several calls like `KL_PROBE_CALL`'s, each at its frame (rising), separated
  by `;`. Arguments, up to four: a quoted one is a string, `true`/`false` and
  whole numbers go as the value they are; the overload is picked by their
  COUNT, so a method with two one-argument overloads gets whichever comes first
  (`MultiplayerLauncher::LoadScene` took a string for a `CourseInfoAsset` and
  threw). With `#n` the call goes to the n-th object of that class in the
  loaded scenes (inactive ones too, by instance id), and the line says that
  object's name. How Walkabout is walked without its menu:
  `XRRig::LoadScene("Vegas_Easy")` loads a course from wherever the game is —
  the name's capitals matter, `vegas_easy` loads nothing —,
  `Mighty.Homestar.HomestarManager::GoToBasement` enters the Homestar Runner
  basement (part of the main menu), `Mighty.Homestar.HSLauncher::Launch#n`
  starts one of the 36 launchers there and
  `Mighty.Homestar.HomestarMenuUtility::ReturnToBasement` comes back.
- `KL_PROBE_KEEP` without `KL_PROBE_CALL` only lists (and filters) at
  `KL_PROBE_CALL_FROM` — how `tools/pipeline_tour.py` reads the names a guest
  loads its scenes by.
- **An object "not there" was once this probe's fault.** `KL_PROBE_CALL` looked
  an instance up with `FindObjectOfType`, by name and argument count — which a
  generic overload shares, so the lookup could return that one and find
  nothing. It now asks the three-argument `FindObjectsByType`, the one of the
  family no generic overload shares the count with.
- `KL_PROBE_XR=1` — print `XRSettings`: the loaded device name, `enabled`, the
  **stereo rendering mode**, the eye texture size and the render viewport scale.
  Every one of those is a number WE answered (through ovrp), and this is the far
  end of the round trip — Unity's own conclusion about the display rather than
  what we said. It is what cleared the display of suspicion when BONELAB threw
  once a frame, and then what confirmed the fix (`stereo=3
  (SinglePassMultiview)`). Independent of `KL_PROBE_INPUT`.
  `KL_PROBE_XR_FROM` (default 60) and `KL_PROBE_XR_EVERY` (300) pick when.
  **Sample LATE.** The pump's frame counter is not Unity's: the first run of
  this landed before `XRApi: XR Subsystem Start` and reported `enabled=0
  eyeTexture=0x0`, which is not a measurement of anything. Frame 400+ on
  BONELAB.
- `KL_PROBE_STACKTRACE=<0|1|2>` — force Unity's stack traces on for every
  LogType (`Application.SetStackTraceLogType`; 0 None, 1 ScriptOnly, 2 Full,
  default 1), **printing what it found before changing it**. That report is the
  point: it separates "the game turned traces off" from "this build has none".
  BONELAB already answers `Exception: ScriptOnly` and still emits an empty
  stack, so it is a dead end there: do not retry it.
  Independent of `KL_PROBE_INPUT`; runs once, at the first tick.
- `KL_PROBE_TYPES=<a,b,...>` — types to census, as `Namespace.Type` (bare name
  for the global namespace); default is the menu pointer chain
  (`VRUIControls.VRPointer,…VRInputModule,…VRLaserPointer,…VRGraphicRaycaster`).
  Prints `active/total`: `FindObjectsOfType` sees only components on **active**
  objects, `Resources.FindObjectsOfTypeAll` includes inactive ones, and the gap
  between the two is the measurement — `VRLaserPointer=0/1` means the laser
  exists and is switched off.

- `KL_PROBE_CLASSAT=<hex>[:<offset>][,…]` — which class a metadata slot of
  libil2cpp holds, with its static fields. Compiled code reaches a class through
  such a slot (`adrp`/`ldr` of a fixed address, then `[class, #0xb8]` for the
  statics); the number is the slot's address as `llvm-objdump` prints it. With
  `:<offset>` the static at that offset is an object, and its class is named
  too. Printed at frame 600 and again at `KL_PROBE_CLASSAT_AGAIN` (default
  2400): a slot says "not initialised yet" until the function that uses it has
  run once. `KL_PROBE_FIND` gives the function's address to disassemble.
- `KL_PROBE_WATCH=<hex>:<offset>[><offset2>][p][,…]` — a static byte of the
  class behind such a slot, printed with the frame number whenever it changes;
  with `><offset2>` the static is an object and the byte is read inside it; a
  trailing `p` watches a pointer for being set at all. For a flag a guest waits
  on: when it flips, or that it never does. How Walkabout's start was read
  (2026-10-13): `PlayFabController.IsLoggedIn` stayed 0 for a build whose HTTP
  requests failed, and the game sat out its own offline wait.

**One caveat, recorded so it is not trusted.** The probe also prints
`eventsystem: overUI=...`, which stays `no` even when the pointer works:
`EventSystem.IsPointerOverGameObject()`'s zero-argument overload asks about
pointer id −1 (the mouse), while this title's `VRInputModule` uses its own id.
It is a blind detector, not evidence that the ray misses.

## IL2CPP metadata dump (`runtime/diag/kl_metadump.c`)

Writes the guest's **decrypted** `global-metadata.dat` out of its own memory
after `il2cpp_init`. Built for VRChat, whose file is protected — it begins
`0xad6b5f05` instead of IL2CPP's `0xFAB11BAF`, the string table is plaintext and
the index tables are not — and whose embedding API is name-mangled, so
`kl_mprobe` cannot attach either. Diagnostic only, host-only, does nothing
unless asked.

- `KL_DUMP_METADATA=<path>` — turn it on and name the output file. Scans this
  process for the blob and writes the first candidate. Three searches, and the
  reason there are three is that each one alone can be defeated:
  - **IL2CPP's own magic** `0xFAB11BAF`, validated by *measuring* the header —
    the first table's offset is the header's length and therefore the pair
    count, every non-empty table must lie inside the blob, and the furthest
    reach is the total. A bare four-byte match in a multi-gigabyte process is a
    coincidence waiting to be written out as a dump.
  - **the guest's own on-disk magic**, whose header cannot be measured (the
    pairs are ciphertext), so the file's length is the extent and
    `klmd_compare_disk` supplies the evidence: a copy BYTE-IDENTICAL to the file
    is the ciphertext where it was read, not a decrypted one, and it says so.
  - **the header's SHAPE with no magic at all**, for a loader that decrypted the
    tables without putting the sanity value back. Accepted only if the tables
    reach **exactly** the file's length. The weaker version of this test was
    tried and measured: accepting any pair-structured header whose version word
    reads 16..40 produced **more than thirty** hits in one VRChat run and wrote
    116 MB of something else out as the answer. Pair-structured data is common;
    the length equality is the only discriminator that holds.
- `KL_DUMP_METADATA_AT=init|end` — when to scan (default `init`, just before the
  frame pump). `end` scans after the pump instead, for a loader that decrypts
  **lazily**: at init only what `MetadataCache::Initialize` touched is plaintext.
  `init` is the default because a guest that dies in the pump still gets a dump.
- `KL_DUMP_METADATA_RO=1` — also scan read-only regions. Off by default because
  a decrypted blob is written, so it is in writable memory, and the default
  keeps the scan off every library mapping.
- `KL_DUMP_METADATA_SRC=<path>` — the on-disk `global-metadata.dat` to compare
  against, overriding the target's own.

The scan only ever touches **resident** pages (`mincore`, cached per page) and
skips to the next page rather than the next word when one is cold. That is not
an optimisation: a word read out of a cold anonymous page faults it in, so a
naive walk of a reserved range does not merely take forever, it allocates it —
an instrument whose cost is the guest's own footprint.

**Measured on VRChat and recorded so it is not re-run**: after 250 frames, over
**6.8 GB** of resident memory, read and write, there is no `0xFAB11BAF`
anywhere, no header shape reaching the file's length, and exactly two copies of
the blob — both **byte-identical to the file**. This title never materialises a
plaintext metadata image, so the plan of "dump it after init and feed
Il2CppDumper" does not work here. The instrument stays because it is general and
the answer for the next target is one run.

**And the conclusion drawn from that was wrong in an interesting way.** "No
plaintext image" is true; "so it must be decrypted per access" is not. VRChat
decrypts the file exactly once, into **eight separate heap buffers**, so no
contiguous copy of it ever exists and there is nothing for a scan to find. The
scheme itself is now reversed and lives in `tools/vrc_metadata.py` — read its
header comment before touching any of this.

### `KL_META_WATCH` — who READS the metadata

The instrument that found it, and the general one: `KL_DUMP_METADATA` answers
"is there a plaintext image?", and when the answer is no the only thing left is
the code that does the decrypting. On this target that code cannot be found
statically at all — `libil2cpp.so`'s runtime `.text` makes **zero** direct PLT
calls (every import is reached indirectly) and its exported entry points are
trampolines through a table filled at load, so neither a string xref nor a call
graph from `open`/`mmap` has anything to walk from.

It can be found dynamically, because the mapping is ours. This takes the pages
away with `mprotect(PROT_NONE)` and lets the readers announce themselves as
faults; the handler names the site, opens the region, and a watchdog thread
closes it again, so one run yields a census rather than a single hit.

- `KL_META_WATCH=1` — arm it. Reports the fd `global-metadata.dat` was opened
  on, **every file-backed mmap by path** (asked of the fd with `F_GETPATH`, not
  matched against the open trace — this guest maps the file on a *second*
  descriptor that the open trace never sees), a stack **x-ray** at the mapping,
  and then one line per distinct reader as `<image>+0x<off>` with the metadata
  offset it first touched.
- `KL_META_WATCH_MS=<ms>` — how often the watchdog re-closes the pages
  (default 50). Smaller finds more sites and costs more faults.
- `KL_META_WATCH_FRAMES=<n>` — how many of the early `read()`s print their
  frames (default 3).

The stack x-ray is the part worth keeping: `kl_fault_print_frames` walks x29 and
this guest keeps no frame chain, so the ordinary instrument stops at the first
frame — and WHO asked for the metadata is the entire question. A conservative
scan of the stack for words pointing into a guest image prints the chain with
some stale words mixed in, which is a checkable answer rather than no answer.

Two supporting hooks exist for it and are NULL unless it installs them:
`kl_file_watch` (guest file operations, in `kl_libc.c`) and `kl_shim_override`
(a diagnostic's answer for a shim symbol, consulted by `kl_shim_lookup` before
any tier — it is how the watch wraps `read` without a permanent wrapper in the
shim). Both cost one null check on a path that is not hot.

## XR runtime / controllers (`runtime/xr/kl_ovrp.c`)

Poses live in the space the guest asked for with `ovrp_SetTrackingOriginType`.
Beat Saber asks for **FloorLevel**, so y=0 is the floor and a head belongs at
standing eye height — `kl_ovrp_tracking_origin()` reports which space is in
force, and a frontend that reports an eye-level head into a floor-level world
puts the camera on the ground with its hands underneath it.

- `KL_OVRP_DERIVE_VELOCITY=0` — stop deriving linear/angular velocity from
  successive poses, so a frontend that publishes pose only reports its motion as
  UNKNOWN (OpenXR `velocityFlags == 0`) instead of measured. Default on.
  `KleptonControllers.swift` publishes real motion off the Sense controllers and
  is unaffected either way; the macOS viewer and the parked default poses go
  through the pose-only call and are what this derives for. What it must never
  go back to is the third option — velocity **zero and valid**, which is not
  silence but the assertion that a controller whose position is changing in the
  same sample is stationary. `kl_openxr` was already careful to distinguish
  those; the zeros defeated it from the publishing side.
- `KL_OVRP_STICK_MOVE=0|1` — move the complete guest tracking rig with the
  left thumbstick, relative to the head's horizontal facing direction. Head and
  both hands receive the same accumulated X/Z offset, so this is artificial
  locomotion rather than a detached camera. Default on only for Richie's Plank
  Experience (`x1642239225880682`), whose room-scale plank can cross visionOS's
  fixed immersive safety boundary; off for every other target. A radial 0.20
  deadzone prevents drift.
- `KL_OVRP_STICK_MOVE_SPEED=<metres/second>` — full-deflection speed for the
  artificial locomotion above. Default **1.0**.
- `KL_OVRP_EYE_HEIGHT=<metres>` — standing eye height for the *default* head
  pose, i.e. what a headless run reports when no frontend has called
  `kl_ovrp_set_head_pose`. **Default 1.6**, and forced to 0 when the guest
  selected the EyeLevel origin (there the eye already is the origin). This is
  not cosmetic: with the head at y=0 under FloorLevel the menu renders above
  the view and the default hands sit ~1.7 m below the frustum.
- `KL_OVRP_HANDS_IN_VIEW=1` — park both hands at a fixed spot inside the
  *current head's* frustum (head-relative, 15 cm out, 12 cm down, 55 cm
  forward), overriding whatever the frontend last wrote. Read side, so it
  beats the viewer's per-frame writes. Answers one question only — does the
  guest draw controllers at all. Diagnostic: the hands do not move with your
  real hands. The offset was absolute tracking-space coordinates until the
  FloorLevel origin was measured, which parked them below the floor and out
  of frustum — the knob meant to prove the controllers render was hiding them.
- `KL_OVRP_POKE="<when>:<BUTTON>[+<BUTTON>],..."` — a scripted controller
  sequence, so a run can drive **itself** past a title screen and into a menu.
  `<when>` is either seconds (`12:A`) or, better, a guest frame (`f9900:A`) —
  the same number `KL_VK_OUT` puts in `vk_f09900_*.png`, so a script worked out
  by looking at the captures lands on the screens they show. Seconds cannot do
  that on a UE4 guest: startup is dominated by the engine's one-time shader
  optimization, which is minutes and is not the same length twice (the seconds
  clock starts at the guest's first controller read, not at exec, for the same
  reason). Names: `A B X Y START BACK RTRIG LTRIG RGRIP LGRIP UP DOWN LEFT
  RIGHT`, right hand unless the name says otherwise (X/Y and the L* pair are
  the left controller, which is OVRPlugin's own convention). A misspelt name is
  NAMED, not skipped — a press that never happens reads as the guest ignoring
  input. `KL_OVRP_POKE_HOLD_MS` (250) is how long each press is held.
  Merged where the guest **reads** its controller state rather than written
  into the published input, so it survives a frontend publishing every frame
  and works with **no frontend at all**: a headless `./build/m_boot re4` gets
  the same presses as the viewer, and both OVRPlugin and OpenXR guests see
  them. KL_VIEW_POKE's reasoning, in the seam a VR guest's input arrives
  through.
- `KL_OVRP_FAKE_BUTTONS=<hex>` — OR this mask into both hands' `Buttons` and
  `Touches` on a duty cycle (~256 polls on, 256 off), so `GetDown`/`GetUp`
  edges actually occur; a bit held from boot never reads as a press. Which
  bits reach the game, measured from libunity's joystick fill (`0x9bd338`
  left, `0x9bd548` right) against this title's InputManager:

  | ovrpButton | joystick button | this title's axis |
  |---|---|---|
  | `0x1` A | 0 | `MenuButtonRightHand` |
  | `0x2` B | 1 | — |
  | `0x100` X | 2 | `MenuButtonLeftHand` |
  | `0x200` Y | 3 | — |
  | `0x4` RThumbstick click | 9 | `MenuButtonRightHandOculusTouch` |
  | `0x400` LThumbstick click | 8 | `MenuButtonLeftHandOculusTouch` |
  | `0x100000` Start | 7 | — |

  Every other bit is read by nobody — **including the raw trigger bits**
  (`0x04000000`/`0x08000000`/`0x10000000`/`0x20000000`), because libunity
  carries the triggers as float *axes*, never as buttons. So this knob cannot
  produce a UI click, and "nothing reacts with `0xffffffff`" says nothing
  about the trigger. Use `KL_OVRP_FAKE_TRIGGER` for that.
- `KL_OVRP_FAKE_TRIGGER=<0..1>` — drive both index triggers to this value on
  the same duty cycle (bare/empty means 1.0). This is the click: Beat Saber's
  `VRControllersInputManager` reads `Input.GetAxis("TriggerLeftHand"/"…Right")`,
  which the InputManager asset binds to joystick axes 8 and 9, and libunity
  fills those from `LIndexTrigger`/`RIndexTrigger`. The only way to exercise a
  menu click without the interactive viewer.
- `KL_OVRP_HANDS_SWEEP=1` — collapse both hands onto the head and sweep their
  pitch −70°..+70° in 5° steps, ~256 frames a step (two full
  `KL_OVRP_FAKE_TRIGGER` presses each), logging each step. The ray a controller
  casts is *not* the pose we report: `IVRPlatformHelper.AdjustControllerTransform`
  rotates the controller transform by a device-specific offset that is game
  data we cannot read. A sweep does not need to know it — if any pitch produces
  a UI hit the offset is the only unknown left; if none does over 140°, the ray
  is not the problem. Pair with `KL_OVRP_FAKE_TRIGGER=1`.
- `KL_OVRP_DUMP_VRDEVICE=1` — dump libunity's own Oculus VRDevice object once:
  the three unique device ids it stamps into both the joystick descriptors and
  the XR node states, then every function-pointer slot **named** by matching it
  against what `kl_ovrp_sym` handed back. This is the whole VRDevice↔plugin
  contract in one place, so a status predicate answered wrong can be found by
  name instead of by chasing `ldr x8, [x?, #N]` offsets. Reads a build-specific
  vaddr (`libunity+0x127a6c0`), which is why it is opt-in.
- `KL_OVRP_IPD=<m>` — force a symmetric head→eye separation, overriding
  whatever the frontend pushed. The guest's IPD arrives *only* through
  `ovrp_GetNodePoseState` nodes 0/1, so this is the A/B for
  "is the compositor's number wrong" — and on the host, where nothing pushes
  one, the only way to get stereo at all. Refused outside 0..0.2 m.
  **Required for host Steam Link VR runs (SL-12).** Its VR client publishes the
  eye-to-head transform and the raw projection params only when the half-IPD it
  reads differs from the one it last sent, and that starts at zero — so with the
  offsets unpushed it decides its geometry is unchanged, never tells the Steam
  host how to project, and the host silently streams no video at all. `=0.063`
  is what makes frames arrive. On device the compositor measures and pushes the
  real offsets, so this stays what it has always been there: the A/B for "is the
  compositor's number the wrong one".
- `KL_OVRP_STAGES=<n>` — eye-swapchain depth (default 3 — 2 for
  `x5812519008825194`, whose per-stage multisampled targets are real memory
  here and put a level load at the jetsam limit with three — clamped to
  1..max). `=1` restores the earlier single-buffered behaviour, which every
  measurement was taken against, and its tearing; each extra stage costs a
  full-size RGBA16F two-slice eye texture (~160 MB at map resolution). The
  A/B in both directions.
- `KL_OVRP_TRACE=1` — log the live call SEQUENCE, with a global ordinal and the
  caller's image+offset, for the frame/layer/display family; `=all` for every
  entry point (the input pollers run thousands of times a frame and bury it).
  The end-of-run work list prints TOTALS per name, and totals cannot say
  whether the guest re-enters the submit path per frame or entered it once and
  left: with the sequence, `BeginFrame4` once then `DestroyLayer` reads as a
  teardown; without it, as a call count of 1. A print and nothing else — no
  register capture, no reads of guest memory.
- `KL_OVRP_COLOR_SCALE=0|1` — apply `ovrp_SetColorScaleAndOffset` to the eye
  picture (colour * scale + offset, in the reprojection's fragment shader). It
  is how a game fades to black and stays black while it loads; dropped, Batman:
  Arkham Shadow showed its level being put together where the Quest shows
  black. Default 1; 0 accepts and ignores the call, as before. The log names
  each change of value (`[ovrp] eye colour scale ...`).
- `KL_OVRP_EYELESS_FRAMES=0|1` — an `ovrp_EndFrame4` submit list with no eye
  layer turns the compositor's eye pass off until one is submitted again, so
  such a frame shows its quads over black as on a Quest. Otherwise the eye
  textures' last contents stay on screen; Skydance's BEHEMOTH submits quads
  alone on its splash and level-loading screens, and what showed behind them
  was undrawn eye storage, a magenta flicker; Asgard's Wrath 2 does the same
  while a level loads, with three stale eye pictures shown in turn and not the
  same one for both eyes. Alien: Rogue Incursion (`x24882487241367228`,
  Vulkan) submits one loading quad alone while a level loads, and its eyes
  differed there: no stage is observed on Vulkan, so the stages went round by
  frame counter under the last eye viewport, at the different sizes its
  dynamic resolution had left them. Into The Radius 2
  (`x6605328856182465`, Vulkan) submits three quads alone from its second
  frame on while it loads, and its picture flickered. Default 1 for
  `x5767635983306831`, `wrath2`, `x24882487241367228` and
  `x6605328856182465`, else 0. While the eye pass is off this way the compositor draws black behind
  the frame's quads (a mixed space shows the room wherever no depth was
  written).
- `KL_OVRP_GL_EYE_FENCE=0|1` — a GL guest's `ovrp_EndFrame4` with an eye layer
  publishes a GPU fence (`kl_glfb_frame_fence`) on the submitting thread before
  the frame record announces the stage. For a guest that draws straight into
  its eye textures and stops swapping once gameplay starts: the swap-driven
  fence then stays fixed while the records advance, and the compositor can
  take a stage's old picture under its new pose — head jitter at a steady
  display cadence. Default 1 for `x2285803881494677` (Gun Club VR build 410:
  jitter at reduced resolution and with foveation off, stable head movement
  with the fence, as its fix was sent in), else 0.
- `KL_OVRP_LAYERS=1` — census the whole `ovrp_EndFrame4` submit list: one line
  per distinct `(layer, shape, stage, viewport, pose, flags)`, printed when it
  first appears and when it changes, plus the union's per-shape tail. Every
  other reader of that array walks past anything that is not the eye layer, so
  a guest submitting OVERLAY layers looked from every log here exactly like one
  submitting none — RE4 draws its intro logos as an `ovrpShape_Quad` splash and
  the run said nothing about them. A title that submits one eye layer forever
  pays one line for the run, so this is cheap to leave on. (RE4 carries
  what it measured and where the quad's world size lives.)
- `KL_OVRP_LATCH=0` — restore the live per-call pose read instead of latching
  head+hands once per frame at `ovrp_Update2` (the guest's real per-step latch
  point). The A/B if the pinning is ever suspected of costing latency;
  controller *buttons* stay live either way.
- `KL_OVRP_EYE_CANT=0` — ignore the per-eye rotation the frontend pushes
  (`kl_ovrp_set_eye_rotation`) and restore the dropped-cant behaviour as the
  A/B. Identity by default, so host and headless runs are unchanged.
- `KL_OVRP_EYE_TAN=vision` or `=l,r,t,b` — force a **canted** pair of eye
  frusta, overriding whatever the frontend measured. `vision` is Vision Pro's
  own (eye 0 `l=1.73205 r=1.0 t=1.0 b=1.19175`, eye 1 the horizontal mirror);
  the four-number form takes eye 0's cone and mirrors it. **This is what makes
  the canted-display failures reproducible on the host**: with the default
  symmetric `{1,1,1,1}` a guest that collapses the two cones is
  indistinguishable from one that honours them, every per-eye viewport comes
  back full width, and the only instrument left is a person in the headset.
  Pairs with `KL_VIEW_EYE=1`. Read on the same path as the union below, so it
  overrides the compositor on device too.
- `KL_STALL_MS=<ms>` — the stall watch (`runtime/diag/kl_stall.c`): when an
  OpenXR guest submits no frame for that long, every thread is sampled while the
  gap lasts and, when frames resume, the gap's length and the stacks of the
  threads that were not waiting are printed. Off unless set; 250 for `alyx`.
  An OVRPlugin guest's frame is stamped as well (`ovrp_EndFrame`); 100 for
  `walkabout`, Demeo and `batman` while their stutter is being looked into.
- `KL_GUEST_FRAME_STATS=<seconds>` — (`visionos/Sources/kl_app.c`) every so
  many seconds one line about the Unity frame loop: how long the guest's frames
  took against the display's period, how many took more than one and more than
  two periods, and how old the offered pose was when a frame started. Off (0)
  unless set; 10 for `walkabout`, Demeo and `batman`.
- `KL_GLFB_SLOW_MS=<ms>` — (`runtime/gfx/kl_glfb.c`, GLES guests) name every
  shader compile, program link and draw that took longer than this, and sum up
  the shader work between reports. Off (0) unless set.
- `Documents/klepton-shader-test.txt` (a file on the device, not a variable;
  `visionos/Sources/kl_app.c`) — one word. `cold`: Metal's own cache in the
  app's container is set aside as `com.apple.metal.kept` and every start runs
  with an empty one, to look at what a game shows while it compiles everything
  (the replay of recorded pipelines, `KL_VK_FOZ_REPLAY`, is off for such a start).
  `restore`: the kept cache is put back. Put there with
  `devicectl device copy to`; the boot log's head says what was done.
- `KL_VK_CACHE_WAIT_MS=<ms>` — (`runtime/gfx/kl_vulkan.c`) how long
  `vkCreatePipelineCache` waits for MoltenVK to read back a pipeline cache the
  guest saved (1 MB or more). MoltenVK compiles every shader in it inside that
  call: 0.2 s while Metal's own cache is warm, 95 s when it is not (Asgard's
  Wrath 2, 30 MB, on a Mac) — a black start. Past the wait the guest gets an
  empty cache and starts as after a fresh install, showing its own shader
  screen where it has one, and the read finishes on a thread behind it, which
  warms Metal's cache for the next start. Default 1500; 0 reads in place, as
  before.
- `KL_OVRP_HEAD=x,y,z[,yaw]` — a head pose for a run with no frontend (metres,
  yaw in degrees). Without it an OpenXR guest's head is on the floor at the
  origin, which Half-Life: Alyx answers with its orange out-of-bounds fade.
- `KL_OVRP_HEAD_SWEEP=<deg>,<s>[,<from>]` — with `KL_OVRP_HEAD`: the head turns
  left and right about its yaw, `<deg>` each way over a period of `<s>` seconds,
  from `<from>` seconds after the head is put (default 0, so scripted menu
  presses can come first), so a Mac run has a moving head (frame generation,
  motion vectors).
- `KL_VK_ASYNC_PIPES=0|1|2` — graphics pipelines are compiled on worker threads
  instead of in the call that creates them (default 1 for `alyx`, else 0). The
  creating call waits at most `KL_VK_ASYNC_WAIT_MS` (default 8, and a tenth of
  elapsed time across all creations); a compile that takes longer is answered
  with a handle of the layer's own, a bind of it binds nothing until the
  pipeline is there, and the draws that follow on that command buffer are
  dropped. 1 = only pipelines that render with a depth attachment (views of the
  scene and shadow maps, redrawn every frame); 2 = every graphics pipeline (test
  only: an interface panel rendered once would keep the hole).
  `KL_VK_ASYNC_DELAY_MS=<ms>` makes every compile that much slower, to see the
  cold case on a machine whose Metal shader cache is warm.
- `KL_VK_ASYNC_RP=0|1` — `KL_VK_ASYNC_PIPES` for pipelines made for a
  `VkRenderPass` as well (default 0: Source 2 uses dynamic rendering, and its
  setting stays what was measured). Unity 2022 makes all of its pipelines this
  way (Arkham Shadow). In mode 1 the depth test is the subpass's: one with a
  depth/stencil attachment, noted when the pass is made. A pass that is
  destroyed waits for the compiles under way. The start window's test switch
  "Shaders: compile behind the picture" sets both; it is on from the start
  for `batman` (targets.py `test_on`, which adds a game's own names to
  `KLTestOn`). The log's
  `pipeline warmup` line says `background gfx` for a compile done behind the
  frame.
- `KL_VK_PIPE_REC=<file>` — one line per graphics pipeline the guest creates:
  FNV hash of each stage's SPIR-V, sample count, attachment formats, view mask,
  compile time. (How it was established that the game's shipped
  `vulkan_shader_cache.foz` holds none of this build's shaders.)
- `KL_VK_VIN_FILL=0|1` — a graphics pipeline MoltenVK refuses because its
  vertex shader reads an input location the pipeline's vertex input state does
  not supply ("Vertex attribute N is not defined in the vertex descriptor",
  "Function requires stage_in attributes but no descriptor was set") is created
  again with those locations reading zeroes from a buffer of the layer's own.
  Pipelines that compile are untouched. Default 1 for `x24882487241367228`
  (Alien: Rogue Incursion), `x5245041552210029` (Arizona Sunshine 2) and
  `x9269393463181825` (Roboquest VR, Meta Store build: its start-up shader
  precompile stopped at the first refusal), else 0.
- `KL_VK_VIN_INT=0|1` — (only with `KL_VK_VIN_FILL` on) the same retry also
  takes a location the state supplies as 32-bit floats but the shader reads as
  int/uint ("Cannot convert attribute from MTLAttributeFormatFloat2 to int4 or
  uint4"): it is declared again with the integer format of the same size,
  which hands the shader the same bits. Default 1 for `x5245041552210029`
  (Arizona Sunshine 2), else 0 — Alien keeps such a pipeline refused, as it was
  measured.
- `KL_VK_GRID_DILATE=0|1` — conservative rasterization for draws through a
  100x60 viewport: each is drawn again four times with the viewport moved by
  ±0.49 pixel, so every pixel a primitive touches gets a fragment. HITMAN 3 VR:
  Reloaded sorts the gates between its rooms and its lights into two 100x60
  screen grids that way, and a lighting shader takes a pixel's room and lights
  from its cell; cells a slanted edge only grazed were left out here (not on the
  Quest) and lit as the wrong room — lighter steps along the window in the VR
  tutorial's third room and along shadow edges. Default 1 for
  `x8012387035448196`, else 0.
- `KL_VK_SUBPASS_MS_SAMPLE=0|1` — a fragment shader that reads a
  MULTISAMPLED subpass input (OpTypeImage Dim=SubpassData MS=1) with a bare
  subpassLoad — an OpImageRead with no Sample operand, which the spec forbids
  and Quest's driver tolerates — is given `Sample 0`. Without it SPIRV-Cross
  emits `texture2d_ms_array::read(coord, layer)` and Metal refuses to compile
  the pipeline. Default 1 for every Unreal guest, else 0 (seen with Pinball FX
  VR: its UE depth-fetch attachment; every such PSO failed and UE called
  ForceQuit about twenty times a run).
- `KL_VK_NO_TRANSIENT=0|1` — create `TRANSIENT_ATTACHMENT` images as ordinary
  ones, so MoltenVK never backs them with memoryless (tile-only) storage. A
  memoryless attachment does not survive MoltenVK ending one Metal pass and
  opening another inside a Vulkan pass (see `KL_VK_PASS_BARRIERS`); the pass
  after the seam reads undefined tile memory, which shows as a picture drawn in
  32x32 tiles, each all there or all missing. Costs the memory of the
  multisampled scene targets. Default 1 for `x5767635983306831` (Skydance's
  BEHEMOTH), `x6605328856182465` (Into The Radius 2, whose main menu showed
  blocks of garbage, different per eye and per frame, without it) and
  `x9269393463181825` (Roboquest VR, Meta Store build: a black scene with
  glimpses of models under its UI), else 0.
- `KL_VK_PASS_BARRIERS=0|1|2` — what happens to a `vkCmdPipelineBarrier`
  recorded INSIDE a dynamic-rendering pass that only makes colour attachments
  readable as input attachments. MoltenVK answers one by ending the Metal pass
  and opening another (store everything, load everything). `1` keeps it
  (default). `0` drops it and stores the pass's multisampled colour as the pass
  ends, so that whatever reads it reads the previous frame (`alyx`: its
  exposure meter). `2` holds it back until a pipeline that reads a subpass input
  is bound. `KL_VK_SUBPASS_DUMP=<dir>` writes the SPIR-V of every shader that
  reads one.
- `KL_VK_WIDEN_AREA=1` — give a multisampled pass that loads nothing its whole
  attachments as render area, so MoltenVK can clear and discard instead of load
  and store. Default 0, 1 for `alyx`. `KL_VK_WIDEN_TRACE=1` says what it saw.
- `KL_VK_FOVEATE=<edge>` — foveate a Vulkan guest's multisampled scene pass
  underneath MoltenVK (`kl_mtl_foveate.m`): a rasterization rate map on that
  pass, and an unwarp of its resolve target as it ends. `<edge>` is the rate at
  the widest edge of the cone; `KL_VK_FOVEATE_INNER=<tan>` (default 0.30) is how
  far out the rate stays 1. `KL_VK_FOVEATE_TRACE=1` prints the candidate passes,
  and, some frames into a level, every Metal encoder of two frames with the call
  stack that ended the scene pass. Off unless set.
- `KL_VK_HEAP_MB=<n>` — tell the guest the GPU has at most `n` MB (heap size and
  `VK_EXT_memory_budget`), instead of MoltenVK's figure, which on a headset is
  more than the app may use in total. Off unless set.
- `KL_VK_NO_QUERY=1` — drop `vkCmdBeginQuery`/`vkCmdEndQuery`. A diagnostic.
- `KL_OVRP_EYE_TAN_MAX=<t>` — tell no eye a cone wider than tangent `t` in any
  direction (Vision Pro's temporal side is 1.73 = 60 degrees; `1.3` is 52). The
  eye texture is narrowed in proportion, so density is kept and the far
  periphery's pixels are not drawn; beyond the picture the composite shows its
  clear colour. Ignored while `KL_OVRP_UNIFY_FRUSTUM` is on.
- `KL_OVRP_UNIFY_FRUSTUM=1` — tell **both** eyes the union of the two cones.
  **Default 0 as of 2026-08-13**; it used to default to on for a multiview
  guest, which was the stopgap for BONELAB's warped right eye. That is now
  understood — it is Oculus symmetric projection, the plugin computes the same
  union itself and submits a per-eye `ViewportRect` saying where each eye's
  picture landed, and the composite honours those. Told the
  union, the guest stops widening its own eye texture and each eye loses ~21% of
  its horizontal pixels. Renders correctly on device with it **off**
  (2026-08-13, user-confirmed), so 1 is now purely the A/B — and the answer for
  any guest ever found that collapses the cones without submitting per-eye
  rects, which would be unservable any other way. Later defaults: on for
  Into The Radius, Walkabout, Demeo, Ghost Town, Contractors
  (`x2436897736439055`, UE 4.27) and, under the profile for
  untested games, every Unity title shipping libOculusXRPlugin — except Angry
  Birds VR (`x2718606324833775`), which places each eye itself and looked flat
  with the union (2026-10-18).
- `KL_VRR` — **foveated guest rendering** (host: `tests/t_mtl_provider.m`;
  device: `KleptonCompositor.swift`). **On by default**; `KL_VRR=0` is the A/B.
  Builds an `MTLRasterizationRateMap` for the eye size and hands it to
  `kl_glfb_set_eye_rate_map`, which registers it on the eye textures and on the
  guest's multisampled scene target. The guest then rasterizes its expensive
  pass at a reduced rate in the periphery: measured **51.7% of the fragments**
  at the default falloff on a 2290x2400 host eye, and **33.5%** on device at
  3072x2464 with the display's own curve.
  - **On device the curve is the display's own**, sampled off the drawable's
    rasterization rate maps in the compositor's priming pass and re-issued at
    the guest's eye size (which is not the drawable's — see `KL_OVRP_EYE_MAX`).
    The two eyes' maps are combined per zone by maximum: visionOS gives each
    view its own map and the pair are mirror images, dense inboard where the
    eyes converge, and the guest has only one scene renderbuffer to foveate.
    `KL_VRR_ZONES` / `KL_VRR_EDGE` below are the fallback for an unfoveated
    drawable, and are what the host always uses.
  - **Only two targets are foveated: the eye textures, and the guest's
    multisampled scene renderbuffer. INTERMEDIATES ARE NOT**, deliberately.
    The registry has a size-keyed rule for the scene target (which ANGLE
    allocates itself in response to `glRenderbufferStorageMultisample`, so
    nothing outside can name its `MTLTexture`), and the guest allocates *other*
    render targets at exactly the same eye size — a post-processing chain
    reading and writing full-resolution intermediates. Foveating one of those
    would be a wrong picture, because the guest samples it with screen-space
    coordinates that know nothing about the warp. The scene target is
    distinguishable from all of them by being MULTISAMPLED, so the rule carries
    `minSamples` (`KL_RATE_MIN_SAMPLES` = 2 in `kl_glfb.c`) and hits it and
    nothing else. If a post chain ever needs foveating, it needs its own unwarp
    at every point the guest samples it — not a widened rule here.
  - **A rate map is built for ONE screen size**, so a guest that re-creates its
    eye textures at a different size needs the map rebuilt. Both frontends do
    that, keyed on size, from inside the provider so it lands before the texture
    is bound. `kl_glfb` enforces it as well rather than trusting the ordering: a
    texture whose size does not match the registered map is left unfoveated and
    says so by name. Beat Saber **1.6.0** is what makes this reachable — it
    goes 2400x2290 -> 2880x2748 -> back mid-run, where the 2019.4 build picks
    one size and keeps it.
  - The eye texture is left in a WARPED layout. `kl_view_mtl`'s composite and
    `KleptonCompositor`'s undo it (the unwarp grid, `kl_reproject.h`); every
    other reader — the `KL_GLFB_OUT` capture, `t_mtl_provider`'s readback — does
    not, so those show the squeeze directly. That is the cheapest confirmation
    it engaged.
  - **Foveation needs eye textures we own, so it follows the Metal PROVIDER, not
    the viewer.** `tests/t_mtl_provider.m` is compiled into `m_boot`
    (it is a host stand-in for Compositor Services, not a test-only
    file), and `kl_mtl_provider_install()` registers it **unconditionally under
    `KL_VIEW`** — so the macOS viewer does foveate, and `KL_VRR` is live there.
    `KL_VIEW_CPU=1` opts out and takes the readback path instead.
  - **`KL_GLFB_MTL=1` installs the same provider with no window**, which is the
    way to A/B foveation on the host without a headset or a viewer:
    `KL_GLFB=1 KL_GLFB_MTL=1 KL_VRR=0|1`. The provider builds the map from
    inside itself (`klmtl_update_rate_map`), because the eye size is only known
    there. Measured on Beat Saber 1.6.0: `screen 2290x2400 -> physical
    1648x1724 (51.7%)`, rebuilt **four times a run** as the guest resizes.
    Without a provider there are no eye MTLTextures, no map, and
    `klvm_grid_buffer` builds the 1x1 identity grid — foveation is simply absent
    rather than off.
  - `KL_ANGLE_VRR_TRACE=1` alongside it shows the ANGLE half: which render
    passes found the map, and whether each eye resolve took the
    physical-passthrough path (`passthrough=1`) rather than warping twice.
- `KL_VRR_EDGE=<0..1>` — the rate at the edge of the eye, default 0.35 (the
  fovea is always 1.0, falling off linearly). Lower is cheaper and blurrier in
  the periphery; 1.0 is a map that does nothing. Ignored on device when the
  drawable's own curve was sampled.
- `KL_VRR_FLOOR=<0.01..1>` — **the smallest per-zone rate the guest's map may
  carry**, applied on device after the display's curve is sampled (to the
  envelope, to the curves per eye, and to the synthetic curve alike). A fragment
  shader under a rate map runs in physical space, so `dFdx`/`dFdy` and every
  texture LOD derived from them measure physical pixels: a zone at rate `r`
  samples `log2(1/r)` mip levels higher than it would unfoveated. The display's
  own periphery (≈0.12 x, ≈0.10 y) is +3.1/+3.3 levels, a soft, screen-fixed
  over-blur. The floor bounds it at `log2(1/floor)` levels — 0.50 is +1 — at a
  fragment cost of 28.6% -> 48.8% of the unfoveated eye on the measured curve.
  One `[cp] foveation LOD:` line states the worst bias on both axes before and
  after, and the fragment fraction. **Off unless set in this fork** (upstream of
  here the default is 0.50): the eye sizes shipped for Beat Saber 1.45 and
  Doom 3 were chosen for the fragment count of the raw curve.
- `KL_VRR_ZONES=<n>` — how many equal screen-space zones each axis of that
  synthetic curve is divided into, default 16, capped at 64. Also the unwarp
  grid's cell count, which is what makes the unwarp exact rather than a sampling
  of the curve (`kl_reproject.h`). Same "ignored on device when the display's
  curve was sampled" caveat — there the count comes from the drawable map's
  physical granularity.
- `KL_OVRP_MSAA=<n>` — how many samples `ovrp_GetSystemRecommendedMSAALevel2`
  reports the device recommends for the eye render target, default **4** (what a
  Quest 2 answers, for the reason `Build.MODEL` says Quest 2). It is a
  *recommendation*, not a constraint — a guest consults it only when its project
  asks for the device default, and the sample count the eye layer is actually
  built with still arrives in `ovrp_CalculateEyeLayerDesc2` from the guest — so
  measured on RE4 the layer comes out `samples=1` either way. Set it to 1 if a
  multisampled eye layer ever turns out to be a problem; that is a measurement,
  not a redefinition of the headset. UE4 is the first guest here to ask.
- `KL_UE4_VULKAN=0` — present no Vulkan feature to a UE4 guest, i.e. answer
  `GetMetaDataInt("android.hardware.vulkan.version")` with 0. **This is where
  the graphics API is decided for an Unreal guest** — not in the RHI. RE4 is
  packaged Vulkan-only and says so itself when refused ("This device does not
  support Vulkan but the app was not packaged with ES 3.1 support", out of its
  own message box), so this restores that refusal exactly and is the A/B for
  anything that suspects the API choice. Unset or `1` presents
  `VK_MAKE_VERSION(1,1,0)`; any value **> 1 is used as the version word**
  itself, because Unreal decides from the number — UE5.3 (Pinball FX VR)
  refuses Vulkan at 1.0.3 without calling a single vk* function and accepts it
  at 1.1.
- `KL_UE4_MULTIVIEW` — tri-state override of the mobile-multiview clear
  (`runtime/guest/kl_ue4.c`). UE4 prepends `layout(num_views = 2) in;` to every
  cooked shader when `GSupportsMobileMultiView` is true, ANGLE's Metal backend
  has no multiview, and the compile failure becomes a ForceQuit — so on a **GL**
  guest the runtime clears that flag (log: *"mobile multiview was ON —
  cleared"*). On a **Vulkan** guest none of that applies, and clearing it
  forces UE4's DoubleWide stereo path (static meshes that vanish as the head
  moves on Into the Radius), so it is left alone (log: *"mobile multiview left
  alone — this guest brought up a VULKAN device"*), and a value cleared before
  the device existed is put back. **In this fork ANGLE has multiview**: a guest
  that is named the `GL_OVR_multiview` extensions (the Array eye layout on GL —
  Maestro) is never cleared, whatever the value.
  - **unset (default)** — decide from `kl_vulkan_guest_active()`.
  - **`=1`** — never clear, whatever the RHI.
  - **`=0`** — always clear, Vulkan included.
- `AndroidThunkJava_ForceQuit()` is logged with the guest's call stack, and by
  default not obeyed: for most titles it is a bail worth staying alive for.
  `KL_UE4_FORCE_QUIT=1` obeys it as the Quest does — the pump ends and the app
  leaves as for a game's own quit. On by default for Medal of Honor
  (`x3647259232032222`), whose Quit button ends in it once
  `moh-exit-no-rhi-teardown` lets `FEngineLoop::Exit` through. The bounded quit
  that came with the surface around it (`KL_UE4_QUIT_LIMIT`,
  `KL_UE4_QUIT_TRACE`) is not taken.
- `KL_UE4_CXX_ALLOC=0|1` — (`kl_ue4.c`) bind libc++_shared's own calls to
  `operator new`/`delete` (`_Znwm`, `_Znam`, `_ZdlPv`, `_ZdaPv`) to libUE4's,
  as bionic does, before any initializer runs. Without it libc++_shared
  allocates with malloc and libUE4 frees inline through FMallocBinned2, which
  raises a Fatal on the unknown block (log: *ForceQuit* with
  `FMallocBinned2::FreeExternal` under `UnrealLogSystem::LogStream`). Default 1
  for Zero Caliber 2 (`x6072194326150034`), else 0. Log: *"operator
  new/delete: N slot(s) ... now take libUE4.so's"*.
- `KL_MOH_HOVER=0|1` — Medal of Honor: Above and Beyond: the menu book and the
  telephone receivers on the Whitehall desk lift when a hand comes near. The
  game's data has that reaction off (`TurnHoverAnimsOff` true, `UseLabel`
  false, no hover transform); this sets the first two and gives the hover
  transform `KL_MOH_HOVER_CM` (default 4) cm above the resting place, every 30
  guest frames, through the engine's own object table. Default 1 for
  `x3647259232032222`, else 0.
- `KL_UE4_WAIT=<seconds>` — the pump budget for a NativeActivity guest, default
  5. There is no frame count on this door: the guest owns its own game thread
  and its own frame loop, so seconds are the only unit. **RE4 needs 300** — its
  one-time shader optimization takes about a minute of host wall clock and the
  eye reads `91/86100 lit` while it runs, which is a real frame that looks like
  a broken pipeline.
- `KL_UE4_PSO_LIST=0|1` — Moss from Steam only (target `moss` with Steam's
  library in its files): `1` writes `r.ShaderPipelineCache.Enabled=1` under
  `[ConsoleVariables]` in the game's user config
  (`<files>/UE4Game/MossGame/MossGame/Saved/Config/Android/Engine.ini`) before the
  engine starts, so it compiles the pipeline list it ships, as the Meta build
  does; the Steam build switches that list off. `0` takes the line out again.
  A Test switch in the start window, on from the start for Moss (targets.py
  `test_on`); without it the default is off (`kl_ue4.c`).
- `KL_UE4_NO_OCCLUSION=0|1` — Roboquest VR, Meta Store build
  (`x9269393463181825`) only, default `1`: writes `r.AllowOcclusionQueries=0`,
  `r.HZBOcclusion=0` and `r.Mobile.HZBOcclusion=0` under `[ConsoleVariables]`
  in the game's user config
  (`<files>/UnrealGame/RoboquestVR/RoboquestVR/Saved/Config/Android/Engine.ini`)
  before the engine starts. With foveated rendering on, at any level, objects
  blinked in and out: the engine judges what is hidden from foveated depth
  (occlusion queries over coarse pixels, and a stored depth resolve that
  `kl_fove` leaves in the physical layout). `0` takes the lines out again.
- `KL_JKXR_WAIT=<seconds>` — the same budget for the JKXR door, default 5. Same
  reason there is no frame count: the engine owns its own render thread. A menu
  reached and rendering wants about 45. It bounds the app's window-and-report
  run (`KL_IMMERSIVE=0`) as well as `build/m_boot`'s; the immersive space pumps
  until it is dismissed and ignores it, exactly as it does `KL_UE4_WAIT`.
- `KL_JKXR_DATA=<dir>` — a directory holding the RETAIL game data
  (`assets0.pk3` and up), which is neither in the APK nor an OBB: JKXR is an
  engine port and the user supplies the game they own. Read ONCE per userdata
  — the pk3s are symlinked into `<userdata>/JKXR/<JK3|JK2>/base` and the links
  persist, so later runs need no knob. Only `assets*.pk3` is taken, so a
  directory holding mods and configs beside them is left alone. Without it the
  engine stops with "Couldn't load default.cfg" and says nothing about where it
  looked; the staging census says so first, by name.
- `KL_UNITY_BOOT_EXTRA="<k=v;k=v>"` — lines appended to a Unity guest's
  `bin/Data/boot.config` as `AssetManager.open` serves it (the file on disk is
  untouched); `;` separates lines, empty means none. Default empty, except
  Eye of the Temple (x5361030930653377), LES MILLS BODYCOMBAT
  (x4015163475201433) and Surviving Mars: Pioneer (x9863010250399155, its
  hands): `disable-gpu-skinning=1`, because their
  compute-shader skinning cannot compile on ANGLE's ES 3.0 context
  ("Internal-Skinning.main: GLSL compilation failed") and every skinned mesh,
  the whip among them, was never drawn.
- `KL_JKXR_ARGS="<args>"` — appended to the engine's command line, after the
  game token. It reaches the engine both ways it can (the string handed to
  `GLES3JNILib.onCreate` and the `JKXR/commandline.txt` the engine re-reads),
  so the two cannot disagree. `+set developer 1` is the useful one; note that
  this port's `Sys_Print` writes to a buffer and drops it, so console output
  does not reach the log however verbose it is asked to be.
- `KL_OVRP_EYE_SCALE=<x>` — scale the per-eye render target size the guest is
  told to use (`ovrp_GetEyeTextureSize`), default 1.0, accepted in 0.05..4.0.
  Applies to whatever the frontend measured off the display
  (`kl_ovrp_set_eye_texture_size`); with nothing pushed the size stays the
  Quest 2's 2290x2400 and this does nothing. The cheapest resolution knob
  there is — Unity then applies its **own** ~1.2x on top of the result.
- `KL_OVRP_ADAPTIVE_RES=0|1` — `ovrp_GetLayerRecommendedResolution` for the eye
  layer follows the GPU instead of always answering the full size: the time the
  guest's frame still had to go on the GPU at frame end, averaged over 30
  frames, is steered toward `KL_OVRP_ADAPTIVE_GPU_MS` (default 14) by scaling
  the recommendation between 70 % and 100 %. Only a title that implements Meta's
  dynamic resolution reads it. Default 1 for `x24882487241367228` (Alien: Rogue
  Incursion), else 0.
- `KL_OVRP_VIEWPORT_TOP=0|1` — mirror the eye layer's submitted render
  viewport to the texture's last rows before it goes into the frame record.
  Default 0. The compositors (visionOS and the Mac viewer) already mirror a
  rect shorter than the texture for every Vulkan guest
  (`kl_reproject_viewport_for_pass`); with this on as well the two cancel and
  the picture shows its lower part over unwritten rows. It used to default to
  1 for Unreal guests on Vulkan under a store-built target, which is how Alien:
  Rogue Incursion came to show a black band at the bottom from toolchain 478 on
  (every such title with a viewport shorter than its texture had the same
  double mirror). Only for an A/B against a compositor that does not mirror.
- `KL_OVRP_SWAP_PER_FRAME=0|1` — whether EndFrame presents for a guest is
  decided by "did it swap since the last EndFrame" instead of "has it ever
  swapped". Saints & Sinners swaps for its pre-VR splash and never again,
  which froze the GPU fence for the session (a ghost beside every object
  while the head turns). Default 1 for Unreal guests, else 0.
- `KL_OVRP_EYELEVEL_DEFAULT=0|1` — a guest that has not named a tracking
  origin is taken as EyeLevel from the first head pose. Default 1 for
  `x2897337400373711` (its title appeared at the player's feet), else 0.
- `KL_OVRP_HEADLOCK_BIT0=0|1` — in the layer list EndFrame is handed, bit 0 of
  a layer's flags means head-locked (beside 0x2, which is where the flag is
  for `ovrp_EnqueueSubmitLayer`). Found independently for The Walking Dead:
  Saints & Sinners (its fade and HUD sank to the floor) and Skydance's
  BEHEMOTH (its transition fade darkened the lower half of the view). Default
  1 for Unreal guests under a store-built target, else 0.
- `KL_OVRP_TEXPTR_INDIRECT=0|1` — on a Vulkan guest, `ovrp_GetLayerTexturePtr`
  hands back a pointer to the layer's `VkImage` rather than the `VkImage`
  itself, which is what Unity's `Texture2D.CreateExternalTexture` reads a
  native texture pointer as. With 0, Unity dereferences the image object and
  builds its overlay textures on the "wild handle" `kl_vulkan.c` substitutes a
  scratch for. Default 1 for `x5812519008825194` (AC Nexus), else 0.
- `KL_LOADVID_FOLLOW_SUBMIT=0` — keep the host-played loading video (AC Nexus's
  Equirect loading layer) up for as long as that layer EXISTS. The default takes
  it down once six eye frames have ended without the guest submitting the layer,
  because the guest hides its loading screen by no longer submitting it, not by
  destroying it.
- `KL_LOADVID_OFF=1` — never draw the host-played loading video.
- `KL_OVERLAY_TEXL=0|1` — composite a guest quad layer from the texture handle
  it SUBMITTED (`ovrp_EnqueueSubmitLayer2`'s texL, resolved to the guest's own
  render texture) rather than from the layer's swapchain image. Default 1,
  except 0 for `x5812519008825194`, where `KL_OVRP_TEXPTR_INDIRECT` makes the
  swapchain image the valid one and the submitted handle is a recycled
  render texture.
- `KL_OVRP_SPACEWARP_TEX=0|1` — when the guest's eye layer asks for Application
  SpaceWarp (`ovrpLayerFlag_SpaceWarpDataAllocation`), give it a motion-vector
  texture size and answer `ovrp_GetLayerTextureSpaceWarp` with real images
  instead of "unsupported". Nothing here consumes the vectors; the point is that
  the Oculus XR provider in AC Nexus fails its eye-texture creation without
  them and Unity then stops the XR display after one frame. Default 1 for
  `x5812519008825194` and `x5245041552210029` (Arizona Sunshine 2, the same
  provider), else 0.
- `KL_PLAT_LANGPACK_LIST=0|1` — answer `ovr_AssetFile_GetList` (and the status
  calls) with the `<tag>.lang` voice packs that are really in the game's OBB
  directory, as installed language packs, instead of an empty list. AC Nexus
  greys out every audio language it is not told is installed. Nothing is
  downloaded, deleted or reported as purchased. Default 1 for
  `x5812519008825194` and `wrath2`, else 0. For these titles
  `ovr_LanguagePack_GetCurrent` / `ovr_LanguagePack_SetCurrent` are answered
  as well (Asgard's Wrath 2 asks them): the current pack is a choice between
  the files on the disk, kept in `android-files/klepton-language-pack.txt`;
  with nothing chosen it is the pack of the language the game is told.
  Default 1 also for any title whose installation lists its asset files
  (`KL_ASSET_FILES_PATH`, below); the list then replaces the directory listing.
- `KL_ASSET_FILES_PATH=<file>` — (`runtime/xr/kl_assetcfg.h`,
  `runtime/xr/kl_ovrplat.c`) the installation's list of the title's asset
  files; without it, `android-files/klepton-asset-files.txt` if that file
  exists. One `asset <store asset id> <file> <size> [<tag>] [meta=<text>]` per
  line, `#` comments; `<file>` relative to the OBB directory (or absolute
  inside it), the tag defaulting to the file name up to its first `.`, `meta=`
  the file's store metadata (`ovr_AssetDetails_GetMetadata`, `""` without it).
  Example (Marvel's Deadpool VR, versionCode 8742):
  `asset 26412782438311256 de.lang 3256791074 de meta=1.0.40.356975.Quest`.
  A listed file that is a regular file of exactly that size is reported
  installed (`ovr_AssetFile_GetList`, `StatusById`/`StatusByName`,
  `ovr_AssetDetails_*`, `ovr_LanguagePack_GetCurrent`/`SetCurrent`) at its path
  in the guest's spelling, `/sdcard/Android/obb/<package>/<file>`; any other
  listed file is "available" in a status answer and absent from the list.
  Malformed lines, `.`/`..` components and paths outside the OBB directory
  (also through a symbolic link) are rejected and logged. Downloads stay
  refused. Where configured, `KL_LOCALE_DEVICE` defaults to 1 and
  `ovr_GetLoggedInUserLocale` gives the same locale as `java.util.Locale`.
  Not configured: every title behaves as before.
  **Deadpool VR needs all three parts** (measured on the headset, 2026-10-16):
  its `ULanguagePacksSubsystem` takes a pack only when the metadata equals its
  own `UGameVersion::GetGameVersionString` (`1.0.40.356975.Quest` = the APK's
  versionName), finds the language by the file name's ending (`de.lang`), and
  mounts the pak only when the path starts with `/sdcard/Android/obb/<package>/`
  (any other absolute path it appends to its `Content/Paks` directory: the
  language can be picked, the menus turn German, and no voice plays).
- `KL_PLAT_TRACE=1` — (`runtime/xr/kl_ovrplat.c`) log every distinct
  asset-file, language-pack and locale call the guest makes with its arguments
  and our answer, every `ovr_*` name it resolves, and the locale it is told.
  Off by default. With `KL_TRACE_FS=fail` it shows which file a language pack's
  sound engine then could not open.
- `KL_LOCALE_DEVICE=0|1` — (`runtime/jni/kl_jni_services.c`) tell the guest the
  device's own language (`KL_DEVICE_LANG`, set by the app from the system's
  first preferred language, e.g. `de_DE`) instead of en_US, where `LANG` is not
  set — which on a headset is always. Default 1 for `wrath2`, whose text and
  voices follow the device and nothing else, and for a title with an asset-file
  list (`KL_ASSET_FILES_PATH`), else 0. The start window's "Game
  language" choice sets it. `.lang` voice packs in the OBB directory are part
  of the read-through OBB index either way.
- `KL_OVRP_ENQ_OWNS_OVERLAYS=0|1` — while the guest is filing panels through
  `ovrp_EnqueueSubmitLayer`, an `ovrp_EndFrame4` whose own list carries no
  overlay leaves the published set alone instead of emptying it (which blinked
  AC Nexus's cards off every few seconds). Default 1 for `x5812519008825194`,
  else 0.
- `KL_OVRP_ENQ_WITH_XR=0|1` — (`kl_ovrp.c`) a Unity OpenXR guest whose
  OVRPlugin still files OVROverlay panels through `ovrp_EnqueueSubmitLayer2`:
  keep that set and publish it with each `xrEndFrame`'s own quads instead of
  letting the OpenXR frame replace it. Default 1 for Mission: ISS
  (`x2094303753986147`), Fluid (`x6946182742093178`) and Spatial Zombies
  (`x24151641901194680`, its menu is a Cylinder), whose panels were black
  between frames or never shown, else 0.
- `KL_OVRP_ENQ_MIRROR_XR=0|1` — (`kl_ovrp.c`, `kl_ovrp_openxr_frame_end`) with
  `KL_OVRP_ENQ_WITH_XR` on a GLES guest, copy the enqueued panels' images for
  the compositor at each `xrEndFrame`, as `ovrp_EndFrame` does on the OVRPlugin
  path; without it they arrive with no MTLTexture and are not composited.
  Default 1 for Fluid (`x6946182742093178`) and Spatial Zombies
  (`x24151641901194680`), else 0.
- `KL_OVRP_ENQ_CYLINDER=0|1` — (`kl_ovrp.c`, `klovrp_EnqueueSubmitLayer2`) give
  a Cylinder filed through `ovrp_EnqueueSubmitLayer2` its shape from the scale
  OVROverlay passes (arc width, height, radius, in metres); without it the
  compositor has no cylinder to build and the panel is not drawn. Default 1 for
  Spatial Zombies (`x24151641901194680`, its menu is one), else 0.
- `KL_OVRP_ENQ_RETIRE=0|1` — a panel the guest destroys (`ovrp_DestroyLayer`,
  or `ovrp_EnqueueDestroyLayer` with the id behind a pointer, as Ghost Town's
  plugin passes it) is taken out of the visible set and the batch being
  gathered at once, and when enqueue submits stop the batch is dropped as
  well, so a retired panel cannot come back with the next flush. Without it
  Ghost Town's fade quads stayed up as a white sheet. Default 1 for
  `x9531494193591052` (Ghost Town, community PR #41), else 0. Host test:
  `make ovrplayers`.
- `KL_OVRP_RECENTER_COUNT=0|1` — `ovrp_GetLocalTrackingSpaceRecenterCount`
  answers how often the host has recentered (the compositor's recenter, after
  it has moved the head and the controllers) instead of 0. Default 1 for
  `x9531494193591052`, else 0.
- `KL_OVRP_BOUNDARY=0|1|<w>,<h>,<d>` — report a Guardian as set up, as on
  every Quest: configured, a play area of the given size in metres (default
  2.0 x 2.5 x 2.0), and a floor rectangle of that size round the tracking
  origin as its geometry. Off, the boundary answers say "none, size 0".
  Angry Birds VR: Isle of Pigs fits its levels to the play area. Default 1 for
  `x2718606324833775`, else 0.
- `KL_OVRP_NO_TRACKERS=0|1` — answer the tracker nodes 5..8 (the Rift's
  external sensors) as a Quest does: not present, not tracked, identity at the
  origin. Off, they report the head pose like every unmapped node. Angry Birds
  VR: Isle of Pigs reads TrackerZero from its C#, and its island followed the
  player. Default 1 for `x2718606324833775`, else 0.
- `KL_OVRP_POSE_TIME=0|1` — fill `ovrpPoseStatef.Time` in
  `ovrp_GetNodePoseState` with the moment the step's sample was latched instead
  of leaving it 0. AC Nexus moves its physical hand bodies only when the
  controller pose time advances, so with 0 the hands that touch things never
  left the rig's origin. Default 1 for `x5812519008825194`, else 0.
- `KL_VK_MAX_SAMPLES=<1|2|4>` — the highest multisample count a Vulkan guest
  is told the device supports (limits and image-format properties), 0 for the
  truth. A guest's multisampled eye targets are real memory here, so this is
  what a larger eye is paid for with. Default 2 for `x5812519008825194`
  (with `KL_OVRP_EYE_MAX` 3840), `x3634830803298285` (Demeo) and
  `x9386827918099701` (BattleGroupVR2: three 4x colour and depth pairs ask
  for about 5.2 GiB at its start; its eye size is left alone, and it runs
  without MTLHeaps, `MVK_CONFIG_USE_MTLHEAP=0`, as Alyx does), else 0.
- `KL_VK_FORCE_SAMPLES=4` — every multisampled image, render-pass attachment
  and pipeline a Vulkan guest makes with fewer samples is made with 4. For a
  guest whose own anti-aliasing setting cannot be reached (Marvel's Deadpool VR
  renders 2x from a setting in an encrypted pak). Costs fill time and memory;
  pipelines compiled for the old count are compiled again. Default: the start
  window's "Edge smoothing" setting, which is off unless chosen and is offered
  to a game that was seen multisampling with fewer than four.
- `KL_OVRP_EYE_MAX=<px>` — cap on the longer axis of that size, default 3072
  (4000 for `doom3quest`, 3840 for `x5812519008825194`; other per-title
  defaults in kl_ovrp.c, e.g. 4000 for `x2285803881494677`, Gun Club VR),
  0 to disable. Vision Pro's logical per-eye resolution is far larger than a
  Quest's, and the guest allocates stages x eyes of RGBA16F at whatever it is
  told, so an uncapped number turns straight into hundreds of MiB and into fill
  cost. The aspect ratio is preserved when it clamps, because the frustum
  tangents pushed alongside would otherwise disagree with the picture's shape.
  Every change logs the requested size, the applied size and the resulting
  swapchain MiB.
  Below it, per game: *Settings › Advanced › Max resolution per eye* in the
  start window (`klepton.eyeMax`, `KleptonEyeMax`, handed over with
  `kl_ovrp_set_eye_max_choice` before the game starts): Default (the title's
  cap, `kl_ovrp_title_eye_cap`), 2048 to 5120, or Native (no cap: the whole
  size the display asks for, about 4633x3717). The order is this variable,
  then that choice, then the title's cap, then 3072; *Resolution* and MetalFX
  scale down from whichever wins. Above the title's cap the window warns that
  the game may stutter or not start. It replaces the Test switch
  `KL_OVRP_EYE_MAX=4608`: a game that had that switch on (chosen, or named in
  `KLTestOn`) is set to 4608 once and the switch's state is dropped
  (`klepton.eyeMax.migrated`). Tests: `make maxeye`, `make maxeyeswift`.
- `KL_OVRP_VIEWPORT_SCALE=<x>` — force the **render viewport** to that fraction
  of the eye texture, default 1.0, accepted in 0..1 and multiplied by whatever
  the guest itself asked for. This is not a resolution knob, it is the **A/B for
  the composite's crop**: a title lowers its render resolution by shrinking this
  rect and *not* by resizing the texture (Beat Saber does it on entering a map),
  so a compositor that ignores it shows the picture in a corner of the eye with
  unwritten texels around it — and every call on that path succeeds, so there is
  no error anywhere. Forced at `ovrp_CalculateEyeViewportRect`, which is where
  the guest *asks* where to render, so the guest really sets that GL viewport,
  really renders into the sub-rect and really submits it at `ovrp_EndFrame4`:
  the knob exercises the whole chain rather than the last link. **A correct
  composite shows the same picture at any scale, merely softer.** Three log
  lines say where a wrong one went wrong — `[ovrp] render viewport` (what the
  guest submitted), `[view]`/`[cp] first render viewport read from the frame
  record` (what the compositor read), and `[view]`/`[cp] render viewport … —
  compositing that sub-rect` (what it did with it).

### Haptics (`runtime/xr/kl_ovrp.c` — the seam that runs OUT of the guest)

The guest queues an amplitude envelope through OVRPlugin's buffered haptics API
(320 Hz, one byte a sample); `kl_ovrp_haptics_pull` is an **envelope follower**
that reports what came due since the frontend last asked. Platform-independent
— the visionOS playback knobs are in the visionOS section.

- `KL_HAPTICS_MIN_MS=<ms>` — how long a level is held after its samples run
  out, default 32. ALVR's number and ALVR's reason: *"controllers can't do 10ms
  vibrations"*. It is a floor on the DRIVE, not on how long we wait before
  reporting one — waiting was the first design and it lost note cuts entirely
  The same knob sets the floor on a discrete pulse's
  duration in the visionOS fallback path.

- `KL_HAPTICS_TRACE=1` — both halves of the conversation: every buffer the
  guest queues **printed as samples** (up to 32, with its min..max), every
  level pulled, and every *edge* of the legacy vibration API. Edges only for
  that last one on purpose — this title calls
  `ovrp_SetControllerVibration(mask, 0, 0)` on both hands every single frame as
  an idle "nothing should be buzzing", and tracing those would bury everything
  else. The sample row answers the one open question about the source: a
  note-cut clip that carries its own **decay** prints a falling row, a square
  burst (whose fade on a Quest would be the LRA's ring-down, not data) prints a
  flat one.
- `KL_HAPTICS_SWAP_STATE=1` — swap the two words of `ovrpHapticsState`
  (`SamplesAvailable` / `SamplesQueued`). The A/B for the one ABI claim in this
  subsystem that fails **silently**: get the order backwards and the guest
  computes "no room" forever and queues nothing, which is indistinguishable
  from a title that simply has no haptics. If `KL_HAPTICS_TRACE` shows state
  queries but never a buffer, try this. (The descriptor's field order fails
  loudly instead — the buffers that do arrive would carry a `SamplesCount` of 1
  or 256 rather than the 20 it paces to.)
- `KL_HAPTICS_VIB_LAPSE=<seconds>` — how long an un-refreshed
  `ovrp_SetControllerVibration` level runs before it lapses, default 2 (the
  real API's own ceiling is about that). It matters more here than there: a
  frontend that has already been handed a pulse cannot be told to stop.
- `KL_HAPTICS_XR_MAX=<seconds>` — the ceiling on one `xrApplyHapticFeedback`
  order, default 5. Not defensiveness: OpenXR lets an app ask for a vibration
  lasting minutes, and for the same reason as the lapse above, nothing here can
  recall a level a frontend has already been handed — so an unbounded duration
  is a controller that buzzes until the process ends. The OpenXR path is a
  **third** haptics source beside the buffered ring and the legacy vibration
  level, kept separate from both because two owners of one slot erase each
  other silently; `kl_ovrp_haptics_pull` merges all three by maximum.

- `KL_DISPLAY_HZ=<hz>` — force the display frequency the guest is told the
  headset runs at, 30..240. It overrides both measurements: on visionOS the
  compositor's priming pass pushes the rate it actually presented at, and under
  `KL_VIEW` the viewer pushes the rate of the display its window opens on.
  Without either it is the Quest 2's 72, the device we describe everywhere else.
  It matters beyond pacing for Steam Link: the VR client publishes this list to
  the Steam host as `VTE_AVAILABLE_FRAMETIMES_US`, the host asks for a rate,
  and the negotiation is visible in the guest's own log.

  The viewer's number is the PANEL's rate, which is a ceiling and not a
  promise: a heavy target can deliver a fraction of it while the far end of a
  video stream is asked for the full rate. The viewer's HUD prints achieved
  against advertised every second; this knob is how a run pins the advertised
  number to what it measured (`Server requested refresh rate 90.0 was not
  available. Using 72.0`). Under `KL_XR_PACE` a guest that owns its frame loop
  is held to the composite, so the two numbers agree unless the guest cannot
  keep up; nothing paces the other host guests.

  For Steam Link this knob decides the far end's ENCODE rate, and the pixel
  count is not ours to set: the host packs four 1536x1536 tiles into one
  1536x6144 frame, so 120 Hz asks SteamVR for 1.13 Gpixel/s and every advertised
  hertz divides a bitrate the host chose into thinner frames. A live run at 120
  measured 773 decoded frames in ~6.5 s — the rate was honoured — so the picture
  quality question at a fixed link rate is bits per pixel, and this knob is the
  only lever on the denominator.
- `KL_HZ_SETTLE=0|1` — the **boot rate settle gate**, default **on**
  (`visionos/Sources/KleptonCompositor.swift`, `primeDisplay`). The Vision Pro
  brings the layer up at 120 Hz and drops to 90 once the immersive session
  engages; the fixed eight-frame priming measurement (~66 ms) lands inside that
  window, so the rate the guest is told is decided by a race — and the guest
  reads that rate exactly once, early, then paces everything off it for the
  session. A boot that lost the race told the guest 120 against a panel
  delivering 90: 4:3, every fourth guest frame sharing a slot, a whole-image
  zigzag proportional to head speed.
  With the gate on, priming keeps presenting black until the presentation
  intervals have agreed on one period to within 6% (`KL_HZ_SETTLE_TOL`) for
  **500 ms** (`KL_HZ_SETTLE_MS`) *and* over at least **120 intervals**
  (`KL_HZ_SETTLE_MIN_GAPS` — duration is not resolution), then latches; if that
  never happens within **3000 ms** (`KL_HZ_SETTLE_TIMEOUT_MS`) it latches the
  median of everything seen and says so loudly. A gap that is a near-exact
  2x/3x of the run's period is a held frame, not a rate change, and extends the
  run rather than breaking it. `KL_HZ_SETTLE=0` restores the eight-frame
  behaviour exactly, so the A/B costs one launch and no rebuild.
  Holding here holds every consumer with no extra plumbing: `primeDisplay` runs
  before `kl_app_guest_start`, so the guest thread does not exist yet and its
  one early Hz read cannot precede the latch.
  **The latched value is a MEDIAN of the run's one-slot gaps, not the mean.**
  The run necessarily begins at the layer's first frames, which is where
  `primeDisplay` also computes the foveation curve and pushes the eye textures,
  so the slowest frames of the session are inside the window; measured on
  device, the mean came out 0.16% low against a dead-steady panel, which is
  13.9 us a frame. That is bias, not noise, and averaging longer does not touch
  it. `KL_HZ_SNAP_TOL` (default **0.005**) then snaps a measurement within half
  a percent of a rate the panel actually runs — 120/100/96/90, plus the 60 a
  thermal demotion produces — to that rate exactly, because the guest is told
  the number once and our estimate's last two digits would otherwise become a
  permanent frequency offset.
  *Read it with:*
  `  [cp] display rate SETTLED at 119.995 Hz after 1001 ms of steady cadence (120 intervals in the settled run, median of one-slot gaps; their mean says 119.812 Hz); 7 pre-settle interval(s) discarded, and they suggested 120.00 Hz — SNAPPED to the panel's 120.00 Hz (within 0.5%)`
  followed by the unchanged `[cp] display .. Hz measured over N frames` and
  `[cp] guest will be told .. Hz`, which remains the truth of what the guest
  got. A panel rate that moves *after* the latch is caught separately by
  `[cp] THE PANEL IS NO LONGER RUNNING AT THE RATE WE LATCHED` — six seconds of
  cadence more than 10% off the latched period. Detection only: the guest read
  its rate once and cannot be re-told.
- `KL_XR_SPACEWARP_OUT=<dir>` — Mac lab: write the motion vectors (raw RGBA16F,
  `sw_f<frame>_layer<l>_eye<e>_mv_<w>x<h>.rgba16f`) and depth (raw float,
  `..._depth_<w>x<h>.f32`) a game chains onto its projection views with
  XR_FB_space_warp, every `KL_VK_OUT_EVERY`-th frame. Without it the space-warp
  info is only named once per layer and eye (`[xr] space warp on layer ...`).
  For an OVRPlugin game with Application SpaceWarp (Assassin's Creed Nexus) the
  vectors of the stage `KL_VK_OUT` captures are written beside its eye pictures
  (`sw_f<frame>_s<stage>_eye<e>_mv_<w>x<h>.rgba16f`).
- `KL_FRAMEGEN_MV=0` — frame generation searches the motion even where the game
  hands its own vectors over (XR_FB_space_warp, or OVRPlugin's SpaceWarp
  textures). Default on. `KL_FRAMEGEN_MV_SCALE=x,y` (default `0.5,-0.5`, measured
  on HITMAN 3 and Assassin's Creed Nexus) and `KL_FRAMEGEN_MV_HEAD=0` (the
  vectors leave the head's turn out) are levers for a game that states them
  otherwise.
- `KL_XR_LAYERS=0` — flatten the guest's projection layers into one eye picture
  instead of compositing each as its own quad (default 1, the per-layer
  composite).

  A guest may submit more than one projection layer, and OpenXR says what they
  mean: draw them back to front, each with its own field of view, each in its
  own space. Steam Link submits two or three. The per-layer composite does
  exactly that — every layer is placed against the DISPLAY by its own tangents
  and never against another layer, so there is no rect to compute, no mapping
  from one frustum into another, no rule for which layer is "the eye" and no
  threshold for what counts as an inset.

  `0` restores the flattening those five decisions belong to: the widest layer
  becomes the eye, a narrower one contained inside it is blitted over the
  centre, and everything else is dropped. It is the A/B against a known state
  and the only setting under which the `KL_XR_FOVEA*` knobs below do anything —
  they place the inset the flattening lays down, and the per-layer composite
  lays none.
- `KL_XR_FOVEA=0` — stop compositing the foveal inset (default on, and only for
  a guest whose driver said it stacks layers, `runtime/xr/kl_openxr.c`). Steam
  Link streams a wide low-detail base projection layer plus a NARROW high-detail
  centre one; the base is captured as the eye and the narrow layer is laid over
  it where its own frustum falls, per eye, every frame it is submitted with a
  released image and a span under 80% of the base's.

  This is the A/B for two symptoms that look unrelated and are not: a visible
  fovea BOUNDARY, and the view stepping up and down a frame at a time. The inset
  is only laid down on frames where all of its preconditions hold, so if it is
  misaligned against the base, the centre of the picture snaps between two
  positions as it drops in and out. Off, the whole eye is uniformly the base's
  detail and neither can happen — which is what makes it the separator.

  Alignment, all read once: `KL_XR_FOVEA_SHIFT_X` / `_Y` nudge the placement in
  THOUSANDTHS of the eye (default 0); `KL_XR_FOVEA_SCALE` (1000 = none) trims the
  placement about the inset's own centre, for doubling that grows from nothing at
  the centre to worst at the rim; `KL_XR_FOVEA_DEPTH_CM` (200) is the depth the
  parallax correction aligns for, because the inset is rendered from the head
  centre and the base from the eye; `KL_XR_FOVEA_FLIP=1` inverts the vertical
  mapping for a guest whose rows run bottom-up. **The defaults were tuned by eye
  on the HOST**, where the IPD is the 63 mm stopgap rather than a measured one,
  so a device run is expected to want its own numbers — `_Y` first for a vertical
  seam.

  `KL_XR_FOVEA_FEATHER=0` makes the inset a hard rectangle instead of fading its
  rim into the base; `KL_XR_FOVEA_INNER` (78, 0..100) is how much of the inset
  stays fully sharp before that fade begins. The feather needs a plain
  `sampler2D`, so an inset that is an ARRAY slice gets the hard edge regardless.

- `KL_XR_FOVEA_DUMP=<dir>` — write EVERY projection layer's image as
  `eye<N>_<seq>_layer<L>.png`, with the frustum each states, repeating every
  `KL_XR_FOVEA_DUMP_EVERY` seconds (default 5; 0 captures once). All layers of
  one frame share a capture, so they are comparable to each other.

  It began as a base/inset pair and that was a mistake worth recording: a pair
  assumes which layer is which, and the first capture that worked disproved the
  assumption — the WIDEST layer, the one taken as the eye, was an empty blue
  gradient, and the streamed picture was in the narrower layer being treated as
  a foveal inset. An instrument that only photographs what you already believe
  in cannot tell you that you named it wrong.

  Prefer a HOST run: the files land where you can read them and no device pull is
  needed — but set `KL_OVRP_EYE_TAN=vision` with it, because the host's default
  base frustum is a symmetric 90 degrees and the device's is not, so an unset run
  measures a different frustum pair than the one being complained about.

  The dump REFUSES rather than guesses: if the texture's recorded size disagrees
  with the size the caller asked for, if the texture cannot be attached for
  reading, or if the read errors, it says which and writes nothing. It also
  prints the image's mean, so a read that succeeded and saw nothing is
  distinguishable from a picture.

- `KL_XR_FOVEA_TRACE=1` — one line a second, per eye, on the foveal inset's
  stability. The inset dropping out on some frames and the inset staying put in
  a MOVING place look identical to the eye — both step the picture about — and
  no per-frame number distinguishes them, so this accumulates over a second:

  ```
  [xr] fovea eye 0: 118 candidate(s), 112 laid down, 6 skipped — the base layer
       was not captured before it this frame; placement travel x 0.4 y 3.9
       (thousandths of the eye)
  ```

  Skips dominate → it is dropping out, and the named reason says why. Travel
  dominates → the placement is moving, and the number is in
  `KL_XR_FOVEA_SHIFT_Y`'s unit, so a y travel of 4 is a defect a shift of 4 would
  cover. "Candidates", not frames: a guest stacking three projection layers
  offers two of them per frame, so the count is opportunities.

- `KL_XR_PACE=0` — stop pacing a host guest that owns its own frame loop, i.e.
  let `xrWaitFrame` return immediately (default on, `mains/m_boot.c` under
  `KL_VIEW`; the device app always paces, from its compositor). Applies to the
  OpenXR guests that drive their own loop — Steam Link's VR door and JKXR — and
  never to a Unity one, whose frame this side already calls.

  It is an A/B for a real cost. Unpaced, the guest renders as fast as it can and
  the compositor shows what it can: Steam Link's VR client ran ~1000 frames a
  second against a 120 Hz window, paying its three per-frame mirror blits eight
  times over for each displayed frame, and measuring the frame pacing it reports
  to the streaming host against a clock nothing drove. Paced, the guest's frame
  count tracks the composite (JKXR: 105 fps paced against 100 shown, 125 fps
  unpaced against the same 100).
- `KL_XR_CAPTURE_LAYER=N` — which projection layer `KL_GLFB_OUT` reads (default
  0, `runtime/xr/kl_openxr.c`). Steam Link's VR client submits **four** projection
  layers a frame — two 1536x1536 pairs for its panels and two 2290x2400 pairs
  for the eyes — and nothing in the submission says which holds what, so this
  moves the capture without a rebuild. That matters because a streaming run
  costs a fresh Steam pairing to repeat. The `[xr] layer N eye M <- swapchain`
  lines say what each one is, and mark the captured one.
- `KL_XR_CAPTURE_EYE=0|1` — which eye `KL_GLFB_OUT` writes (default 0). Needed
  because a Unity OpenXR guest's eye swapchain is ONE array texture with a
  slice per eye, so there is no per-eye texture name to pick between. The
  capture now takes the presented image the guest STATED
  (`kl_glfb_set_live_eye_image`: name, size and array layer) rather than
  searching for an FBO by texture name — the search read the eye at the
  window's size and produced the window, and a GL name is a slot rather than
  an identity anyway (trap 31).
- `KL_XR_EYE_MIRROR=0` — stop copying the guest's presented eye image into
  storage the compositor can sample (`runtime/xr/kl_openxr.c` →
  `kl_glfb_mirror_eye_layer`), and re-point the guest's own swapchain texture at
  an MTLTexture instead.
  **The default moved to the copy on 2026-08-16, on a measurement**, and the
  re-point is kept only as this A/B. Re-pointing calls
  `glEGLImageTargetTexture2DOES` over a texture the guest has ALREADY attached
  to a framebuffer of its own, and one guest's rendering does not survive it:
  the same scene, the same frame of an id Tech 3 port, came out 2,875,392 of
  5,496,000 lit with the top row and the right-hand column of the picture simply
  missing, where the same run with no provider registered — and the same run
  with the copy — is 5,496,000 of 5,496,000 and correct block for block. Every
  GL call succeeded in all three. Nothing visible from this side distinguishes a
  guest that keeps its framebuffers from one that rebuilds them, so the route
  that cannot fail that way is the default: the destination is a texture of ours
  that nothing has ever attached. Use 0 on a target where the per-frame blit
  costs more than it buys — Steam Link streams video and is the one measured on
  device — and expect a black eye rather than a re-point for an ARRAY swapchain,
  which cannot be re-pointed at all:
  `glEGLImageTargetTexture2DOES` takes only a `GL_TEXTURE_2D`, and ANGLE's Metal
  backend reduces every EGLImage sibling to a slice view
  (`TextureImageSiblingMtl::initImpl`), so an EGLImage is always 2D. The copy is
  one `glBlitFramebuffer` per eye per frame, from the image the guest presented
  into the provider slice a Unity/OVRPlugin guest would have been handed
  directly — so nothing downstream of the eye table changes.
  **Foveation is refused while it is in use** and says so once: a rate map only
  pays when the GUEST's rasterizer writes fewer fragments, and here the guest
  renders into a swapchain of its own that carries no map, so the map could only
  attach to the copy — squeezing a full-resolution picture on the way in and
  stretching it back out. Measured, on the first run that composited: 2,839,242
  lit of 5,496,000, which is the foveation ratio exactly; with the map dropped,
  5,496,000.
- `KL_XR_REFRESH_EXT=0` — stop advertising `XR_FB_display_refresh_rate`
  (`runtime/xr/kl_openxr.c`), putting the runtime back to before SL-11. The A/B
  for anything that changes when the client can answer the host's rate
  question at all: without the extension the client publishes an EMPTY rate
  list, and a host told the client can present at no rate never starts sending
  video.
- `KL_XR_VULKAN=0` — stop advertising `XR_KHR_vulkan_enable`
  (`runtime/xr/kl_openxr.c`), putting the runtime back to before the OpenXR↔Vulkan
  bridge. The A/B for Open Brush, and it restores the original failure
  **exactly**: the guest's `xrCreateInstance` returns
  `XR_ERROR_EXTENSION_NOT_PRESENT`, its own startup diagnostic prints
  `XR_KHR_vulkan_enable (MISSING)`, and XR shuts down before a swapchain exists.
  Left at 1 (the default), the extension is still withheld on a checkout with no
  MoltenVK vendored — the gate is `kl_vulkan_xr_supported()`, so a build that
  cannot back the promise never makes it.
- `KL_XR_EXTRA_EXTENSIONS="<name> [<name> ...]"` — append names to the
  extension list `xrEnumerateInstanceExtensionProperties` advertises
  (`runtime/xr/kl_openxr.c`). **Scouting only, and a lie by construction**: an
  extension named here has no entry points behind it, so a guest that takes us
  up on it resolves NULL or aborts by name. It exists so that "what does this
  guest do differently if the runtime claims X?" costs a run instead of a
  rebuild. Each name is announced once at startup. The motivating case is
  VRChat, which requests exactly one feature extension —
  `XR_VALVE_frame_controller_interaction`, out of Valve's Steam Frame OpenXR
  package — and is told by Unity's own startup diagnostic that the runtime does
  not support it; that made it the only thing in the whole boot the guest asked
  for and did not get, and therefore the only cheap suspect for a gate.

## Reprojection (`runtime/gfx/kl_reproject.c`)

The composite/timewarp pass — one file, compiled by both compositors
(`KleptonCompositor.swift` on device, `kl_view_mtl.m` in the viewer).

- `KL_OVRP_TOUCH_FRAME=0` — hand an OVRPlugin guest the grip pose the platform
  publishes, rather than the pose an Oculus Touch's TRACKED origin would have
  had. On by default: the render model is drawn around the tracked origin and
  that is the pose OVRPlugin reports, 10.2 cm back along the handle and pitched
  20.6 degrees off the grip (`tools/rendermodel_frames.py`). An OpenXR guest is
  unaffected either way — it asks for the grip and gets it.
- `KL_XR_PREDICT=1` — predict the head pose to the `displayTime` the guest
  named instead of returning it as it stands now from the pose entry points.
  **Off by default since 2026-10-15** (the owner tested games with and without
  it on the headset and they ran fine either way); the OVRPlugin half of the
  same default is one pose per guest frame, which is always on (see "ONE POSE
  PER GUEST FRAME" below). The app's Advanced › Test switch *XR prediction*
  sets `KL_XR_PREDICT=1` (OpenXR games only since the OVRPlugin switch
  `KL_OVRP_ONE_POSE` was removed); an installation can start it on
  (`KLEPTON_TEST_ON=KL_XR_PREDICT`, AVP Play's per-game option). The launch
  line wins over both.
  The prediction is the frontend's (a timestamped device-anchor query, walked
  back 5 ms at a time until the tracker answers), capped 50 ms ahead, and it is
  computed ONCE per (frame, instant): `xrLocateSpace`, `xrLocateViews` and the
  layer placement in `xrEndFrame` all read the same one, because a guest that
  renders from one of them and submits from another otherwise draws from one
  head and declares another. Off is exactly what this did before the seam
  existed. No host frontend publishes a tracker at all, so on host this knob
  does nothing and every pose is the latched head. The log says the value once,
  at the first pose a guest asks for: `[xr] head prediction ON|OFF`. Every
  OpenXR guest goes through it — Android OpenXR games, Steam Link, and
  Half-Life: Alyx through `kl_lx_openxr.c`. It is no longer switched live from
  the metrics panel; that tap now switches MetalFX (see the metrics panel).
- `KL_XR_POSE_TRACE=1` — once a second, the head each pose entry point answered
  with this frame and the head the guest actually submitted, differenced
  pairwise (worst degrees and metres over the second) with a count of the frames
  each source was asked on. This is how "the guest draws from one head and
  declares another" is measured rather than inferred from its source, and it is
  the instrument for any swim or slosh on head motion. A row of zeros says the
  runtime is consistent and the fault is elsewhere.
- `KL_HEAD_MIDPOINT=0` — report visionOS's device anchor as the guest's head
  pose and rotate the timewarp about it, which is what this did before the
  midpoint was measured. OpenXR defines VIEW space as the point midway between
  the eyes and the device anchor is not that point, so the default (on) takes
  the midpoint from the average of the drawable's two eye transforms, publishes
  head poses and eye offsets about it, and restates `device_from_view` to
  match. Latched once, from the first drawable: a streaming guest sends its eye
  transform to the host once and the host will not revise it. The measured
  offset prints as `[cp] eye midpoint`.
- `KL_REPROJECT_DEPTH=<m>` — how far out the reprojection quad is placed
  (default **500 m**, the same distance ALVR uses). The quad is eye-centred, so
  this changes nothing about our own picture: it is what the SYSTEM's
  depth-based reprojection is told about our content, and placing it far away is
  what keeps that correction rotational. Far is not a clipping risk — the
  drawable reports `depthRange = (far inf, near 0.1)`, so nothing is ever
  discarded for being too far; a frame that vanishes is depth WRITES being off,
  not this number.
  **This is the swim knob.** The composite corrects the head's TRANSLATION as
  parallax for a plane at this distance, and at the 500 m default that
  correction is nothing — so the world slides whenever the head translates, and
  a head TURN translates the eyes by several centimetres about the neck. That
  is why yaw swims and pitch (whose eye displacement is vertical and in depth)
  does not. A few metres, near the content, is the experiment: `2`, `3`, `5`.
  The correction is exact for content at this distance and wrong for the rest,
  which is the best a single plane can do.
- `KL_REPROJECT_TRANSLATE=0` — correct rotation only, which is what the
  composite did before the parallax above. Inert at the default depth, so this
  is only meaningful together with a finite `KL_REPROJECT_DEPTH`.
- `KL_CHROMA=1` — chroma-key the guest's picture so the room shows through
  wherever it drew the key colour. Ported from VisionOSALVRClient with its maths
  and defaults intact, so a value found in that client transfers. Off by
  default. Applies to the EYE pass only — composition-layer quads (a guest's own
  panels) are not matted.
- `KL_CHROMA_COLOR=<r,g,b>` — the key colour, 0..1 per channel, default
  `0.0627,0.4863,0.0627` (16, 124, 16). Matched in HSV with hue weighted four
  times value and twice saturation, which is what makes it a key rather than a
  colour comparison: a green screen under uneven light varies far more in
  brightness than in hue. A pixel with almost no saturation or value is always
  KEPT — black and white have no meaningful hue, and measuring their distance to
  the key would matte out every shadow.
- `KL_CHROMA_RANGE=<min,max>` — the HSV distance fade band, default `0.35,0.7`.
  Below `min` a pixel is fully keyed out, above `max` fully kept, between them it
  fades; a band rather than a threshold is what stops the matte having a hard
  jagged edge.
- `KL_HAND_MATTING=0` — stop the system drawing the user's own hands and arms
  over the guest's picture (`.upperLimbVisibility`). On by default, which is what
  visionOS does in `.mixed`; off is what a guest that draws its own hands wants,
  because two pairs of hands in one place is worse than either alone.

  All four are seeded into the **Matting** panel in the boot window and are
  PERSISTED from there (UserDefaults, JSON, key `klepton.chroma`) — unlike the
  controller dials beside them, because a key colour is a property of the room
  rather than of a debugging session. Precedence at launch: an explicitly set
  environment variable wins, then the saved settings, then these defaults.


- `KL_HAND_TUNE=1` — keep an installed game's start window open once the
  game is up, showing the hand-position sliders (`HandTuneView` in
  `KleptonTuning.swift`): the Sense controller's pitch and offset, applied to
  every pose while they are dragged and logged as `[tune] hand position:`.
  LES MILLS BODYCOMBAT's owner set its values this way; they are its default
  (`KLSenseTune.targetHand`). Gun Club VR has its own there too, as its owner
  aligned the gun's grip: pitch as for every title, offset
  `-0.005,-0.010,0.010` (3 cm lower and 3 cm further back than the general
  `-0.005,0.02,-0.02`). The "Controller alignment" panel's Reset goes back to a
  title's own values where it has them. The game's own grip angle (-17°) and
  Gun Grip Offset (None) are that owner's saved in-game choices, not set here.
- `KL_SENSE_PREDICT_MS=<ms>` — how far into the future a Sense-controller pose
  may be predicted. 0 is the measured pose; the default 50 is the same horizon
  `kl_openxr` clamps the HEAD to, so both poses a guest reads are bounded by one
  number. **A time cap, not a fraction of the frame interval**: a tracker's
  extrapolation error is a function of how far ahead it reaches, and a fraction
  would mean a different horizon at 90 Hz than at 60. Both extremes were
  measured wrong on Steam Link — full prediction overshoots (we predict AND
  publish a velocity, and SteamVR predicts again to the PC's photon time), none
  lags. The app logs the measured interval and whether the cap binds; if
  presentation is nearer than the cap, the cap does nothing and wants lowering.

- `KL_REPROJECT_NOCANT=1` — treat `device_from_view` as having no rotation;
  the A/B for the eye-cant handling.
- `KL_SRGB_DECODE=0|1` — force the composite's sRGB→linear decode on or off,
  overriding what was measured. **The default is measured, not fixed**:
  `kl_glfb` sets it when the guest disables `GL_FRAMEBUFFER_SRGB` *and* its eye
  texture is an sRGB format, which is Steam Link (Beat Saber's is RGBA16F, so
  it stays off there and nothing about that path changes).
  The reason it exists at all: `EXT_sRGB_write_control` lets a guest say "the
  values I am writing are already sRGB code values"; ANGLE does not expose it,
  ES applies the encode anyway, and the composite then samples that back and
  treats an sRGB code value as linear — which is **the picture reading too
  bright**. One more decode undoes it. The whole question is a judgement about
  brightness with a person as the only instrument, so being able to flip it
  inside one session is the difference between settling it and arguing about
  it. `make reproject` gates the shader half (128 → 55).

## Viewer (`runtime/gfx/kl_view.c`, `mains/m_boot.c`)

- `KL_POKE_CAP=<n>` — at frame-pump start, overwrite libunity's texture-unit
  cap (the value its `SetTexture` path checks before logging "Invalid texture
  unit!") with n. The raise is 64, matching the 32-sampler ANGLE rebuild:
  Unity defaults the cap to 32 without ever querying GL for it, while its
  HLSLCC-baked sampler bindings reach unit 35 on the post passes, so binds past
  32 are refused and those samplers read stale unit-0 textures.
  `KL_POKE_CAP_OFF=1` leaves the field alone.

  **The singleton address and field offset are per-Unity-build and apply to no
  other**, so the store is GATED on the version stamp read out of the mapped
  libunity (`unity_version()` in `runtime/guest/kl_guestpoke.c`), and each
  measured row carries whether the raise is USED on that build. On an unlisted
  version, or on a measured row that is not used, it skips and names which.
  Setting `KL_POKE_CAP=<n>` explicitly forces it past both gates — do that only
  with re-measured offsets, since the write is a 4-byte store through whatever
  the offset happens to point at. Beat Saber 1.6.0 (Unity 2018.4.4f1) found
  this the expensive way: the unguarded read took the lifecycle down with a
  SIGSEGV inside `recon_run`, which reads like a shim bug rather than an
  expired constant.

  **Not used on Beat Saber** — the Unity 2019.4 and 2022.3.33f1 rows are
  measured and declined. The engine's own cap check is load-bearing as a guard:
  at 32 it refuses the out-of-range bind itself and the frame is only wrong.
  Raised, the bind instead reaches a GL that clamps per-stage texture image
  units to 32 regardless and refuses it there, and the engine believes the unit
  was set, indexes its own unbounded GL state cache with it and writes through
  the function-pointer table every GL call dispatches through — the process
  dies thousands of frames later in an unrelated subsystem, branching through a
  slot holding whatever the page held, on device and in the viewer alike.
  Bisected to the commit that first ran the raise on the headset, and confirmed
  by an A/B on device, 2026-08-19. **Marked for deletion** — see the TODO on
  `kl_guest_poke_texture_unit_cap()` in `runtime/guest/kl_guestpoke.c` for the
  two fixes that would retire it, neither of which needs an offset into
  anything.

- `KL_LOG=<file>` — where `build_run_viewer.sh` keeps the run's output, default
  `/tmp/viewer.log`. Not a runtime knob: the script `tee`s through `script`, so
  the pty forces line buffering and a death by SIGNAL still lands the
  `kl_fault` report — the frame chain, the guest images, the managed method
  names and every subsystem report. The interactive viewer is the only run that
  reaches the parts of the game a pointer drives, and until this existed a
  crash there left nothing but an OS `.ips`, which names a pc and no caller.
- `KL_VIEW=1` — interactive one-eye viewer on SDL3; WASD+mouse-look drives
  the head pose ovrp reports. Runs the guest in-process on a spawned thread (no
  guard test, no re-exec) and pumps frames until the window closes instead of
  `KL_FRAMES` times. Wants `KL_LIFECYCLE=1` beside it, or the guest stops at
  `initJni` and never renders.
  `KL_GLFB=1` is needed for a **GLES** guest and is not a hard requirement any
  more: a Vulkan guest never brings ANGLE up at all and its eye textures reach
  the compositor from MoltenVK, so without `KL_GLFB` the viewer takes the
  compositor path and waits for one from whichever source produces it. Setting
  neither on a GLES guest is a black window, and the startup line says so.
  Controls: mouse-look aims, **mouse-L = right index trigger** (the menu
  click), mouse-R = grip, `Z`/`X` = A/B, `C`/`V` = X/Y, `G`/`H` = left
  trigger/grip, arrows = right thumbstick, WASD/`R`/`F` move.
- `KL_VIEW_SCRIPT="<secs>:<yaw>,<pitch>[,c];..."` — what a person would do with
  the mouse, timed from the window opening: at `<secs>` turn the head to `<yaw>` /
  `<pitch>` degrees (the hands turn with it, as with mouse-look; yaw > 0 turns left,
  pitch > 0 looks up) and, with `,c`, click the right trigger for 0.3 s. Up to 24
  steps, each logged as `[view] KL_VIEW_SCRIPT at …`. So a run finds its own way
  through a game's menus with no one at the window. Alien: Rogue Incursion from a
  fresh userdata into its first level (2026-10-16, warm shader cache):
  `88:0,14,c;91:0,14,c;94:0,14,c;98:25,24;101:25,24,c;106:-19,18;108:-19,18,c;112:-16,10;114:-16,10,c;118:-8,10;120:-8,10,c;124:0,0`
  (play options OK, NEW GAME, slot 3, NEXT, START GAME) — once its first-run
  "Initial options" and calibration screens are behind it (OK at pitch 10).
- `KL_HOLOGRAM=0` — (`KleptonCompositor.swift`) Half-Life: Alyx's loading screen as the plain card
  instead of the amber map rebuilt from the game's files (`KleptonHologram.swift`, `make hologram`).
  `KL_LOADING_CARD=0` turns off both.
- `KL_STATS_CARD=1` with `--view` — the metrics panel in the window, drawn by
  KleptonMetrics.swift from `build/libklvm_panel.dylib` (`make viewer-panel`;
  `KL_VIEW_PANEL_LIB` names another path), composited with the headset's depth:
  a depth buffer, the eye picture nearer-wins, the guest's quads as
  KleptonCompositor draws them (`KL_OVERLAY_SUBMIT_ORDER` and its per-title
  default), the panel over everything (`KleptonOverlayDepth.statsCard`). Without
  the dylib the panel is not shown and the viewer says so once; without
  `KL_STATS_CARD` nothing about the composite changes.
  `KL_VIEW_CARD_DEPTH=nearer` draws the panel nearer-wins instead — the headset
  through toolchain 563, where a quad of the game's nearer than 1.2 m hid it
  (Alien's first level: a 0.94 x 0.79 m quad 0.83 m ahead).
- `KL_VIEW_AIM_AT_EYE=1` — put both hands at the head position. Beat Saber's
  menu pointer (`VRUIControls.VRGraphicRaycaster`) casts from the controller
  transform, and the default head-relative hands sit 0.25 m below your gaze,
  so the ray lands 0.25 m low at any panel distance. With this set the ray is
  the gaze ray and the viewport centre is a crosshair. Off by default: the
  offset is the honest emulation and is what puts the in-game controller
  models where a body would hold them.
- `KL_VIEW_POKE="fx,fy@secs[;fx,fy@secs]..."` — mono guests only. A sequence of
  synthetic clicks (up to 12) at fractional **guest surface** coordinates,
  delivered through the same `SDLActivity.onNativeMouse` a real click uses.
  Hover, press and release land on three separate ticks, which is load-bearing:
  pressed and released inside one iteration takes a button's highlight and
  produces no click. Exists so the input path can be proved without posting a
  CGEvent at the desktop, which clicks whatever window is really under that
  point.
  Not a viewer knob any more despite the name (SL-18): it lives in
  `runtime/guest/kl_mono.c` and is driven from `kl_slink_sdl_pump`, i.e. the Android
  UI thread — where a real MotionEvent would be delivered, and the one thread
  guaranteed to keep turning. So it needs **no window at all**, which is what
  makes it usable inside the visionOS simulator, where `simctl` runs the app
  headless and there is nothing to click.
  Every `secs` is measured from the SAME zero — the mono transition — not from
  the click before it, so a run is described by when each screen is expected
  rather than by gaps that have to be re-derived when an earlier screen gets
  slower. A poke whose deadline has already passed fires as soon as the one
  before it finishes, so the order is always the written one. This is what
  drives Steam Link's shell to pairing:
  `KL_VIEW_POKE="0.599,0.576@28;0.498,0.840@31"` picks the host tile and then
  *Start Pairing*.
- `KL_VIEW_CPU=1` — force the viewer's old readback path: `glReadPixels` the
  whole eye, tone-map it, memcpy it to the sink, row-flip it, upload it.
  Measured 23.5 fps against the hardware compositor's 54.7 on the same scene,
  and it needs no Metal interop — which is what makes it the A/B when the
  compositor shows the wrong picture. **Required, not optional, for a guest
  whose OpenXR eye swapchain is an ARRAY** (VRChat): the hardware compositor
  samples the eye as an MTLTexture and ANGLE's Metal EGLImage is always 2D, so
  `klxr_back_eye_images` refuses the swapchain by name and there is nothing to
  composite. The readback path states the layer instead
  (`kl_glfb_set_live_eye_image`) and works.

  Which door takes which path is otherwise automatic and readback is nowhere
  on it: Steam Link's VR door composites through the eye/overlay provider
  textures like every XR door, and a FLAT guest (the Qt shell / client doors)
  renders into an IOSurface-backed pbuffer the compositor samples directly
  (`kl_glfb_request_flat_surface`; the run says `flat guest — IOSurface
  composite, no readback`). `KL_VIEW_CPU=1` is the one switch that brings
  `glReadPixels` back for either shape.
- `KL_VIEW_EYE=1` — composite the **right** eye instead of the left. The window
  shows one eye, and which one is not a detail: a guest under Oculus symmetric
  projection renders the two eyes into different sub-rects of one texture, so
  eye 1 is the one whose crop and whose quad can be wrong
  while eye 0 looks perfect. With `KL_OVRP_EYE_TAN=vision` this is the whole
  canted-stereo failure, on macOS.

## Audio (`runtime/media/kl_audio.c`)

The CoreAudio output sink behind `kl_opensl.c`'s buffer queue and
`kl_aaudio.c`'s streams: rebelancap's sink (his commits 3fc48c9 and 975cad6),
the only audio path since the switch between it and the earlier one
(`KL_AUDIO_SINK2`) was removed. Every guest sample format (8, 16, 24, 32 bit,
float) is widened to float before the resampler; each OpenSL player and AAudio
stream is a source of its own, and ONE owns the output — the first to hand over
a buffer that is not silence; the others are paced for their buffer's length
and dropped, and the owner changes after 2 s of its silence while another
source is heard. An empty buffer between two blocks of an AAudio guest's mixer
is waited out of the sink's lead instead of played. `dlsym` on libaaudio
answers NULL for a name that is not served. A census is printed every 15 s
(`[au] …` per source, `[aaudio] …` per stream) and one line in the `[hb]` loop.
The device stays open until the last source has closed.

- `KL_AUDIO=0` — no CoreAudio device at all: the OpenSL feeder goes back to
  pacing each buffer with `usleep` and dropping it, which is what this runtime
  did for its whole life before `kl_audio.c`. The A/B for anything that looks
  like an audio-induced timing change — and read **by value** (`kl_env_on`),
  since it defaults on.
- `KL_AAUDIO_BURST=<frames>` — the size of every pull an AAudio guest's mixer
  is asked to serve, 32..8192, when the guest does not set its own with
  `setFramesPerDataCallback`. Default **256** (since 2026-10-13; it was 240):
  a power of two divides the fixed blocks sound engines mix in, where 240
  against a 1024-frame block left every fifth pull with 176 zeros, a fine
  crackle. Five titles whose Wwise was heard to need it get **1024**: AC Nexus
  (x5812519008825194), The Walking Dead: Saints & Sinners (x2897337400373711),
  Tales from the Galaxy's Edge (x3484270071659289), Pistol Whip
  (x2104963472963790) and Tetris Effect (x3386618894743567). A value given
  here overrides all of that.
- `KL_AAUDIO_OLD_BURST=1` — the default is 240 again, for the games that had
  it (the five Wwise titles keep 1024). The way back if a game sounds worse at
  256; a switch in the start window's test group.
- `KL_AUDIO_DUMP=<path>` — tee exactly the frames the render callback will
  hand the hardware into a WAV. The only way to check *what* is being played
  rather than merely that something is: counts and peak levels cannot tell
  music from a wrong resample ratio or swapped channels. Records what the
  producer accepted, contiguously, so pauses are elided and the file is
  shorter than the run.
- `KL_AUDIO_SMOOTH=0|1` — (`runtime/media/kl_audio.c`, `pacing_reader`) let a
  writer waiting for room see the read position carried forward by the clock
  between two device callbacks, instead of where the last callback left it.
  The device takes a whole IO buffer at a time (10 ms on visionOS, which would
  not grant 5), so FMOD's 5.3 ms blocks were otherwise taken back two at a time
  and its mixer clock — the one Unity's VideoPlayer times a clip with sound by —
  advanced in 10 ms jumps, one refresh at 100 Hz. Default on for LES MILLS
  BODYCOMBAT (x4015163475201433).
- `KL_AUDIO_LATENCY_MS=<n>` — how much audio the ring aims to hold (default
  80, clamped 10..500). This is the whole latency budget — the ring itself is
  a second long so a hiccup cannot wrap it, but the *fill target* is what the
  producer blocks against. Raised automatically if FMOD's buffer turns out
  larger than half of it.
- `KL_AUDIO_TRACE=1` — one line every 200 writes: input/output frame counts,
  ring fill in frames and ms, underruns. What to turn on when the sound is
  present but wrong.
- `KL_AUDIO_MIX_SOURCES=0|1` — no owner: every source reaches the device,
  MIXED. Each is widened from its own format and resampled from its own rate
  with its own resampler position, and summed into the ring at a cursor of its
  own; one stream pausing or flushing leaves the others playing. **Default on
  for Steam Link** (steamlink-vr), which keeps two AAudio outputs open during a
  stream — the 2D shell's SDL3 one (float, 960 frames) and the VR client's own
  (int16, 256) — and both carry sound (community fix #49). **Also on for
  Roboquest VR** (`x9269393463181825`): FMOD's OpenSL player and Unreal's own
  mixer both carry the game's sound. While two sources
  mix, the periodic `guest PCM so far` line and the 15 s census are followed by
  one line per source: its share of real time since it came and how often it
  fell behind the reader and was resynced. `make audio-mix` checks it against
  the real output device.
- `KL_AUDIO_MIX_UNLOCK=0|1` — while sources mix, let a writer that waits for
  room nap *outside* the mix lock, so the other stream is not held up behind
  it. Default on for Steam Link, Pistol Whip (x2104963472963790) and
  Roboquest VR (x9269393463181825): Steam
  Link's pair managed ~200 writes a second where real time is ~237 with the
  nap under the lock — dropouts every few seconds.
- `KL_AUDIO_SOURCE=opensl|aaudio` — force the owner: only that backend reaches
  the device.
- `KL_AUDIO_PREFER=opensl|aaudio` — while a stream of that backend is open it
  owns the output from its first buffer, heard or not, and the others are
  paced and dropped; with none open, the usual rules. **Default `aaudio` for
  Pistol Whip**, whose Unity OpenSL player is near-silent beside Wwise's AAudio
  stream (community fixes #26/#27: AAudio owns the device exclusively).
- `KL_AUDIO_STEAL_MS=<n>` — how long the owner must be silent before another
  source that is heard takes over (2000).
- `KL_AAUDIO_HOLE_DROP=<n>`, `KL_AAUDIO_SEAM_WAIT_MS=<ms>`,
  `KL_AAUDIO_SEAM_LEAD_MS=<ms>` — the block-seam handling; `…_WAIT_MS=0`
  drops an empty buffer and pulls again without waiting.
- `KL_AUDIO_REPORT_SECS=<n>` — the census interval (15; 0 turns it off).
- `KL_AAUDIO_DUMP=<path>`, `KL_AUDIO_DUMP_S=<n>` — a WAV of what the guest's
  callback handed over (beside `KL_AUDIO_DUMP`, what goes to the device), `n`
  seconds each (20), closed by themselves.
- Gates: `make audio-formats` (sample formats, OpenSL sources) is in `make
  check`; `make aaudio-seam` (the block-seam harness, 20 s) and `make
  audio-mix` (two streams mixed) run in real time against the output device
  and are not.
- `KL_AUDIO_SPATIAL=1` — visionOS only. Leave the system's spatialiser in the
  path. The default is `setIntendedSpatialExperience(.bypassed)` plus two
  preferred output channels (`KleptonAudio.directStereo`, imported from ALVR's
  `fixAudioForDirectStereo`): the guest hands us a mix it has already panned
  for a head-mounted listener, and visionOS otherwise pans it a second time
  into a sound stage anchored to the app's scene — which is the sound following
  the window. Turn this on only to A/B that.
- `KL_AUDIO_WATCHDOG=0` — stop the independent watchdog thread from running.
  **On by default, and it is the thing that keeps audio alive on visionOS.**
  This OS silently stops calling CoreAudio's render callback across a scene
  transition — the Digital Crown pressed to passthrough, the boot window closed
  while the immersive space runs — with no error, no interruption notification,
  and an output unit that still reports itself started. ALVR carries the same
  heartbeat against the same bug. The check used to be reachable only from the
  spin inside `kl_audio_write`, which covers "the callback died while the guest
  kept producing" and misses "the guest stopped too" and "an interruption that
  never ended"; it has a 4 Hz thread of its own now. Turn it off only to see
  the failure it hides.
- `KL_AUDIO_INTERRUPT_MAX_MS=<n>` — how long an interruption may last with no
  matching "ended" before the watchdog assumes the notification was lost and
  restarts anyway (default 3000). `.began` without `.ended` is a documented
  hazard on this OS family and used to be silence for the rest of the run,
  because the write loop breaks out of its spin *before* the watchdog on that
  flag. Raise it if a real interruption is being fought over.

### Microphone (capture: `kl_audio.c` cap_build, `kl_aaudio.c` klaa_capture_into)

Armed by the KleptonMic toggle and opened on the first guest input stream
(Steam Link's `AAudioStream_read`, Walkabout's Photon voice through
`kl_jni_services.c`). The unit is VoiceProcessingIO with its AGC asked on, its
output element disabled (so its echo canceller has no reference), capturing
int16 at the device's native rate and channel count; `kl_aaudio.c` converts to
the guest's ask — one-pole low-pass + linear interpolation whenever the guest
asks below the native rate (Steam Link's 16/24 kHz voice against 48 kHz).

- `KL_MIC_AGC=0` — ask VPIO's automatic gain OFF (VPIO's own default is on,
  so this sets 0 rather than skipping the set). The A/B for "others hear me
  overdriven". Default 1. A Test switch in the start window.
- `KL_MIC_RAW=1` — capture through plain RemoteIO: no voice processing at all
  (quiet, ~759/32767 on Vision Pro, room and game sound included). The other
  half of that A/B. Default 0. A Test switch in the start window.
- `KL_MIC_REPORT_SECS=<n>` — interval of the two mic lines below (default 5,
  0 = none). While the capture unit is open, `[au] mic: <unit> AGC on|OFF|n/a
  <rate>/<ch> | … s captured (+…), … s read (+…), … s dropped (+…) in n s |
  peak …/32767 (… dBFS), X of Y buffers at full scale | callback N frames
  (min..max)` — peak and full-scale count are since the line before. A
  growing "full scale" count is a hard clip in the int16 client format; a
  "dropped" that grows is the guest not keeping up. Beside it, per input
  stream, `[aaudio] mic AAudioStream_read|callback: n calls in … s, k short,
  z empty, got of asked frames, last asked F with timeout T ms | guest
  format <- mic format (ratio, low-pass + linear downsample | straight copy)`.
  The one-time `[au] microphone capture:` line names the unit and the AGC
  as read back from it.

## x18 (`runtime/kl_x18.c`)

- `KL_X18=0` — disable x18 veneering (also `n`/`N`, matched on the first
  character). The survey counts sites either way, so an A/B run compares like
  with like. Default on. This is the A/B that identified the veneer pass as
  the cause of Steam Link's failing TLS handshake: with it off the handshake
  completed, with it on the guest answered `bad_record_mac`.
- `KL_X18_PAIR_POOL=0/1` — treat a lone adjacent pair of "unallocated" words
  in an x18 site's ±16-word window as a two-entry literal pool (patch the site)
  rather than as data (refuse it). Read by klepton-ld at translation time as
  well as by the loader. Default on only when `KLEPTON_TARGET` is
  `x1906871599408213` (Robo Recall: Unplugged, whose PhysX faulted on six
  sites the window had refused); off everywhere else, because the same rule
  also unrefuses one word of BoringSSL's P-256 table in Beat Saber 1.40's
  `libcrypto.so`, `libEOSSDK.so` and UE5's `libUnreal.so`.
- `KL_CTR=0` — disable trap 26's `mrs Xt, CTR_EL0` veneer. Deliberately its own
  knob rather than part of `KL_X18`: the two rewrites fix different registers,
  and an A/B on one must not silently move the other. Default on. **The control
  arm crashes**: an un-veneered `mrs Xt, CTR_EL0` is SIGILL from EL0 on every
  Darwin kernel, so this is how you reproduce that on purpose, not a fallback.
- `KL_CTR_EL0=<hex>` — what the veneer answers, default `0x8444c004` (Apple
  silicon's own value: 64-byte lines, IDC=0, DIC=0, so `__clear_cache` really
  does its `dc cvau` / `ic ivau` maintenance, both of which ARE legal from EL0).
  `0xb444c004` sets IDC and DIC, which makes `__clear_cache` a pair of barriers
  and skips both maintenance loops — the fallback if a kernel ever traps those
  too. See `KLX_CTR_EL0_VALUE` in `runtime/kl_x18.h`.
  **Baked in at translation time**, so changing it means re-translating
  (`make dylibs`, `visionos/mkguest.sh`); `make ctr` prints the value in force
  and `klepton-ld` prints it per library that has a site.
- **There is no knob for where the veneer POOL goes, and that is deliberate.**
  It is reserved on the end of the guest image's own mapping (`kl_image.c`), so
  it is within `b`'s ±128 MB of the code by construction. It used to place
  itself with an `mmap` hint just past the code — which is only a hint, and the
  address right after an image is the one guaranteed to be taken. For libUE4
  (172 MB) under a viewer run the nearest hole measured **240 MB** away and all
  8038 veneers were refused, silently. A refusal is now printed by name at load
  with the pool's distance; if you ever see that line, the library is running
  against Darwin's reserved x18 and any crash in it is trap 0.
- `KL_HWCAP=<hex>` — override the measured `getauxval(AT_HWCAP)` outright
  (`kl_libc_slink.c`). An A/B, not a tuning knob: the bits select whole
  hand-written assembly implementations, so `KL_HWCAP=0x3` (FP|ASIMD only)
  moves BoringSSL onto its portable C path. That is what separated "our
  runtime mishandles ARM crypto assembly" from "the fault is elsewhere" —
  the failure survived, and the cause turned out to be corrupted constants
  rather than corrupted code.

## Signals / tracing

- `KL_TRACE_SIG=1` — log `sigaction`, `pthread_kill`, and `sem_post` (the
  first 12 posts) as the guest makes them. Read by `kl_libc.c` and
  `kl_pthread.c`.
- `KL_GUEST_SIGNALS=1` — install the guest's fatal-signal handlers (SIGSEGV,
  SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP, SIGSYS) instead of refusing them.
  Read by `kl_libc.c`. Default refuse: a guest handler expects a Linux
  ucontext and wedges inside Darwin's `_sigtramp`, costing every crash its
  diagnosis.
- `KL_TRACE_FS=1` — log every guest file op and its result; `=fail` logs only
  the failures. Read by `kl_libc.c`. Any other non-empty value means "all".
  Covers open/fopen/access/mkdir/rename/unlink **and stat/lstat**. The stat
  pair was added after their absence produced a wrong conclusion: managed
  `File.Exists()` is a stat and never an open, so Beat Saber's probe for
  `files/settings.cfg` and `files/PlayerData.dat` (plus both `.bak`s) was
  invisible, and the game looked like it was showing its first-run gate
  without ever checking saved state. It checks; the files simply do not exist.
  `open` lines carry the call's flags and descriptor (`flags=0x241 fd=37`).
  For Gun Club VR (`x2285803881494677`) the operations on its preference file
  (`TBMPrefs` and its temporary names) are logged with this unset too, names
  and results only, never contents.
- `KL_TRACE_IMAGES=1` — log each loaded image's base and span, the stub pool
  mapping, and each emitted stub. Read by `kl_image.c`.
- `KL_TRACE_MMAP=1` — log every guest `mmap`/`mprotect` (`[mm]` lines): the
  request, the result or errno, and the caller as image+offset. Read by
  `kl_libc.c`. The instrument for a guest that runs its own ELF loader: which
  call left a page without the permission it later needed. Unaligned
  `mprotect` on 16 KiB pages is widened (write) or narrowed (otherwise) and
  named in `[klepton] mprotect(...) is not 16 KiB-aligned` either way.

Signal numbers: Linux and Darwin disagree on SIGUSR2 (12 vs 31) and SIGSYS
(31 vs 12), and Darwin 12 is fatal, so a guest's SIGUSR2 handler was refused
and its `pthread_kill(…, SIGUSR2)` killed the process. That one pair is
swapped on every entry point that takes a signal number or mask (`sigaction`,
`signal`, `kill`, `raise`, `pthread_kill`, `pthread_sigmask`, `sigwait`,
`sigsuspend`, `sigpending`); every other number passes through as before. `KL_SIG_SWAP=0` restores the pass-through
of both numbers (this fork's switch; read once).

## Steam Link (`runtime/guest/kl_slink.c`, `build_run_host.sh`)

Steam Link is a target row like any other (`./build/m_boot steamlink-vr`); the
front door is a knob, not a build. `build_run_host.sh`'s flags map onto these:
`--gap` → `KL_GAP_ONLY=1 KL_NOFORK=1`, `--main` →
`KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1`, `--shell`/`--vr` → `KL_SLINK_SHELL=1`/
`KL_SLINK_VR=1`, `--view` adds `KL_VIEW=1`.


- `KL_SLINK_MAIN=1` — start the guest at all. On the SDL doors that is
  onCreate → `nativeRunMain` → the guest's `main` on its own thread; on the VR
  door it is the activity lifecycle after `ANativeActivity_onCreate`, which is
  where the guest reaches OpenXR. Without it the run stops at the bound chain
  (SDL) or at onCreate (VR), which are the gates that must stay green
  independently. `KL_VIEW=1` implies it.
- `KL_SLINK_SHELL=1` — open the OTHER front door: the 2D **configuration
  frontend** (`libshell_arm64-v8a.so` -> `main`, Qt6) instead of the streaming
  client (`libmain.so` -> `SDL_main`). VR APK only — the old one ships Qt5 with
  the stock `qtforandroid` QPA, the VR one ships Qt6 with Valve's own `qvirtual`,
  which imports no JNI at all. `./build_run_host.sh steamlink-vr --shell` sets it. The shell draws its own UI with no Steam host on the network; the
  client draws nothing at all without one.
- `KL_SLINK_VR=1` — open the THIRD front door: `libvrlink_scene.so`, the
  **OpenXR NativeActivity** (`ANativeActivity_onCreate`), instead of either SDL3
  half. Not a chain — its `DT_NEEDED` is entirely Android system libraries we
  shim, so one guest library is the whole working set, and `libopenxr_loader.so`
  is deliberately NOT loaded because it is replaced by `runtime/xr/kl_openxr.c`.
  VR APK only, and it says so by name if pointed at the other tree. Mutually
  exclusive with `KL_SLINK_SHELL` (VR wins, with a line saying so).
  **Both knobs reach the visionOS app too, and the DEFAULT there is different**
  (SL-18): the app opens the SHELL unless it is handed a session — either
  `KL_SLINK_VR=1` or a `KL_SLINK_SARGS` — because the shell is the only door
  that can produce one, and the VR half leaves before its first frame without
  it. `KL_SLINK_SHELL=1` forces the shell back even with a session present.
  `make slink-vr-gap` is the work list, `make slink-vr-scene` the run; with
  `KL_SLINK_MAIN=1` it goes on to drive the activity lifecycle
  (`onStart`/`onResume`/`onNativeWindowCreated`/`onWindowFocusChanged`) and then
  **pumps the main looper**, which this guest requires — it hangs its callback
  handler off the UI thread's looper rather than running work on a thread of its
  own.
- `KL_SLINK_ARGS="<space-separated argv>"` — `SDL_main`'s own options, which
  the real activity fills from the launching intent's `sArgs` extra. Without
  it the streaming client is being asked to stream nothing.
  `--transport k_EStreamTransportUDP --server <ip>` is the pair that reaches a
  connection attempt — `--transport` is load-bearing and its value is a
  protobuf enum name (`k_EStreamTransport{None,UDP,UDPRelay,SDR,UDP_SNS,
  UDPRelay_SNS}`).
- `KL_SLINK_SARGS="<ip>~<port>~<port>~0,0,1~~~~<token>"` — the **2D→VR
  handoff**, arriving. The VR half is not a standalone app: it reads its
  session out of the launching Intent's `sArgs` extra, and without one it
  prints "No sArgs and release build panic. Aborting back to SteamLink." and
  exits before its first frame. Set it and the app runs its scene setup, its
  frame loop and its SVL stack instead. The value is what SL-6 measured the 2D
  shell building after the host authorized; a synthetic
  one of the right shape is enough to get past scene setup, a real one is
  needed for an actual stream. `make slink-vr-run` carries a synthetic default.

  Since SL-10 the synthetic one no longer gets as far: the decoder comes up and
  the run reaches `SVLDataLink::InitCrypt`, which rejects the fake token and
  `DebuggerBreak()`s — SIGTRAP, from the guest, and correct behaviour rather
  than a shim failure. A real `sArgs` from a live pairing run is the way past
  it, and SL-11 measured the format from `InitCrypt` itself
  (`libvrlink_scene+0x15b368`), correcting two guesses SL-10 made from the
  outside:

  - The string is scanned for **seven** `~` separators and the token is
    everything after the seventh; the `int` argument the log prints
    (`InitCrypt(...,82,1)`) is the **length of the whole sArgs string**, so a
    stray character picked up while copying it out of a log shows up there.
    Extract it with `tr -d '\r'` — runs go through `script`, so the log lines
    end `"\r`, and 82 quietly becomes 84.
  - "Unknown / confusing key identifier: %s" prints four characters because the
    token *begins with a four-character key identifier* (`MID0` on every token
    this host has issued), which the guest copies into a five-byte buffer. It
    is **not** a truncated print of the whole token, and the token's length is
    not the problem.
  - The rest is base64 with the URL-safe alphabet (`...789-_`, and `+` is
    **not** accepted — converting to the standard alphabet makes it worse),
    decoded to at least 5 bytes for transport mode 1 (XOR0) or 68 characters
    for mode 2.

  A token that fails with **"XOR0 transport specified, but len too short"**
  after all that is trap 19, not a bad token.
  **Since SL-15 you rarely set this by hand.** `KL_SLINK_HANDOFF` (below) makes
  the shell re-exec into the VR front door with the session it just earned, so
  `KL_SLINK_SARGS` is for replaying an old session or driving the VR half alone.
- `KL_SLINK_HANDOFF=0` — **disable the automatic 2D→VR handoff.** On by
  default. When the shell reaches `SteamLink.startVRLink(String)` — it has
  paired and the host has authorized — the driver re-execs itself into the
  OpenXR front door carrying the session as `KL_SLINK_SARGS`, which is what
  Android does at that point too (a new activity in a fresh task, the old one
  calling `finishAndRemoveTask()`). Re-exec rather than an in-process
  transition because the two are different front doors with different chains,
  and libshell's `main` is on the stack of the thread making the call.

  It fills in only knobs that are **unset**, and prints each one it sets:
  `KL_SLINK_MAIN=1`, `KL_GLFB=1`, `KL_OVRP_IPD=0.063` (the host stopgap — an
  IPD of zero stops the host sending video at all, SL-12) and, when not
  windowed, `KL_SLINK_WAIT=45`. `KL_VIEW_POKE` is *unset* on the way through: it
  is a click script for the shell's UI.

  `0` restores the old behaviour — print the session and abort by name.
  `make slink-shell` sets it, because credentials persist and a machine that has
  paired once can reach the handoff with nobody clicking anything; that gate
  measures the shell, not the VR half.
- `KL_SLINK_START_INFO` / `KL_SLINK_ORIG_PACKAGE` / `KL_SLINK_ORIG_ACTIVITY` —
  the other three extras from the same Intent. Unset means absent, and absent
  is not the same as empty: with none of the four set, `getExtras()` answers
  null, which is what a normally-launched activity sees.

  `KL_SLINK_START_INFO` is **derived** when unset and `KL_SLINK_SARGS` is set:
  the shell does not carry an independent value, it splits `sArgs` on `~` and
  forwards field 3 (`SteamLink.startVRLink`), so we do the same. Setting it
  overrides the derivation.
- `KL_XR_BINDINGS=1` — print every suggested action binding **and what it
  decoded to**, not just the count per interaction profile, so it is off by
  default; turn it on when the question is which concrete input path an action
  expects to be driven from. It reports the decode rather than only the path
  because a binding we take and one we silently fail to recognise look
  identical otherwise.

  **Measured (SL-20, the first run of this knob in the repo):** six profiles,
  39–45 bindings each, and for the active one
  (`/interaction_profiles/oculus/touch_controller`) **41 suggested, 39 taken**.
  The two not taken are `/input/thumbrest/touch` on each hand, which has no
  input source on this platform and is therefore left unbound — an action that
  reports `isActive = false` rather than a permanently-not-touched sensor we
  cannot measure. Earlier notes here said "27–33 per profile, 174 total" and
  before that "~40 for each of seven"; both were estimates off the JSON.

  The bindings are not in its code: they are read from
  `steamlink-vr/assets/config/controller_config.json`, which carries the six
  profiles, one pose action on `.../input/grip/pose`, one haptic action on
  `.../output/haptic`, and 15 input actions — so the map can be read offline
  without a run at all. The seventh "type" in the older count is the hand
  profile (`svl_hand_interaction_augmented`), which is not in that table.
- `KL_XR_LOCATE_EVERY=<n>` — how often a repeated `(call, space, base)` triple
  is re-printed by the locate census, default 600; `=0` prints each triple once
  only. The repeat is what makes it a measurement rather than a census: the
  first call happens before the head has moved, so every position reads 0 and a
  space leaking the head's own position is indistinguishable from one that does
  not.
- `KL_XR_PROFILE=0` — answer `xrGetCurrentInteractionProfile` with
  `XR_NULL_PATH` again, i.e. "no controller is bound". Default on
  (`/interaction_profiles/oculus/touch_controller`, SL-20).

  **This is the A/B for anything that got worse when the controllers
  appeared.** Answering the profile is not a local change: the guest keys its
  whole controller description off it, so with it on it publishes
  `VTE_PROPS_STATIC_L`/`_R`, streams controller state to the host every frame,
  and SteamVR then renders and encodes controller models and laser pointers
  that were not in the picture before. That is real extra work on both ends and
  more motion in the encoded frame at a fixed bitrate. Measured, so the knob is
  known to bite: `VTE_PROPS_STATIC_{L,R} was updated` appears **4 times** with
  it on and **0** with it off, same run otherwise.
- `KL_CTRL_TRACE=1` — the controller-alignment trace, **off by default**. One
  line per (seam, hand): `[ctrl] published hand N: euler XYZ …`
  from kl_ovrp when the frontend publishes a pose, and `[ctrl] openxr grip hand
  N: …` / `openxr aim` from kl_openxr after its own pitch. Once per pair, then
  only on a change of more than 2 degrees and at most once every two seconds.

  It exists because the correction is spread across three seams and no log
  joined them. The frontend applies a convention offset when it publishes
  (`KLSenseTune` in `KleptonControllers.swift`: a hilt pitch and a position
  nudge, tuned by playing Beat Saber); kl_ovrp hands that to an OVRPlugin guest
  unchanged; kl_openxr adds `KL_XR_GRIP_PITCH` to a LOCAL copy for an OpenXR
  guest. Two guests can therefore receive rotations tens of degrees apart with
  every constant behaving exactly as documented, and until both lines existed
  nothing said which seam a misalignment came from. Read the two together: the
  difference between them IS what the OpenXR path adds.
- `KL_XR_GRIP_PITCH=<degrees>` — rotate the CONTROLLER pose about its X axis,
  positive tilting forward up. **Default +37, confirmed by eye on a headset
  streaming from SteamVR.** This is the one that visibly rotates the controller.

  **It affects the OpenXR guest only, so Beat Saber is untouched** — no
  per-guest split is needed and none exists. Beat Saber speaks OVRPlugin and
  resolves *zero* of the 53 xr\* entry points (its own end-of-run report says
  `0 resolved by the guest`), and the rotation is applied to `kl_openxr`'s local
  copy of the pose rather than written back into `kl_ovrp`, so the two guests
  cannot be made to disagree by this knob. The pitch line does not even print on
  a Beat Saber run. If this ever moves into `kl_ovrp`, it needs a per-guest
  split *at that moment* — the shared seam would be the reason, not the knob.

  OpenXR gives a controller two poses and Steam Link binds both: the stream's
  `pamir-stream-pose` reads `.../input/grip/pose` — the hilt SteamVR renders
  and streams back — and the in-headset UI's `ui_pointer_pose` reads
  `.../input/aim/pose`, the ray. **The controller you are looking at is the
  GRIP pose**, so this knob applies to every action-space pose, aim included:
  an aim ray built on a mis-pitched hilt is mis-pitched with it.

  SL-20 first shipped the correction at 0 on the aim pose ONLY, arguing that
  `KleptonControllers.swift` already builds a hilt frame pointing where the hilt
  points. Both halves were wrong, and the second one is the instructive one:
  with the rotation on the aim pose alone, `KL_XR_AIM_PITCH` moved nothing a
  person could see, so the knob read as *"the rotation is not the problem"*
  while the rotation was never being applied to the pose in question. `make
  xrinput` asserts the correction reaches the grip pose now, because that
  failure has no other symptom — the position is right, the space is tracked,
  and every call returns `XR_SUCCESS`.

  **The guest's own `controller_config.json` does not predict the sign, and it
  looks like it should.** Its per-profile hilt rotations are all *negative*
  about the same axis (-20.6 Touch, -10 Pico, -5 Vive) — a negative default
  here is wrong by twice the angle. Those are the guest's
  grip-to-*device* offsets, applied on its side to a pose it already has; this
  is the correction from the frontend's hilt frame *into* the grip pose the
  guest expects, and the two run opposite ways. A plausible source that gives
  the wrong answer — do not re-derive from it. A controller off by twice the
  angle rather than merely still wrong is the tell for a sign error.
- `KL_XR_SYSTEM_NAME=<string>` / `KL_XR_RUNTIME_NAME=<string>` /
  `KL_XR_VENDOR_ID=<n>` — the device identity an OpenXR guest reads.
  Defaults `Oculus Quest2`, `Oculus`, `0x2833` (the Oculus USB vendor id).
  Steam Link (steamlink-vr, exact target) is the one exception: its system
  name defaults to `Meta Quest 3`, matching the Quest 3 (eureka) it presents in
  `Build.*` so the host streams at that panel's size (see `KL_BUILD_<FIELD>`).
  The variable still wins over that default on every target.
  `KL_XR_RUNTIME_NAME` is also what `ovrp_GetRuntimeName` answers.

  **These are the ONLY things an OpenXR guest can identify the device by**, and
  they have to agree with the identity every other seam gives. We present a
  Quest 2 in `Build.MODEL`, `Build.PRODUCT` and `ovrp_GetSystemHeadsetType` (9 =
  Oculus_Quest_2) because reporting anything else fails every Oculus branch in a
  guest — but an OpenXR guest sees none of those. It has the interaction profile
  (already `oculus/touch_controller`), these three, and nothing else.

  They used to answer `Klepton HMD` / `Klepton` / vendor 0, i.e. "a runtime I
  have never heard of", and the guest then fell back to its DEFAULT controller:
  JKXR and Open Brush both drew **Quest 1** controllers while every OVRPlugin
  guest drew Quest 2s. That is not cosmetic — a controller model carries its own
  grip-to-model transform, so the guest applies a pose offset in its own frame
  that nothing here can see or cancel, which is a per-title alignment error by
  construction.

  Knobs because clients branch on these strings. Steam Link already showed
  Quest 2 controllers (its identity comes from the Steam host, not from here),
  so it is the target to watch for a regression.
- `KL_XR_GRIP_PIVOT="x,y,z"` — metres, in the grip's own frame: **the point the
  grip pitch should turn ABOUT**, i.e. the point that must not move when the
  correction is applied. `_L`/`_R` per hand. Default 0,0,0 (the old behaviour).

  `klxr_pitch_about_x` turns the orientation and leaves the position where the
  platform put it, so the frame pivots about the tracked origin — back at the
  wrist — and everything the guest draws from that pose swings on an arc of the
  pitch angle. Measured on JKXR: the hilt ends up out by the knuckles instead of
  through the closed fist, while the pitch itself is RIGHT (at zero the same
  guest reads as "a gun grip rather than the sword grip the game is expecting").

  Prefer this over `KL_XR_GRIP_POS` below: it is a POINT you can estimate by
  looking at your hand — "the hilt centre is about six centimetres forward and
  two down from where the controller is tracked", so `0,-0.02,-0.06` — where the
  equivalent translation is a blind 3-axis sweep with no physical meaning and a
  different right answer for every pitch angle. Axes: **-Z is the pointing
  direction, +Y up, +X right**, so a hilt centre ahead of the tracked origin has
  a NEGATIVE z.
- `KL_XR_GRIP_POS="x,y,z"` — metres, in the grip's own frame, added to the
  CONTROLLER pose after `KL_XR_GRIP_PITCH`. `_L`/`_R` per hand. Default 0,0,0;
  for DreamSpace (`x9449316705178989`) 0,-0.10,-0.02 unless any of the three is
  set (its Quest controller models sat above PSVR2 Sense controllers).
- `KL_XR_LOCAL_FLOOR=0|1` — (`kl_openxr.c`) offer the LOCAL_FLOOR reference
  space (core in OpenXR 1.1; here the pose STAGE has) to an instance created
  with apiVersion >= 1.1. Default 1 for DreamSpace (`x9449316705178989`),
  Fluid (`x6946182742093178`) and Spatial Zombies (`x24151641901194680`), which
  never began their sessions without it, else 0.

  **A pitch alone rotates where the controller points and not where it PIVOTS.**
  The frame turns about the origin the platform gave us, so the point that
  should have stayed put — the hilt's centre, inside the closed fist — swings on
  an arc of the pitch angle. That is a different error from a wrong angle and it
  has its own tell: rolling the wrist sweeps a large arc and the held object
  sits out by the knuckles rather than through the curl of the fist. Measured on
  JKXR against Beat Saber, where the object tracks through the fist correctly.

  The OVRPlugin path has carried both halves all along — `KLSenseTune`'s `pos`
  beside its rotation — and adjusting that position is what fixed the pivot on
  Beat Saber and BONELAB. This is the same lever for the OpenXR path, which had
  only the rotation. Sweep it one run per value, no rebuild, the way
  `KL_SENSE_POS` is swept on the other path; `_L`/`_R` is what a CHIRAL error
  needs (an offset mirrored between the hands).
- `KL_XR_AIM_PITCH=<degrees>` — the EXTRA offset between the aim ray and the
  grip, applied only to `.../input/aim/pose`. **Default 0**: the real aim-vs-grip
  angle of this input source has not been measured, and the frontend's hilt
  frame already points roughly where a hand points. Separate from the knob above
  because they answer different questions, and conflating them is the bug that
  produced a knob which did nothing.

  Both are read once and printed together at the first `xrCreateActionSpace`
  (`[xr] controller pose: grip pitched 37.0 deg …`) — **not** lazily when a
  pose is first corrected, which never runs on a host with no frontend and so
  never said which value was in force.
- `KL_SLINK_SIZE=WxH` — the panel size, published to SDL
  (`nativeSetScreenResolution`), the `ANativeWindow` and ANGLE together via
  one `slink_panel_size()` — the display is a group answer, and ANGLE is
  sized before the guest's window surface exists.
- `KL_SLINK_WAIT=<s>` — how long to let the app run once started, in WALL-CLOCK
  seconds. It was a pump count until SL-13 (`maxs * 10` iterations of
  `kl_ndk_pump_looper(100)`, which returns as soon as it has work), so a busy
  guest ended the run early — `KL_SLINK_WAIT=40` measured 9 seconds once the
  stream was live, and nothing in the log said so. It prints what it actually
  waited now.
- `KL_SLINK_LIB` / `KL_SLINK_FN` — which library and entry `nativeRunMain`
  drives (default `libmain.so` / `SDL_main`).
- `KL_SLINK_LIBDIR=<dir>` — wrapper only: which unpacked APK libdir to point
  the binary at (default `steamlink-android/lib/arm64-v8a`;
  `steamlink-vr/lib/arm64-v8a` is the `make slink-vr` target).
- `KL_GAP_ONLY=1` — map and relocate all seven libs, print what is
  unresolved, stop before init: the shim work list, in seconds.
- `KL_TIMEOUT=<s>` — wrapper only: the outer timeout around the run
  (default 180).

## Spikes (`spikes/`)

Standalone reproducers, not part of the runtime; each reads its own knobs.

- `S10_THREADS=N` — worker thread count (default 3).
- `S10_ITERS=N` — draw iterations per worker (default 4).
- `S10_STAGE=N` — stop after stage N (default 99, i.e. run everything).
- `S10_SHARE=0` — per-thread contexts share nothing (default 1, share with the
  root context).
- `S10_SERIAL=1` — hold a global lock across each thread's GL work (default 0).
- `S10_SWIZZLE=1` — run only the swizzled single-channel-texture stage, the
  guest's actual shape.
- `S10_REPLAY_ONLY=1` — run only the replay of the guest's texture sequence,
  skipping every other stage so a failure is attributable.
- `S10_EYE=1` — run only the eye-sized SRGB8_ALPHA8 FBO draw-and-blit stage.
- `S11_SIZE=WxH` — pbuffer size for s11_draw (default 1832x1920).

## Vulkan (`runtime/gfx/kl_vulkan.c`) — the synthetic libvulkan.so

BONELAB's graphics API. Needs `make mvk`; with no MoltenVK
vendored the whole path refuses by name and none of these do anything.

- `KL_VK_OUT=<dir>` — write each submitted frame there as a PNG. On this guest
  that is the OVRPlugin eye layer, one file per eye
  (`vk_f<frame>_s<stage>_eye<n>.png`), written at `ovrp_EndFrame4`; on a flat
  Vulkan guest it is the swapchain image at `vkQueuePresentKHR`. Unset means no
  capture and no readback cost.
  **It captures NOTHING for an OpenXR guest on Vulkan (Open Brush)** — that
  guest submits through `xrEndFrame` and never presents, so neither hook fires,
  and an empty output directory there is the wrong instrument rather than an
  empty frame. Use `KL_GLFB_OUT=<dir>`, which makes m_boot's end-of-run P5
  readback write `mtl_eye0.png` / `mtl_eye1.png` from the same storage.
- `KL_VK_OUT_ALL=1` — capture EVERY stage that has an image, not only the one
  the association named; the named one gets a `_named` filename suffix. This is
  the all-views-at-once debugger for the Vulkan stage association, which runs
  on the frame-counter fallback ("eye stage NOT observed") on every Vulkan
  guest: a capture of only the named stage cannot show a wrong guess, where a
  grid of `s0/s1/s2 × eye0/eye1` from one frame shows exactly which stage holds
  the fresh picture and which one was about to be sampled. Pair with
  `KL_VK_EYE_TINT=1` to also separate "never drawn" (tint survives) from
  "drawn but stale".
- `KL_VK_OUT_EVERY=N` — capture every Nth frame (default 1). A full eye pair is
  ~21 MB of readback, so 1 is only for short runs.
- `KL_VK_OUT_RUN=N` — with `KL_VK_OUT_EVERY`, capture N frames in a row each
  time (default 1), so two consecutive frames can be compared (motion between
  them). Also applies to `KL_XR_SPACEWARP_OUT`.
- `KL_VK_EYE_CLEAR=0` — leave a freshly allocated Vulkan eye image as Metal hands
  it over. Default on: every new eye image is cleared to transparent black first,
  because an uncleared one shows whatever its memory held (old frames, different
  per eye) until the guest draws into it — Medal of Honor's smaller eye while a
  level loads.
- `KL_VK_EYE_TINT=1` — pre-clear every eye image before the guest ever sees it:
  **GREEN for eye 0 and BLUE for eye 1**. Two A/Bs in one. It separates the two
  ways an eye texture comes back wrong, which are otherwise identical in the
  capture — if the tint survives, the guest never drew into the image; if it
  does not, the guest drew and the content is really what it rendered (this is
  what proved the uniform magenta was uninitialised memory rather than Unity's
  error shader). And under the Array layout, where the two eyes are two SLICES
  of one image, the per-layer colours say whether the capture reads the right
  slice — a capture reading layer 0 twice produces two byte-identical PNGs,
  which is exactly what a guest rendering only the left eye produces too.
- `KL_OVRP_MULTIVIEW` — Vulkan path only, **default 1** (kl_ovrp.c). Do we tell
  the guest the two eyes may be array LAYERS of one texture rendered in one
  pass, i.e. Unity's Single Pass Instanced? `0` restores MultiPass, which is the
  configuration in which BONELAB's SRP threw `ArgumentOutOfRangeException` once
  a frame and drew nothing — so this is the A/B for that whole failure. Always
  false for a GLES guest whatever this says: ANGLE-on-Metal has no multiview,
  and telling one yes is an eye texture it renders half of.
- `KL_VK_FRAME_SYNC` — **default 1** (kl_vulkan.c). Wait for the guest's VkQueue
  to go idle at `ovrp_EndFrame4` before publishing the frame serial a compositor
  reads. That is a real CPU stall on the guest's frame thread, and it is there
  because there is no cross-queue GPU fence on this path: the GL side signals an
  MTLSharedEvent from inside ANGLE's own command stream, and the guest owns the
  VkQueue here, so nothing of ours is in its submissions. `0` keeps the serial
  and drops the wait — the A/B for "is the compositor showing a torn frame?",
  and the shape a timeline-semaphore version would have.
- `KL_VK_TRACE=1` — the loader's own chatter: which MoltenVK was opened, which
  extension names were dropped, acquire/submit failures.
- `KL_VK_WIDTH` / `KL_VK_HEIGHT` — the synthetic Android surface's size
  (default 2064x2208). Only reachable by a guest that creates a WSI swapchain,
  which BONELAB does not.
- `KL_MVK_DIR=<dir>` — where to load MoltenVK from (default
  `vendor-moltenvk/out/macos`); `KL_MVK_DYLIB=<path>` names the file outright.
  Both are the A/B against a different MoltenVK build.

## Fixed foveation (`runtime/gfx/kl_fove.m`) — Vulkan guests, OFF by default

Confirmed on the headset with Half-Life: Alyx, then extended to the Unity and
Unreal eye shapes below. Design, measured facts and known gaps:
`docs/FOVEATION.md`. GLES guests foveate through `KL_VRR` instead.

Three eye shapes are recognised (all need `KL_FOVEATION` on; with it unset
nothing is installed, swizzled or handed out, and kl_vulkan's render-pass
records are never written):

1. **OpenVR double-wide** (Half-Life: Alyx): a `vkCmdBeginRendering` pass whose
   colour and depth are both the extent of the texture the guest Submits.
   Needs an OpenVR runtime that reports that extent; none is in this tree yet,
   so kl_fove carries the maths and the map but this shape is not recognised.
2. **The runtime's own eye images** (every Unity/Unreal Vulkan guest): a `vkCmdBeginRenderPass[2]` pass whose colour attachment 0 IS
   an eye image kl_ovrp (or kl_openxr) handed the guest, or whose resolve
   attachment 0 is (an eye-size MSAA target resolved into the eye in the pass),
   with a depth attachment of the same extent and sample count. A 2-layer
   ARRAY eye (multiview, viewMask 0x3) gets ONE rate map with two layers, layer
   l centred on eye l's projection centre (`kl_ovrp_eye_tangents`, which
   kl_openxr's xrLocateViews also answers from; inside the submitted
   ViewportRect when one is recorded) and is foveated only as a LAYERED Metal
   pass (renderTargetArrayLength 2 — MoltenVK's lowering of multiview on Apple
   GPUs); a one-slice pass over the pair draws unfoveated (counted). Every
   patched shader passes `[[render_target_array_index]]` to the decoder (an
   existing argument or stage_in member is reused, else one is added; Metal
   answers 0 when the vertex stage does not write it). An MSAA pass that
   resolves in-pass is expanded on the single-sample image it resolves into
   (MoltenVK opens it with storeAction Unknown + resolveTexture); the expand
   runs per slice. Measured on GORN 2 (host): 4x MSAA multiview colour+depth
   resolved into the kl_ovrp swapchain, one layered encoder per frame.
3. **The guest's own eye-size intermediate** (Batman: Arkham Shadow's render
   pipeline; UE's SceneColor -> tonemap is the same family). The scene pass's
   colour is NOT an eye image but is eye-sized (2 layers / viewMask 0x3 for a
   pair, depth of the same extent and samples). (a) **In-pass**: a later
   subpass of the SAME render pass writes (or resolves into) a runtime eye
   image (Batman: Arkham Shadow: `#0 depth x2, #1 colour x2 DONT_CARE-stored, #2
   R8G8B8A8_SRGB x1 STORE`, 3 subpasses, the post subpass reads #1 as an input
   attachment). Marked at once: every Metal encoder of the pass (scene
   subpasses and the eye subpass) gets the SAME 2-layer map; the scene stays in
   physical layout (it is transient — required: its storeOp is DONT_CARE, the
   eye attachment is not LOADed, all subpasses share one view mask), subpass
   inputs read it at the physical position (the MSAA-eye rule), and the EYE image is
   expanded after the pass. (b) **Copied / sampled later**: the pass's colour is
   a CANDIDATE, confirmed (sticky until vkDestroyImage) by a
   vkCmdCopyImage[2] / vkCmdBlitImage[2] / vkCmdResolveImage[2] into an eye
   image, or by the next render pass that draws into an eye image (sampled —
   a heuristic: that pass is assumed to read it). The confirming frame stays
   unfoveated; afterwards the intermediate is marked like an eye and expanded
   before the next render, compute **or blit** encoder (so its copy into the
   eye reads screen layout). A layered MSAA intermediate with no in-pass
   resolve stays unfoveated (counted, as for eyes).
   **Size slack (Deadpool):** the intermediate need not be exactly
   eye-size — equal or up to +16 px per axis, never smaller
   (`kl_fove_inter_extent_ok`), because Unreal rounds its scene target up
   (Deadpool: 5088x3236 scene for a 5088x3234 eye; the exact match left it
   silently unfoveated, `fove high 0%`). The map is sized to the intermediate
   (`[fove] map 5088x3236, 2 layer(s)`) and the expand runs on it (or on what it
   resolves into), so the eye being smaller changes nothing downstream.
   **Transient MSAA scene (Deadpool):** UE4's
   depth-read subpass pair — `#0 colour x2 CLEAR/DONT_CARE, #1 x1
   DONT_CARE/STORE, #2 depth x2`, subpass 0 draws the MSAA scene, subpass 1
   LOADs it (depth read as an input) and resolves #0 into #1. MoltenVK opens
   subpass 0 as a layered encoder that STORES the samples with no resolve,
   which the rule above refused (`layered MSAA … UNFOVEATED`), and subpass 1
   then LOADed with nothing pending (`a pass LOADs the eye …`) — both every
   frame, `fove 0%`. When kl_vulkan sees the confirmed intermediate's colour
   is storeOp DONT_CARE and a later subpass resolves it into a single-sample
   attachment (`kl_fove_note_ms_transient`), the layered no-resolve encoder is
   marked and its pending entry HELD (no expand; the samples die with the
   pass), the LOADing resolve encoder is marked with the same map, and the
   x1 resolve target is what gets expanded (the layered single-sample
   expand), before the pass that samples it. No per-sample expand is needed.
   Proof lines: `[vk] [fove] intermediate …: MSAA x2 scene with storeOp
   DONT_CARE, resolved by a later subpass into attachment #1 (x1) …`, `[fove]
   intermediate slot N: MSAA scene is TRANSIENT …`, `[fove] transient MSAA
   intermediate: first HELD … encoder` + `first RESOLVING … encoder`, `[fove]
   first expand: … x2 slices`, and in the 10 s line `transient MSAA held N /
   resolved N` with `eye LOADs left unfoveated 0`, `layered MSAA without
   resolve … 0`, `eye subpasses UNMAPPED 0`. A LOAD of held samples left
   unmapped is a `BROKEN FRAME` (counted as UNMAPPED). A layered MSAA store
   whose samples ARE kept (not DONT_CARE) still draws unfoveated: the
   per-sample expand remains one-slice only.
- **Re-centre on the frame record.** A layered (ARRAY) map built before the first frame record used the
   full image for each eye's straight-ahead point; Deadpool renders each eye
   into a ~4012-wide sub-rect of a 5088 texture, so the fovea sat 682 px off
   (a hard clear/blurry seam in both eyes). `kf_map_for` now recomputes an
   ARRAY map's centres on lookup and rebuilds it in place when one moves more
   than **4 px**: in place when no pending expand refers to the map, else
   into another map slot while the old one stays as it was for the expands
   still pending on it (a command buffer that already drew with a map keeps
   it for its remaining passes). Old maps are kept alive 2 s for frames in
   flight, then released. Until toolchain 565 a map an expand referred to was
   simply kept, and at most 8 re-centres were made a run; Alien: Rogue
   Incursion, on dynamic resolution (`KL_OVRP_ADAPTIVE_RES`) and with an
   expand always in flight, kept the 5th map while its viewport went from 79%
   to 55% of the image (fovea ~800 px off, measured on the Mac). Line:
   `[fove] map WxH RE-CENTRED (#n): it was built at x,y / x,y — the frame
   record moved the straight-ahead point (rebuilt in place … | built into
   slot n; slot m kept …)` (the first 8, then every 50th); with all 32
   retirement slots younger than 2 s, `… kept for now` once and the
   re-centre waits for a later lookup. `[fove] frame record (stage n) carries
   [no usable] a render viewport …` (first 6 changes) shows a guest whose
   records come with and without a rect.
   Full-image records (Into the Radius, Batman: Arkham Shadow) give the same
   centre as no record and never rebuild. No knob.
- `KL_FOVE_PER_EYE=0|1` — default **0**: every eye image and every map layer
  gets the ENVELOPE of both eyes' straight-ahead points placed in that eye's
  rect (per axis the larger of the two profiles), so both eyes' maps are the
  same relative to their pictures, symmetric about the middle, with each
  eye's own point at full rate. `1` = one peak per eye at its own point only
  (the shape through toolchain 565). On the Vision Pro's canted cones (60
  degrees out, 45 in) that point is 63% across the left eye and 37% across
  the right, so one peak left the outer side of each eye at the floor: in
  Alien: Rogue Incursion the left eye blurred over its left half and the
  right eye over its right half — the same complaint as Doom 3's through the
  display's own per-eye curves (f9f514f, `KL_VRR_PER_EYE`, GL guests). The
  envelope costs fragments: at `high` on Alien's shape 49% of the image
  instead of 39% (`make check`'s t_fove prints both). Lines: `[fove] map WxH
  layer l: the envelope of both eyes' points … full-rate middle x …`, or
  `one peak per eye (KL_FOVE_PER_EYE=1)`. `KL_VK_FOVEATE` (kl_mtl_foveate.m)
  honours the same switch for its two-layer map.

- `KL_FOVEATION=off|low|medium|high` — default **off**. Profiles (box / floor,
  ramp 0.19 H): low 0.603 H / 0.333, medium 0.508 H / 0.22, high 0.381 H /
  0.125. No rate-map support (visionOS Simulator) = off with one line; no
  2-layer support = 2-layer eye pairs stay unfoveated (one line). Logs `[fove]`.
- **Lines that prove engagement on a Unity/OVRPlugin guest** (one run):
  `[vk] [fove] EYE PASS (marked) render pass …: MSAA colour resolved into a
  runtime eye image WxH x4 (2 layer(s), … viewMask 0x3) …`, `[fove] eye target
  registered: … 2-layer eye pair`, `[fove] map WxH, 2 layer(s) (one per eye) …
  layer 0 = eye 0 centre …` + `layer 1 = eye 1 centre …` (centres NOT 50%/50%
  on the headset — asymmetric frusta; each centre names the
  render viewport it came from, `…; viewport 0,0 WxH of WxH[, another
  generation: scaled as a fraction]` or `…; no frame record: full image`),
  `[fove] marked encoder #1: … + resolve
  … array length 2`, `[fove] first expand: … x2 slices`, and every 10 s
  `[fove] libraries …; encoders marked N (layered N) …, eye LOADs left
  unfoveated 0, one-slice passes … 0, layered MSAA without resolve … 0;
  expands N (… of a resolve target N marks …); refused 0 …`. A near miss is
  named once with its full attachment table: `[vk] [fove] near miss, NOT
  marked: … | why: …` (+ `[vk] [fove] near miss images: …`), and a transfer
  into an eye image (the guest drew elsewhere) as `[vk] [fove] vkCmdBlitImage
  INTO a runtime eye image …`.
- **Map centre and the viewport generation.** The
  per-eye centre is the frustum's straight-ahead fraction inside the guest's
  render viewport, and that rect is now read as a FRACTION of the texture it
  was measured against (`kl_ovrp_render_pose.viewport_of`), applied to the
  image the map is built for. Read as pixels instead, a guest that re-creates
  its eye images after the first frame (Into the Radius 5088x4081 -> 6784x4528;
  Unity's ~1.2x resize and again on map entry) would get a map built from the
  previous generation's rect — the fovea 848 px left / 204 px up, a blur band
  on the right eye's outer edge. viewport_of 0 or a rect that does not fit it =
  the full image. To check a guest that resizes its eye images: compare its
  `[fove] map … centre` lines against W x fx, H x ~0.456 (the line's own
  percentages), and the `of WxH` it names against the map's WxH.
  - **In an installed game this is a setting**: the start window shows the
    level (Off / Light / Medium / Strong) in place of the foveation switch for
    a game known to draw with Vulkan, and hands it over as `KL_FOVEATION`
    (`Strong` also sets `KL_FOVE_BOX=0.30 KL_FOVE_FLOOR=0.08`). Where it starts
    is the target's `vk_foveation` in `visionos/targets.py` — `medium` for
    Marvel's Deadpool VR, off for everything else. A value on the launch line
    wins.
- `KL_FOVE_MSAA=0` — leave MSAA eyes UNFOVEATED (one `[vk] [fove] … MSAA`
  line / near-miss reason). Default 1. MSAA paths: the depth-only
  clear of a tiler-0 MSAA eye is mapped and leaves a depth_only pending entry;
  2d_ms reads of an eye image at `gl_FragCoord` keep the PHYSICAL position;
  an MSAA eye stored with no resolve is expanded per sample (resolve the
  physical region, blit each fovea rectangle with all samples, periphery +
  per-sample fovea back) — one slice only: a LAYERED MSAA pass that stores
  its samples with no resolve draws unfoveated (counted). `KL_FOVE_MSAA_FOVEA=0`
  (debug) — the per-sample fovea copy off.
- `KL_FOVE_BOX=<fraction of H>`, `KL_FOVE_RAMP=<fraction of H>`,
  `KL_FOVE_FLOOR=<rate 0.02..1>` — override the level's numbers (box 0.30 /
  floor 0.08 was found invisible on the headset in Half-Life: Alyx).
- `KL_FOVE_BUFFER=<1..30>` — the fragment buffer slot for the rate-map
  parameters (default 20). A `[[position]]` shader already using it is
  refused and foveation turns itself off; a clobber inside a foveated pass
  does the same.
- `KL_FOVE_FRAGCOORD=0|grid|gridraw` (debug) — unpatched shaders / a 64-px
  position grid with or without the remap. `KL_FOVE_EXPAND=0` (debug) — leave
  the eye in physical layout. `KL_FOVE_MSL_DUMP=<absolute dir>` (debug) —
  every MSL source, as patched. `KL_FOVE_TRACE_ENC=<n>` (debug) —
  the first n Metal render-pass descriptors after an eye is registered
  (textures, types, sample counts, slices, load/store, resolve, array length).
- `KL_FOVE_SUBPASS_HELPERS=0|1` — default **1** whenever `KL_FOVEATION` is
  on. A subpass-input read (`tex.read(uint2(gl_FragCoord.xy), …)`, MoltenVK's
  emulation) must see the PHYSICAL position, because the attachment is in
  physical layout while the mapped pass runs. Unreal 4's mobile depth fetch makes
  that read inside a helper above the entry point
  (`VulkanSubpassDepthFetch(…, thread float4& gl_FragCoord, …)`, used by DepthFade
  and soft-edge translucency), which the entry-body scan never saw: the helper
  got the remapped screen position and read the scene depth at the wrong texel.
  In Moss that turned the soft shadow in the book's fold into a hard black band,
  with foveation on only. With `1`, a helper whose only uses of the position are
  `<tex>.read(uint2(<pos>.xy)…` from one of its own texture parameters is passed
  the saved physical position (`kl_fove_phys`) at its call sites; a helper that
  uses the position for anything else keeps the remapped one. `t_fove` covers
  both settings (`kl_fove_set_subpass_helpers_for_test`). `0` = the old behaviour (A/B control). Proof line: `[fove]
  shader …: N subpass-input read(s) kept at the PHYSICAL position (M through a
  helper's call)`, and in the 10 s line `fragment entry points patched N (K with
  subpass reads, …)` with K > 0 for Moss (before the fix: 0). **For a test
  copy** (no launch line on the headset): it is a tester-only Test switch
  ("Foveation (Vulkan): old depth reads in helper functions"), listed and
  applied only in a test build (`KLTestBuild`); turn it on from the install with
  `KLEPTON_TEST_ON_EXTRA=KL_FOVE_SUBPASS_HELPERS` (see "Test switches that are
  on from the installation" below).
- `KL_FOVE_FENCE=0|1` — default **1** (confirmed on the headset with
  Half-Life: Alyx). MoltenVK orders its passes with its own
  MTLFences, so without this the guest's next reader of the eye can run
  concurrently with the expand and see the raw physical (compressed) layout
  (Half-Life: Alyx without it: shrunken copies of the scene in the top-left of
  the left eye). Every expand path's last encoder updates a fence (`kl-fove-expand`;
  the layered single-sample expand chains slice 0 -> slice 1 through a second
  fence) and every render/compute/blit encoder opened afterwards on the same
  queue waits on it (same command buffer at once; a later one once the
  expand's buffer is committed). `0` = unordered (control). Proof lines:
  `[fove] ON … expand ON (fenced: the guest's later passes wait for it)`,
  `[fove] expand fence: the guest's encoders after an expand wait for it (first
  wait: …)`, and every 10 s `[fove] expand fence: encoders
  after an expand: waited on its own buffer N, on a later buffer N; NOT waited:
  other buffer while the expand's was uncommitted N, other queue N; expands
  recorded at an encoder N, at commit N`. "other queue" = an encoder on a
  different MTLCommandQueue (no fence can order it — Metal fences are
  per-queue; the swizzle is class-wide, so the runtime's own Metal work, e.g.
  the compositor, is counted there too); non-zero "uncommitted" means the
  guest encodes the reader into another command buffer before committing the
  expand's — read it before anything else. The first expand also logs the eye
  image's hazard tracking mode.
- `KL_FOVE_INTERMEDIATE=0|1` — default **1** whenever `KL_FOVEATION` is on.
  `0` = shape 3 above is never marked (the near-miss line of shape 2 comes
  back). Proof lines (Batman: Arkham Shadow, in-pass): `[vk] [fove] intermediate CONFIRMED:
  VkImage … -> eye image … via subpass (attachment #2 of render pass …)`,
  `[vk] [fove] EYE PASS (marked) render pass …: eye-size intermediate, a later
  subpass writes a runtime eye image …`, `[fove] intermediate target
  registered: … via subpass …`, `[fove] intermediate slot N: eye image … is
  written in-pass from it` (one per swapchain stage), `[fove] intermediate:
  first marked scene encoder` + `first marked EYE-SUBPASS encoder`, `[fove]
  first expand: … x2 slices`, and in the 10 s line `intermediate marked N (eye
  subpasses N, eye subpasses UNMAPPED 0, expands before a blit N)`. Copy /
  sampled shape: `[vk] [fove] intermediate CONFIRMED: … via copy|blit|resolve
  (vkCmd…)` or `via sampled (…)` and `[fove] intermediate: expanded before a
  blit encoder`. **`[fove] BROKEN FRAME: the eye subpass of a foveated
  intermediate was left UNMAPPED`** (UNMAPPED > 0) = the picture is wrong
  that frame — set `KL_FOVE_INTERMEDIATE=0` and bring the line back. The
  transfer census line (`vkCmd… INTO a runtime eye image …`) now also names
  the first region's layers/offsets/extent and whether the source is a
  candidate/confirmed intermediate; the copy_commands2 spellings are hooked
  too.
- `KL_FOVE_PROBE=1` (debug) — the first 4 transfers of each kind into an eye
  image (not just the first), and the first 4 depth-less passes drawing into
  an eye image with the last candidate intermediate. It also swizzles MoltenVK's
  render/compute encoder classes and prints every fence wait/update,
  useResource, memory barrier, texture bind and dispatch on the 2 encoders
  after each of the first 4 expands (`(THE EYE)` marks the eye), plus 10 s
  totals. `KL_FOVE_SLOW=<n>` (debug stress) — n busy
  iterations per fragment of the MSAA periphery expand, to widen the race
  window (no effect on single-sample expands).
- `KL_FOVE_AREA_SNAP=0|1` — default **1**. An eye pass whose render area
  falls a few px (< 8) short of its framebuffer is widened to the whole
  framebuffer: MoltenVK clears an attachment with a Metal clear only when the
  render area IS the framebuffer extent, and otherwise opens it with LOAD, and
  a pass that LOADs an eye with nothing foveated pending cannot be mapped.
  Unreal rounds its scene buffer up (an area of 4608x2935 on a 4608x2936 eye);
  without this every eye pass LOADed and nothing was foveated. The extra
  rows/columns lie outside every viewport the guest submits. Line: `[vk] [fove]
  area snap #1: eye pass area WxH widened to its framebuffer WxH …`.

## MoltenVK vendoring gate (`tests/t_mvk.c`, `make mvk-check`)

The Vulkan side of BONELAB. Not part of the runtime yet —
these are the gate's own knobs.

- `KL_MVK_DYLIB=<path>` — which MoltenVK to load (default
  `vendor-moltenvk/out/macos/libMoltenVK.dylib`, i.e. what `make mvk` stages).
  The A/B against another MoltenVK build.
- `KL_MVK_VERBOSE=1` — restore MoltenVK's own info-level logging, which the gate
  otherwise silences (it sets `MVK_CONFIG_LOG_LEVEL=1` before the dlopen,
  because the device census buries the gate's own output). Worth setting once on
  a new machine: it is the fastest answer to which GPU and which Metal feature
  set are present.

## visionOS app (`visionos/Sources/*.swift`, `visionos/Sources/kl_app.c`)

Read on the device/simulator; `visionos/run.sh` forwards every `KL_*` it sees
except its own control vars (next section).

- `KL_SHADER_CACHE_GUARD=0` — keep a guest's compiled-program caches across a
  driver change. By default `kl_app.c` stamps `kern.osversion`, `hw.model` and
  `KL_GLFB_REWRITE_GEN` into `<files>/.klepton-gl-driver` and, when the stamp
  moves (an OS update, a rewrite-rule change), deletes every
  `ProgramBinaryCache` / `UnityShaderCache` directory beneath it, once
  (upstream of this fork also `ShaderCache`, `il2cpp_cache` and `GPUCache`;
  left out here until a guest is seen keeping GL program binaries under those
  names). A stale cache reloads into programs
  that never link, which UE4 titles turn into a crash on the first level load.

- `KL_TARGET=<name>` — which guest the app boots: `beatsaber`, `superhot` or
  `steamlink-vr` (`visionos/targets.py --list`). The **default is compiled in**
  (`KL_TARGET_DEFAULT`, set by `gen_xcodeproj.py` from `KLEPTON_TARGET`), not
  read from the environment, because an app launched by hand from the Home View
  has no environment at all. This overrides it, which is how the two guests can
  be A/B'd from one build without regenerating the project — but note the two
  apps embed **different guest frameworks**, so pointing one at the other's
  target fails at `kl_app_configure` with "missing guest libraries".
- `KL_IMMERSIVE=0` — the immersive space is the default **for Beat Saber**;
  `=0` restores P4's window-and-report shape, which has to stay takeable because
  it is the measurement that localises a device regression. **For
  `steamlink-vr` the default is the other way round** — that guest cannot draw
  without an authorized session, so the default launch would open a space that
  is black by construction and hide the one surface with information on it.
  `KL_IMMERSIVE=1` turns it on for a run that HAS a session. Note the window
  path never opens a `LayerRenderer.Drawable`, so nothing measures the display:
  the guest gets Quest 2 defaults and a **zero IPD**, and `KL_OVRP_IPD=0.063` is
  required there for the reason SL-12 records ("The eye
  geometry has ONE source").
- `KL_AUTOBOOT=0` — autoboot is the default too; `=0` restores the
  Boot-button-only shape, for attaching a debugger or starting a GPU capture
  before the guest runs.
- `KL_CUSTOM_LAUNCHER=1` — **build-time** flag (set when generating the Xcode
  project, e.g. `KL_CUSTOM_LAUNCHER=1 ./build_run_vpro.sh hl1`), NOT a runtime env
  var. It bakes the "configure, then Start" launcher into the game builds
  (hl1/hl2/portal): a window title of the game's name, a folder picker for the game
  files (on-device or iCloud), hl1's commandline.txt + VR/controls/perf panel, and a
  "Boot" button in place of autoboot — so a shipping build ships with it and no
  runtime env is needed. **Off by default** — the game builds otherwise behave the
  original way (autoboot honouring `KL_AUTOBOOT`, a plain "Boot" button, no picker),
  so existing scripts (`visionos/run.sh`) are unchanged. Mechanism:
  `visionos/gen_xcodeproj.py` adds the `KL_CUSTOM_LAUNCHER` Swift active-compilation
  condition, which `klLauncherTitle()` in KleptonLauncher.swift gates on.
- `KL_EXIT_ON_BACKGROUND=0` — stay alive when the app is backgrounded. The
  **default is to `exit(0)`**, ALVR's shape: everything the guest holds across a
  suspend (the ARKit session, the Compositor Services layer, ANGLE's context and
  the eye swapchain, FMOD's player, a Unity engine that does not expect the
  display to leave) is either unresumable or expensive to re-establish, so every
  launch is made a first launch. Set `=0` when the headset has to come off with a
  capture or a debugger still attached. Either way the phase change is logged
  (`[app] scene phase -> …`).
- `KL_UNITY_QUIT_EXITS=0|1` — (`visionos/Sources/kl_app.c`) a Unity guest's
  `nativeRender` returning false ends its frame loop and the app leaves, the
  way Android finishes the activity when a game calls `Application.Quit()`.
  Without it the loop goes on calling an engine that has stopped drawing, and
  a game quit from its own menu leaves its last picture standing until the app
  is closed by hand. Default 1 for `x2582932495064035` (Real VR Fishing) and
  `x2436558143118760` (Synth Riders) and
  `x2285803881494677` (Gun Club VR, with `KL_UNITY_QUIT_TEARDOWN`), else 0: no
  other game has been watched answering false. Not on the `KL_SYNC_GUEST` path.
- `KL_UNITY_QUIT_TEARDOWN=0|1` — (`runtime/guest/kl_driver.c`) when
  `nativeRender` first answers false, end Unity the way Android does before
  the loop stops: Gun Club VR's own public static `TBMPrefs.Save()` through
  IL2CPP (that title only; nothing is set, the game writes what the player
  chose), then `nativePause` and `nativeDone`, on the guest frame thread.
  `nativeRender` is never called again after it. Default 1 for
  `x2285803881494677` (Gun Club VR build 410: its in-game Exit kept its settings
  across a restart only with the save in front, as its fix was sent in), else
  0. Persistence when visionOS force-quits the app is not covered.
- `KL_SYNC_GUEST=1` — drive the guest inline on the compositor thread, i.e.
  P5b's shape before the guest moved to its own thread. The clock P5.4's device numbers were taken
  against, and the A/B for anything that looks like a pacing regression.
  Default is the guest on its own thread, one frame per published pose.
- `KL_FULL=1` — `.full` immersion. `.mixed` is the default — the guest's world
  is opaque so passthrough shows through nowhere it matters, and seeing the
  room makes scale and IPD easier to judge. The scene manifest declares
  `UIImmersionStyleMixed` to match; keep the two in step.
- `KL_OVERLAY_SUBMIT_ORDER=0|1` — composite the guest's quad layers in the
  order it submitted them (no depth test between them, depth still written),
  as a Quest's compositor does. Without it a quad's whole rectangle, transparent
  parts included, hides whatever farther quad is submitted after it — and so
  does the eye picture, which is drawn as a quad at `KL_REPROJECT_DEPTH`: a
  panel the guest places beyond that distance fails the depth test against it
  and is never seen. Default 1 for `x24882487241367228` (Alien: Rogue
  Incursion) and `x5812519008825194` (AC Nexus, whose calibration card sits
  2.3 m out), else 0.
- `KL_GUEST_OVERLAYS=0` — stop compositing the GUEST's own non-eye layers.
  **Not the same thing as `KL_OVERLAYS` below, which is a system setting**; the
  two are one word apart and were briefly one name, which would have made each
  one's A/B silently move the other. This one is the guest's `ovrp_EndFrame4`
  submit list: an Unreal title draws its splash, its loading screens and its
  menus as `ovrpShape_Quad` stereo layers, so with this off RE4 shows the world
  and nothing else. Every guest before it submitted only a 1x1 dummy, so it
  costs them nothing either way. Both compositors read it. Pair it with
  `KL_OVRP_LAYERS=1`, which says what the guest actually submitted.
  It covers an OpenXR guest's `XR_TYPE_COMPOSITION_LAYER_QUAD` too — the same
  records, filed from `xrEndFrame` instead — and for a guest that presents its
  whole frame as a quad and submits no projection layer at all (JKXR does, every
  frame) this is not a decoration on top of the picture, it IS the picture: with
  it off the display is black and the run is otherwise identical.
- `KL_OVERLAYS=1` — put the system's persistent overlays back. Both immersive
  spaces pass `.persistentSystemOverlays(.hidden)` by default: the Home
  indicator and the hand-gesture affordance beneath it are drawn by the system
  *over* the guest, and both guests put interactive content exactly where it
  lands — Beat Saber's lower menu row, Steam Link's dashboard toolbar. It also
  reappears on every hand raise, which for a hand-driven title is continuous.
  `.hidden` is a request rather than a guarantee (the system still shows the
  indicator when it considers it mandatory), so turn this on when the question
  is whether the system still thinks our space is on screen at all.
- `KL_HAND_ROT="x,y,z"` — extra rotation in degrees, applied in the **grip's
  own frame** to every hand pose after the wrist→grip correction — for the
  part that is not a basis error (Beat Saber's `AdjustControllerTransform`
  offset is game data we cannot read). One device run per candidate angle, no
  rebuild. `KL_HAND_ROT_L`/`_R` override per hand, which is what a
  *half*-wrong basis needs.
- `KL_HAND_POS="x,y,z"` — the same, in metres — slides the hilt along the
  grip's own axes. `KL_HAND_POS_L`/`_R` likewise.
  **Both `KL_HAND_*` knobs are hand-tracking only**; they used to apply to the
  Sense controllers too, which silently pushed every Sense pose 4 cm down its
  own hilt for a reason that belongs to the metacarpal midpoint and not to a
  pose the platform publishes. The Sense path has its own pair:
- `KL_SENSE_ROT="x,y,z"` / `KL_SENSE_POS="x,y,z"` — degrees and metres, in the
  grip's frame, applied to `AccessoryAnchor`'s `.grip` pose. Replaces the
  default outright. `_L`/`_R` per hand.
**These values can be tuned LIVE.** The boot window carries a "Controller
alignment" panel (`visionos/Sources/KleptonTuning.swift`) with a slider per
term, and it stays open beside the immersive space — so the guest keeps
rendering while they move and a candidate costs a drag instead of a two-minute
relaunch. The panel seeds itself from whatever is actually in force, so the
variables below still set the starting point; it prints the whole set back as a
copy-pasteable environment line, because a value found by dragging and not
written down is a value found twice.

- `KL_SENSE_MIRROR=0` — stop mirroring the shared offsets' X term for the LEFT
  hand. A grip offset is a point on a hand and the two hands are mirror images,
  so the same physical displacement is `+x` on one and `-x` on the other;
  without the mirror a shared value is right on one hand and wrong by twice its
  own size on the other, which is the chiral error reported on BONELAB. Applies
  only to the shared `KL_SENSE_POS` / `KL_SENSE_PIVOT` — an explicit `_L`/`_R`
  is already an answer about that hand and is taken exactly as given.
- `KL_SENSE_PIVOT="x,y,z"` — metres in the grip's frame: **the point
  `KL_SENSE_ROT` turns ABOUT.** `_L`/`_R` per hand, default 0,0,0 (turn about
  the reported origin, which is what this path has always done).

  **A translation cannot remove an arc.** The rotation is applied about the
  origin the platform reports — back at the wrist — so everything the guest
  draws from the pose swings by the rotation angle about that point;
  `KL_SENSE_POS` slides the result but cannot change what it rotated around.
  Measured on device: with `KL_SENSE_POS="0,0,0"` BONELAB's swing is unchanged.

  Same lever, same name and units, as `KL_XR_GRIP_PIVOT` on the OpenXR path, and
  estimable the same way — it is a point you can look at your hand and guess
  ("the hilt centre is about six centimetres forward and four down from where
  the controller is tracked" -> `0,-0.04,-0.06`).
- `KL_SENSE_PITCH=<deg>` — just the X term of that rotation, which is the one
  that has ever needed changing. Default **-37**: the magnitude is Beat Saber's
  own in-game controller adjustment, the sign is device-measured (the same
  magnitude positive pitches the hilts backward — the game applies its
  adjustment in Unity's left-handed frame). It stacks with ALVR's +5.037° PSVR2
  model tilt, for **-31.963°** about X. If a playtest leaves about five degrees
  forward, try `KL_SENSE_PITCH=-45.037` (= -40.0 total, Beat Saber's own
  Oculus Touch constant).
- `KL_SENSE_VEL_FRAME=world` — read `AccessoryAnchor.velocity`/`angularVelocity`
  as already being in tracking space. Default treats them as accessory-local
  and rotates them by the grip orientation, which is what ALVR does; Apple
  documents neither, and the two differ only in direction.
- `KL_HAND_MIRROR=1` — left hand back on ALVR's *mirrored* wrist→grip
  constant. The default left constant is `R · Rx(180)`, forced by four
  playtests.
- `KL_HAND_ANATOMICAL=1` — build the left grip frame from joint **positions**
  (index/little knuckle + wrist) rather than any wrist-frame constant —
  OpenXR's own basis, consistent between hands by construction. Off by default
  only because it has never been confirmed on device.
- `KL_PINCH_OPEN=<m>` / `KL_PINCH_CLOSED=<m>` — two-point calibration of the
  fingertip-distance trigger: at or beyond `OPEN` (0.06) is 0.0, at or within
  `CLOSED` (0.03) is 1.0. `CLOSED` is a **floor**, not a threshold — erring
  wide costs sensitivity, erring narrow costs the click.
- `KL_PINCH_SRC=system|distance` — which source may press the trigger.
  Default is **both, OR'd**: the system's `.indirectPinch` and the fingertip
  distance — each has been unreliable alone and their failures are
  uncorrelated. Use to isolate one when the question is which is misbehaving.
- `KL_PINCH_DISCRETE=0|1` — (`visionos/Sources/KleptonPinch.swift`) a hand's
  pinch as a trigger that is 0 or 1 and nothing between, the button with it.
  Default 1 for BattleGroupVR2 (`x9386827918099701`), whose trigger action is
  a boolean on the trigger's value: the partial closure every other game is
  handed while the fingers approach is a click there. Else 0, and hand input
  is what it was. On: a system pinch, direct or indirect, presses at once and
  releases when its gesture ends, whatever the fingertips measure; each
  gesture presses once, followed by its id, so the event that ends it needs
  no hand of its own and an update that arrives after the hand was lost, or
  more than 200 ms late, presses nothing. The fingertip distance presses
  after 40 ms closed and can press again after 50 ms open; up to 80 ms of
  missing fingertips on a tracked hand are bridged without counting towards
  either. A lost hand, a distance that is not a number and a stalled frame
  loop release, and need the hand open again before the distance presses.
  `KL_PINCH_SRC` chooses the source as before. Controllers are not touched.
- `KL_PINCH_TRACE=0` — silence the pinch trace, which is **on by default**
  during bring-up: one line every 2 s per hand — closest pinch reached, axis
  value produced, frames asserted, or `no skeleton`. Those four distinguish
  the four ways "the trigger does not click" can be true. With
  `KL_PINCH_DISCRETE`, a second line counts the frames without the hand,
  without a skeleton and without both fingertips, and each system pinch's
  start and release is named with its hand and id.
- `KL_HAPTICS_OFF=1` — do not play the guest's haptics. The pull still runs
  (nothing backs up either way — the queue retires on the clock), so this is
  purely "is the buzzing the guest's, or ours". Playback is CoreHaptics on a
  `GCDeviceHaptics` engine per hand; **hands cannot be vibrated**, so a
  hand-tracked run drains the queue and drops it.
- `KL_HAPTICS_GAIN=<0..1>` — scales every pulse's intensity. Default **0.25**,
  which is ALVR's own PSVR2 scale (it uses 0.7 for everything else): the Sense
  actuators are strong enough that a guest amplitude of 1.0 played at full
  intensity is not what the guest meant by it.
- `KL_HAPTICS_SHARPNESS=<0..1>` — CoreHaptics's second axis, which OVRPlugin
  has no equivalent of (kl_ovrp.h says why frequency is not in the seam).
  Default 1.0, ALVR's constant: crisp, which is what a note cut is.
- `KL_HAPTICS_PULSE=1` — drive with a discrete `.hapticContinuous` event per
  frame (ALVR's shape, floored at `KL_HAPTICS_MIN_MS`) instead of the default:
  **one long-lived looping player per hand whose intensity follows the guest's
  envelope**. ALVR's source is discrete server events; ours is a continuous
  320 Hz stream, and playing that as a run of short events is a restart per
  frame — which is what a note cut felt like on device before the envelope
  follower. A hand
  also falls back to this on its own if the continuous player cannot be made
  or started, and says so in the log.
- `KL_CP_PROBE=<n>` — bisection ladder for a dark compositor: 1 = clear only
  (colour cycles R/G/B so a constant field cannot be misread), 2 = flat
  magenta quad (geometry only), 3 = sampled with alpha forced to 1, 4 =
  full-viewport blit. `KL_CP_NOFENCE=1` skips the guest fence wait; the
  `alive:` line reports `cmdbuf done/committed` so "committed but never
  executed" is visible.
- `KL_CP_ANCHOR_HOLD=0` — when the tracker will not answer for the instant the
  composite is drawing at, fall back to the IDENTITY device pose instead of the
  last one it did answer with. The default (hold) exists because the identity is
  not a weaker correction but a false assertion — that the head is at the world
  origin, unrotated — and its symptom is the guest's picture unsticking from the
  world and sitting fixed in front of the face for as long as the refusal lasts.
  `queryDeviceAnchor` refuses instants it will not predict to, so a guest hitch
  that pushes the presentation time out is enough to reach it. The `cadence`
  line counts it whenever it happens: `anchor N walked back / N held / N
  unknown` — walked back is the timestamp being stepped into range, held is the
  last measured pose standing in, unknown is having never tracked at all.
- `KL_CP_ANCHOR_TRACKED=0|1` — take a device anchor whose `isTracked` is false
  for no answer at all: the guest keeps its last head pose and the composite
  holds the last measured one (`KL_CP_ANCHOR_HOLD`) instead of both following
  whatever transform the anchor carries while the headset is finding its place
  again. Counted on the `cadence` line like any other refusal. Default 1 for
  `x2582932495064035` (Real VR Fishing, as its fix was sent in), else 0 — what
  other games show through a tracking loss has not been compared.
- `KL_CP_EYE=<0|1>` — composite ONLY that eye, leaving the other black. The
  binocular-vs-temporal split for a doubled image: one second, halves the
  search space.
- `KL_CP_AMPLIFY=0` — one render pass per eye instead of the single
  vertex-amplified composite. Takes foveation down with it (the rate map
  needs the amplified pass).
- `KL_CP_FORMAT=16` — drawable back to `rgba16Float`; the default is 8-bit
  `bgra8Unorm_srgb` (ALVR's choice — half the composite bandwidth, hardware
  sRGB encode on write). The guest's eye textures stay RGBA16F either way.
- `KL_VRR_PER_EYE=0|1` — (headset compositor) 0 gives both layers of a
  mirrored guest's rate map the envelope of the two eyes' curves; 1 gives each
  layer its own eye's curve (the display's, made an exact mirror pair), which
  stores about what the display keeps — at 6890x5526, 2784x2720 an eye where
  the envelope stored 4509x2720. Not set, the guest decides
  (`kl_glfb_rate_map_per_eye`): a curve per eye for Beat Saber from 1.45 on,
  the envelope for every other guest — in Doom 3 one eye coarser than the other
  at the same spot was seen and disliked, at every size tried.
- `KL_VRR_MIRROR_PHYS=0` — (`runtime/gfx/kl_glfb.c`) copy a foveated mirrored
  eye as the whole texture again. Default 1: only the map's physical part,
  which is all the guest wrote.
- `KL_CP_GAZE_TRACE=1` — four times a second, for the first two minutes, where
  each view's rate map on the drawable is densest (the centre of its full-rate
  plateau, as a fraction of the screen). The measurement of whether the
  display's foveation follows the eyes. On a Vision Pro M5 with visionOS 27.2
  it does not: 166 samples in 37 seconds while the wearer looked left, right,
  up and down were all x=0.635/0.365 (the two eyes, mirrored), y=0.468.
- `KL_CP_FOVEATION=0` — no variable-rasterization-rate map and no
  `maxRenderQuality` ceiling (the quality knob is only accepted alongside
  foveation).
- `KL_CP_QUALITY=<0..1>` — the render quality to request (default 1.0). The
  ceiling costs memory and per-frame GPU time; the *running* quality is set
  separately by the render loop.

## Wrapper-script control vars (`visionos/run.sh`, `build_run_vpro.sh`)

Read by the scripts themselves, never forwarded to the app.

- `KLEPTON_TARGET=<name>` — which guest to BUILD, read by `visionos/run.sh`,
  `mkguest.sh`, `stage_assets.sh` and `gen_xcodeproj.py` from the one table in
  `visionos/targets.py`. It decides the guest libraries, the APK and asset tree
  that get staged, the bundle id, the display name and the **product name** —
  and with that the `.xcodeproj`, the `.app`, the derived-data directory and the
  `Frameworks/<target>/` the translations are staged into, so two apps built
  from this tree never write to the same place. Default `beatsaber`.
  `./build_run_vpro.sh <target>` sets it from its first bare argument, which is
  the shortest way to say which guest a device run is for — and the same table
  then decides which app's log that run pulls back, so the two cannot disagree.
  The HOST driver takes the same names: `./build/m_boot <target>` (or a path to
  a guest lib directory, which is what every Makefile gate passes), and
  `make check TARGET=<name>` points the host gates at another title's tree.
- `KLEPTON_BUNDLE_SCOPE=<name>` — the front of the bundle id, which is
  `<scope>.dev.klepton.target.<target>` and defaults the scope to **`$USER`**.
  It is derived rather than stored, so a new target needs no id invented for it,
  and it is scoped per user because an App ID can be registered to exactly one
  team: without that, whoever builds this tree first takes the id, and everyone
  else gets `Failed Registering Bundle Identifier` plus the loss of the two
  memory entitlements, which need an explicit App ID. Sanitised to the
  characters a bundle id may hold; an empty or unset scope leaves the id
  unscoped rather than emitting a leading dot.
- `KLEPTON_BUNDLE_ID=...` — override the whole id, which is also how an
  already-installed app's identity is KEPT. Changing the id makes a new app with
  an empty container: assets, OBB and the guest's saves stay in the old one. The
  staging stamp is keyed on the id, so the next build re-stages by itself rather
  than launching into an empty container — it is just not free.
- `KLEPTON_DISPLAY_OVERRIDE=<name>` — the whole name under the icon
  (CFBundleDisplayName), for every target. It takes precedence over the row's
  `display`, over `KLEPTON_GENERIC_DISPLAY` and over the custom launcher's
  compact name. `KLEPTON_DISPLAY_NAME` is the older spelling and still works;
  `KLEPTON_DISPLAY_OVERRIDE` wins if both are set.
- `KLEPTON_DISPLAY_SUFFIX=<text>` — appended to the name after the override is
  applied, e.g. `" (Test)"` gives `Beat Saber (Test)`. Use it with
  `KLEPTON_BUNDLE_ID` to install a test copy beside the one already on the headset
  without the two icons looking the same. Unset or empty, neither variable
  changes anything; non-printable characters are dropped.
- `KLEPTON_TEAM=<id>` — override the signing team;
  `KLEPTON_DEVICE=<udid>` — skip device auto-detection;
  `KLEPTON_ENTITLEMENTS=0` — build without the two memory capabilities (see
  `gen_xcodeproj.py` for why that is the unusual build).
- `KL_SKIP_STAGE=1` — never stage assets, even on a target that has never had
  them; `KL_STAGE=1` — always stage, whatever the stamp says. The 2.2 GB
  upload is a ~20 s loop vs a ~20 min one; a reinstall rotates the data
  container, which is what the stamp guards against.
  **Neither knob covers the metadata beside `assets/`** — `apktool.yml` and
  `AndroidManifest.xml`, 12 KB, which the guest reads through `<assets>/../`.
  Those go across on every run (`stage_assets.sh --meta`, called by `run.sh`
  outside the stamp), because gating them with the assets is how a device ran
  for an hour with the right 1.3 GB OBB and a guest asking for 1.28's
  versionCode: the fix was in the build, the file was on the device, and
  nothing re-staged.
- `KL_OBB_DIR=<dir>` — where `stage_assets.sh` finds the guest's OBB, default
  `~/Library/Application Support/Klepton/userdata/<guest>/obb` — the same
  directory a host run reads it from, since it is guest userdata and not part
  of the repo. A **split application binary** guest keeps its data there rather
  than in the APK (Beat Saber 1.40: a 53 MB APK beside a 1.3 GB
  `main.1716.com.beatgames.beatsaber.obb`), and the guest finds it through
  `getObbDirs()` -> `<files>/obb`, which on device is
  `<container>/android-files/obb`. Present-or-absent: 1.28 and Steam Link have
  none and nothing is staged for them.
- `KL_SKIP_OBB=1` — stage everything except the OBB. It is the largest single
  file by far and it changes only with the APK, so this is the knob for the
  second upload onward.
- `KL_JKXR_DIR=<dir>` — where `stage_assets.sh` finds the RETAIL game data for
  an engine-port guest, default
  `~/Library/Application Support/Klepton/userdata/<guest>/JKXR` — the host run's
  own external storage. JKXR ships only the port's VR pk3s in its APK and
  expects `assets0.pk3` and up beside them, so `<dir>/<JK2|JK3>/base/assets*.pk3`
  is what goes across, at the same relative path, into
  `<container>/android-files/JKXR/…`. The pk3s there are symlinks into the
  user's install and are staged through their resolved paths — a symlink copied
  as a symlink puts correctly named 72-byte files on the device and the engine
  reports the game data missing anyway. `KL_JKXR_DATA` stays a HOST-side knob
  (set it once for a host run; a Mac path names nothing on device).
- `KL_SKIP_RETAIL=1` — stage everything except that retail data. Same reasoning
  as `KL_SKIP_OBB`: 1.2 GB that changes only when the user's install does.
- `KL_LOG_OUT=<file>` — where the pulled log lands (device runs default
  `/tmp/klepton-device.log`; `build_run_host.sh` uses it too, default
  `/tmp/klepton-host-<target>.log`).
- `KL_WATCH=<n>` — how many 5 s polls to watch the device log for
  (default 120).
- `KL_QUIET=<n>` — how many no-growth polls before declaring the run over
  (default 32 ≈ 160 s). Must outlast `KL_ALARM`, or the poller declares the
  run over before the watchdog that would explain it has fired.
- `KL_KEEP_LONGEST=` (empty) — let a shorter fresh log overwrite a longer
  previous one; default keeps the longest, because a launch that never
  happened leaves the previous run's file in place.
- `KL_CONSOLE=0` — go back to polling the container's log with
  `devicectl device copy from` instead of streaming the app's output over
  `devicectl device process launch --console`. **Streaming is the default for
  an open-ended run**, and the reason is measured rather than stylistic: a
  `copy from` costs **~27 s per call whatever the file's size** (150 KB and
  15 MB are indistinguishable), and the poller made one every 5 s until the log
  had not grown for `KL_QUIET` samples — up to an hour of transfer for a run
  measured in minutes. Streaming costs nothing and is live. Keep the old path
  working: a launch from the Home View has no console at all, and a run whose
  app dies still leaves the container's file for the next launch to pull.
  Streaming implies `KL_LOG_FILE=0` (below) — `run.sh` sets it, because a
  console launch whose app still redirects into a file produces no output
  anywhere.
- `KL_CONSOLE_STOP=1` — quit the app when the watch loop ends. Default leaves
  it **running on the headset**, still streaming: signals sent to `devicectl`
  are forwarded to the app, so tearing the bridge down would quit the thing you
  are looking at the moment the script stops watching it. The next run's
  `--terminate-existing` ends both, so at most one bridge is ever outstanding.
- `KL_LOG_FILE=0` — the app does **not** redirect stdout/stderr into
  `Documents/klepton-boot.log` (visionOS, `kl_app.c`). The only way anything
  outside the process can read them live; the in-window boot report and the
  after-the-fact pull both go empty in exchange, so this is set by the console
  path rather than by hand.

## Host environment pass-through

Not knobs — the shim reads these to answer the guest with host truth.

- `TMPDIR` — `kl_libc.c` builds the synthetic `/proc` tree under
  `$TMPDIR/klepton-proc.XXXXXX`, falling back to `/tmp/`.
- `LANG` — `kl_jni.c` derives `Locale.getDefault()`'s language and country
  from it (`en_US.UTF-8` becomes en/US). Empty, unset, or `C*` falls back to
  en/US.
- `KL_BACKUP_EXCLUDE=0` — do not mark the game's files as left out of the device
  backup at this start. The **default marks them**: the APK, the unpacked assets,
  the expansion folders, the Linux trees and the shader lists are what the
  installer can put back, and in Documents they would otherwise go into the
  owner's device backup (72 GB for Half-Life: Alyx). Saves and settings are never
  marked. A mark already set stays. `visionos/Sources/KleptonBackup.swift`.
- `KL_ALYX_GPU_IDS=0` — Half-Life: Alyx: leave `hlvr/cfg/deckard/video2.txt`
  alone at start. The **default puts the running GPU's VendorID/DeviceID into
  it**, because the game replaces the whole file with its automatic setup when
  they differ, and the device id changes with a system update. The boot log's
  `video file:` line says what was found. `alyx_video_gpu_ids` in `kl_app.c`.
- `KL_ALYX_TEX_FLOOR=0` — Half-Life: Alyx: leave out the two launch arguments
  `+r_texture_stream_resolution_bias_min 1.0 +r_texture_budget_dynamic 0`
  (the texture streamer's floor and a pool that keeps its given size), for a
  comparison with the game's own 0.8. `KleptonAlyx.swift`.
- `KL_VK_EYE_ENVGUARD=0` — Half-Life: Alyx: leave the `vr_eyeball` pixel
  shaders as the game ships them. The **default (on for `alyx`) rewrites them**
  when their modules are created: they index the scene's cubemap table with a
  packed value instead of its upper bits and so read far past the end of a
  constant buffer — zeros on a PC GPU, which bounds-checks that, and whatever
  lies there on Metal, which does not: NPC eyes flashing solid red, green, blue
  or magenta for a frame or three. Six modules of Steam build 25487405 are
  recognised by their shape; `[vk] eye cubemap fix:` lines in the log say how
  many were rewritten. `runtime/gfx/kl_vk_eyefix.c`, after Glovebox.

- `KL_PERF_LOG=0` — no `[perf]` line. The **default writes one a minute** while
  a guest that owns its frame loop draws: its frames a second, the rate the app
  is displayed at (the poses the compositor offered), the GPU time of the scene
  command buffers where that is measured (`KL_MTL_GPU_TIME`), the thermal state
  (0 nominal .. 3 critical) and memory used / left. `kl_app.c`, `perf_tick`.
- `KL_MTL_GPU_TIME=0|1` — the GPU time of the command buffers that carry the
  multisampled scene pass, from their own start and end times (a completed
  handler each; **on for `alyx`**). Feeds `[perf]`.
- `KL_STATS_CARD=1|line` — how the game runs, shown in the headset; see "The
  metrics panel" at the end of this file. Half-Life: Alyx sets it from "Show
  performance" in its start window.
- `KL_GUEST_PACE_DIV=<n>` — a guest paced through `xrWaitFrame` starts a frame
  on every nth refresh of the panel (default 1). Timed against the panel's own
  period, so a display rate the system has halved does not make it a quarter.
  Alyx: "Frame cadence".
- `KL_VK_MSAA_MEMORYLESS=1` — the multisampled depth target of the scene pass
  in Metal memoryless storage (88 MB at Alyx's largest picture with 2x, about
  165 MB at the medium one with 4x). **Alyx sets it** unless "Keep depth
  samples out of memory" is switched off in its start window.
  `[vk] memoryless:` lines say what was moved, `[mtl] memoryless:` lines count
  any pass that wanted to load or store it — which must stay none.
- `KL_VK_METER_STORE_EVERY=<n>` — with `KL_VK_PASS_BARRIERS=0`: the scene
  pass's multisampled colour, which is stored for the exposure meter to read,
  is stored on every nth frame only (default 1). Measured as no gain
  (`KL_VK_METER_STORE_AB`, 2026-10-13), so nothing sets it.
- `KL_ALYX_CADENCE=1|2`, `KL_ALYX_FEWER_EFFECTS=0|1`, `KL_ALYX_STATS`,
  `KL_ALYX_LEAN_DEPTH=0|1` — Alyx's frame cadence, effects, frame-rate readout
  and depth samples, named by a launch instead of the start window.
- `KL_ALYX_AUTO_SIZE=1`, `KL_ALYX_LEAN_METER=1` — two trials that are not in
  the window: the engine's own picture-size controller (it only ever went
  down) and `KL_VK_METER_STORE_EVERY=8`.
- `KL_ALYX_FAST_DEVICE=0|1` — the picture size of Alyx's "Balanced" and
  "Sharp": 1 the largest, as found on the Vision Pro with the M5, 0 one size
  below it, for the M2. Default by `hw.machine` (`RealityDevice17` and later: 1).
- `KL_ALYX_TEXTURE_POOL=<MB>` (0, 1024, 1536, 2048, 2560, 3072) and
  `KL_ALYX_TEXTURE_CAP=<px>` (1024, 2048, 4096) — Half-Life: Alyx's texture
  memory and the finest size a texture is loaded at, named by a launch instead
  of the start window. 0 is "Automatic": what the
  picture size leaves (1536 MB at the smallest picture, 768 at the largest).
- `KL_VK_METER_STORE_AB=<frames>` — measure `KL_VK_METER_STORE_EVERY` inside
  one run: the colour is stored every frame for `<frames>` frames, then every
  8th for as many, and so on; after every four rounds of each a `[vk]
  KL_VK_METER_STORE_AB:` line gives the scene GPU time of both and the
  difference. For a run from `Documents/lx/env.txt`; not a switch in the window.
- `KL_VK_FOVEATE_OUTER=<tangent>` — with `KL_VK_FOVEATE`: the tangent at which
  the edge rate is reached; beyond it everything is drawn at that rate. Default
  is the widest edge of the cone, a fall over the whole picture. Alyx's
  "Strongest" sets 0.70 (35 degrees).

## Taken over from rebelancap's fork, round 4 (avp-play issue 16)

What was taken before the release is additive or a switch; his audio sink and
his pose/frame-record core replace ours and are not in. Each of the switches
below is also a row under Advanced > Test in a game's start window.

- `KL_FMOD_BLOCK=<frames>` — the block size `org.fmod.FMOD.getOutputBlockSize`
  answers. **Default 512**, what Asgard's Wrath 2 and Vampire were heard with;
  his fork answers 256 (the number `OUTPUT_FRAMES_PER_BUFFER` gives).
  `runtime/jni/kl_jni_fmod.c`.
- `KL_AUDIO_ONE_OUTPUT=1` — `AudioManager.getDevices` shows the guest one
  output (the built-in speaker) instead of an empty list. **Default off, on for
  Pistol Whip**; his fork answers one to every guest, because Meta's XR Audio
  sink for Wwise picks its output from that list. `runtime/jni/kl_jni_net.c`.
- `KL_CROWN_SILENCE=0` — do not silence the output when the scene goes inactive
  or the compositor layer is torn down. **Default on**: the Digital Crown takes
  the immersive space away at once, the system backgrounds the app up to half a
  minute later, and until then the game played on. `KleptonAudio.swift`,
  `kl_audio_set_silenced`.
- `KL_XR_BOOT_GATE=1`, `KL_XR_BOOT_GATE_MS=<ms>` (4000), `KL_XR_HONEST_HEAD=1` —
  OpenXR guests: hold the first frame until a head pose and both hands have
  been published, and report an unpublished head as untracked. **Default off**
  (his: on wherever a display paces the frames). `runtime/xr/kl_openxr.c`.
- `KL_OVRP_GL_ARRAY=0|1` — existing switch; **now also on for Tetris Effect**
  (`x3386618894743567`), which asks for the Array eye layout on GL whatever it
  is told. His fork describes such a layer as Stereo instead.
- The `[bank]` lines (soundbank and expansion-file opens, stats and failures)
  are always on, as in his fork. `runtime/libc/kl_libc.c`.
- `[sl] heartbeat:` every ten seconds in a device log: players, state, buffers
  consumed, the output peak. `kl_opensl_heartbeat`.

Not from his fork, found on the way (2026-10-13):

- `KL_OVRP_RECO_EYE=0|1` — `ovrp_GetLayerRecommendedResolution` answers the eye
  texture size at density 1 instead of the eye layer's own (largest) size.
  **Default on for Red Matter 2 and Red Matter** (`x2180753588712484`), which
  otherwise draw at their ceiling in every frame. `runtime/xr/kl_ovrp.c`.
- `KL_BUILD_MODEL=<name>` — existing override; **Red Matter 2 is told "Quest"**,
  the only Quest model name it knows.
- `KL_OVRP_EYE_DESC_CAP=1` — the eye cap (`KL_OVRP_EYE_MAX` or the target's
  default) also bounds the layer a guest ends up with after its OWN scale, not
  only the size it is offered. Moss multiplies the offer by 1.5 in a level and
  got 4608x3697 an eye over a 3072 cap: 18.8 ms of GPU a frame on the headset,
  45 frames a second. **Default off; on for Moss** (seen on the headset: about
  110 frames a second), **for Gun Club VR** (`x2285803881494677`: Unity's 1.4
  made its 4000 cap 5600 an eye, about 42 frames a second, toolchain 557)
  **and for any game with a "Max resolution per eye" chosen** in the start
  window; Beat Saber's 1.2x level scale is wanted. The bound is the cap times
  the render scale, so "Resolution" still lowers the picture. His fork
  bounds the desc always. `kl_ovrp_eye_desc_size`.
- `KL_OVRP_EYE_ALIGN=<n>` — each axis of the eye size is rounded down to a
  multiple of n, last of all. **Default 1 (off), 4 for Moss.** Unreal rounds its
  scene buffers UP to a multiple of 4 and kl_fove compares extents exactly: Moss
  drew a 3840x2468 scene for a 3840x2465 eye, and foveated rendering was on and
  marked nothing (`[fove] near miss … at ANOTHER extent`, `encoders marked 0`).
  On the Mac with 4: marked, 5.0 ms of GPU a frame instead of 18. From his fork
  (be18669). `klovrp_eye_align`.
- `KL_VK_ANISO=<n>` (2..16) — a Vulkan guest's linearly filtered samplers get
  at least n-fold anisotropic filtering; compare and unnormalized samplers are
  left alone. **Default off.** Moss asks for none at all (its Quest 2 device
  profile ends at `r.MaxAnisotropy` 1; the census line `[vk] samplers so far`
  says what a guest asked for). `klvkrec_CreateSampler`.
- `KL_STATS_CARD=1`, `KL_GUEST_PACE_DIV=2` — existing switches, now also Test
  switches for every game: the frame-rate line in the headset, and a frame on
  every second refresh while the panel runs at its full rate.

Moss's defaults since 2026-10-13 (the owner's setting on the headset): `KL_OVRP_EYE_MAX` 4608,
`KL_OVRP_EYE_DESC_CAP` on, `KL_OVRP_EYE_ALIGN` 4, `KL_VK_ANISO` 16, `KL_GUEST_PACE_DIV` 2, foveated
rendering at "high". "high" itself changed with it for every Vulkan game: box 0.18 and floor 0.16 (were
0.30 and 0.08); the Test switch `KL_FOVE_BOX=0.30` + `KL_FOVE_FLOOR=0.08` is the way back.

After the release, one piece of his pose core (his commit be18669): added as a
switch that was off (2026-10-13), on by default from 2026-10-15 as the
OVRPlugin half of "no XR prediction", and **always on** since its switch
(`KL_OVRP_ONE_POSE`) was removed. `runtime/xr/kl_ovrp.c`, the block `ONE POSE
PER GUEST FRAME`.

- One head pose per guest frame, for a guest that delimits its frames with
  `ovrp_WaitToBeginFrame` (see `KL_XR_PREDICT` for the OpenXR side). At the
  frame's boundary the published head pose is frozen together with the eye
  transform (offsets and cant), and every head-derived answer for the rest of
  that frame returns it: both `ovrp_Update2/3` steps, every
  `ovrp_GetNodePoseState`, the eye view and the head an OpenXR guest reads.
  Hands and controllers stay live. The frame record filed for reprojection is
  taken from the same samples and so carries the frozen pose.
  Without it an OVRPlugin guest got one sample per step, so the Physics and the
  Render step differed, and a second Render sample in the same frame differed
  again. `KL_OVRP_LATCH` and the step samples keep running underneath. A read
  that names a frame index (`ovrp_GetNodePoseState3`) is still answered from
  that index's own sample (`KL_OVRP_RECORD_INDEXED`), which is the pose frozen
  when the guest sampled for that index. `make posebound` is the gate.
- `KL_OVRP_POSE_BOUNDARY=cycle|index` — for a guest that arms one-pose: what
  renews the frozen pose. **Default `cycle`**: once per `ovrp_WaitToBeginFrame`.
  `index`: whenever `ovrp_Update2/3` carries a new frame index — which freezes
  twice in a frame whose two steps carry different indices (Unity with the
  render thread trailing; his measurement on Paint the Town Red VR). A guest
  that never calls `ovrp_WaitToBeginFrame` (OpenXR) is renewed at each
  `kl_ovrp_frame_latch` whatever this says.

  With the switch on, two lines at the start (`[ovrp] one pose per guest frame
  ON`, `[ovrp] pose boundary = …`) and four every 300 frozen frames; with it off
  none of them:
  - `[ovrp] one-pose: served update2 A step B head C eyeview D live E |
    live-divergence worst X deg mean Y | eye-offset rot pushes … changes …
    worst … deg` — how many answers the frozen poses gave per entry point
    (`live` = asked before the first freeze, answered the old way).
    `live-divergence` is how far the published pose had moved on from the
    frozen one when it was asked for: the motion that used to reach the
    picture inside one frame. The last three numbers are how often and how far
    the eye's rotation changed between pushes from the compositor.
  - `[ovrp] pose boundary: freezes F, reuses R, guest frame cycles C |
    poses/frame P | update2 per freeze U (mode=cycle|index)` — `poses/frame`
    must read 1.00; above that a frame was answered from two poses (his 1.70 in
    gameplay with `index`). `update2 per freeze` is 2.00 for a guest that
    samples both steps once per frame. `guest frame cycles 0` with
    `mode=index` is a guest that does not call `ovrp_WaitToBeginFrame`.
  - `[ovrp] frozen series: mean step … deg, alternation index …, sign flips …%,
    worst reversal … deg` — the frozen poses against each other. A head turned
    evenly gives an alternation index near 0 and no sign flips; an index near 1
    or flips while turning mean the series itself zigzags (two time bases
    interleaved in what the compositor publishes), which no reprojection sees.
  - `[ovrp] intra-frame pose spread: worst … deg` — the largest angle between
    two head poses `ovrp_GetNodePoseState` returned between two frame
    boundaries. 0.000 when every read was answered from the frame's pose;
    above that, reads named another frame's index (a render thread still
    drawing the previous frame — each such read still matches that frame's
    record) or straddled the boundary.

## The metrics panel (`KL_STATS_CARD`)

- `KL_STATS_CARD=1` — a panel fixed to the head just above the middle of the
  view (0.34 x 0.17 m at 1.2 m, its centre 5 degrees up), after the performance
  overlay of a Quest (OVR Metrics Tool): the last second in numbers and the
  last minute as three graphs, refreshed once a second. In English whatever
  the device speaks. It is drawn into the game's picture and so is as sharp as
  that is — 30 pixels a degree at an output quality of 0.4 (Moss), 40 at 1.0 —
  with a texture level drawn for each of those sizes; no setting of its own
  makes it sharper than the picture, the game's "output onto the display" does.
  **Default off, and that only means "not there from the start"**: the panel
  can be called up in every game (next paragraph). Test switch "Show
  performance in the headset from the start".
  `KL_STATS_CARD=line` is the one line it began as (frames a second, display
  rate, GPU load, heat), at the lower left.

  **Create + Options held together for a second** show the panel and hide it
  again, in every game and whatever `KL_STATS_CARD` says. Nothing is sampled or
  counted before the panel is first shown (it says "Measuring …" for its first
  second, and its graphs begin there); from then on the sampling goes on while
  it is hidden, so the graphs are whole when it returns. So that the game does
  not open its menu with it, the two buttons always go through
  `kl_metrics_chord`: a press of one alone reaches the game a quarter of a
  second late (a shorter tap is played once it is over), and while both are
  down neither does — also when they are let go before the second is up.
  `KL_STATS_CARD_BUTTONS=0` hands both buttons to the game untouched; the panel
  is then only what `KL_STATS_CARD` makes it. `applyPanelChord`,
  KleptonControllers.swift.

  The panel is drawn **over every quad of the game's**, whatever its distance
  (`KleptonOverlayDepth.statsCard`: no depth test, its own depth written). Through
  toolchain 563 (`fd7f273`) it was depth-tested at its 1.2 m, and a quad nearer than that
  hid it over the quad's whole rectangle, transparent parts included: in Alien:
  Rogue Incursion it showed in the menu and was gone once a level began (a panel
  held in the hand, interaction labels). `make statscard` runs the pass on the
  Mac and writes `{menu,level}-{before,after}.png`.

  **A short tap of Create + Options together** (both up again within the
  second) while the panel is shown switches **MetalFX upscaling** from the next
  frame, in every build — where MetalFX can upscale: the device has Apple's
  spatial scaler and the game was started below 100 % resolution
  (`KleptonMetalFX.canUpscale`). The panel then has a second hint line, "Tap
  both: MetalFX on / off", and redraws at once after a tap: *MetalFX*, *Path*
  and *Output* show the new state. It is the same choice as *Settings ›
  Picture › MetalFX upscaling*, stored the same way, and the switch there
  shows it at once. Every game's Picture box has *Resolution* and this
  switch together, Alyx's too (beside its Graphics presets, which neither
  hide nor lock them), and so does the developer window's; *Advanced ›
  Picture, in detail* only points to it. Anywhere else the
  tap does nothing (`[metalfx] tap of Create + Options: nothing to switch` in
  the log). It replaces the live XR head prediction switch test builds had.

  | Row | What it is |
  | --- | --- |
  | FPS / Hz | new pictures of the game shown a second / pictures presented a second (the rate the system shows the app at) |
  | Shaders | requests a second to Metal for a library from a shader's source or a render pipeline state, by ANGLE or MoltenVK alike, in two numbers: **new** — took 2 ms or longer, so it was compiled — and **cached** — answered at once from what the system had compiled before, in this run or an earlier one (`klc_note`, `runtime/gfx/kl_mtl_busy.m`; measured on a Mac: 0.01-0.1 ms against 4-95) |
  | GPU | share of the second the GPU worked for this app: the spans of every command buffer the process committed, overlaps counted once (`runtime/gfx/kl_mtl_busy.m`) |
  | GPU, game | that time less the compositor's, over the game's frames |
  | GPU, display | GPU time of one composite pass |
  | CPU | CPU time of the whole process, in cores |
  | (thread name) | the busiest thread's share of one core — the one a CPU-bound game waits for |
  | Memory | footprint, and what is left before the system's limit |
  | Heat | the system's thermal state |
  | Render | the picture the game draws, in pixels: the eye texture, or the first projection layer's (the backdrop) |
  | MetalFX | the upscale (`KleptonMetalFX`): **on** (green) when this frame was upscaled, **off**, or **linear · <why>** (amber) when it is on and the picture keeps the linear stretch — `100 %`, `budget`, `format`, `texture view`, … (`[metalfx] linear path:` in the log says it in full) |
  | Path | how the picture comes: the engine (`Vulkan`, `GL`), `OpenXR … · n layers` or the eye pass's `array` / `per eye`, and `foveated` for a GL eye drawn through the display's rate map |
  | Output | the size after the upscale, in the picture's own pixels (for a foveated eye the screen size the copy stands for); only while it upscales |

  The column closes up when it has more than nine rows, and a value too long
  for its row is set smaller.

  Graphs: frames a second against the display rate; GPU load and the busiest
  thread; shaders a second, as a bar for every second that had any, the compiled
  ones red from the bottom and the cached ones grey on top — a hitch in the
  first graph with a red bar under it was a compile. (The sample also
  holds the repeated pictures a second, `stale`; the panel showed them until
  the owner asked for the compiles in their place — they are the display rate
  less the frame rate, which both stand at the top.)

  Counted where a picture is shown (`kl_metrics_note_shown`, the compositor's
  frame loop), so every kind of guest has a frame rate — the line before this
  read the count of started frames, which only a guest paced through the XR API
  has, and showed nothing for a Unity game. The sample is taken on the
  heartbeat's thread and the picture of it drawn on a queue of its own; the
  compositor's thread pays two counts and one quad a frame.
  `runtime/diag/kl_metrics.c`, `visionos/Sources/KleptonMetrics.swift`
  (drawing only — runs on a Mac with made-up samples), `encodeStatsCard`.
  Tests: `build/t_metrics`, `build/t_mtlbusy`.

## The profile for untested games (`KL_UNTESTED`)

A store title (`x<app id>`) that is not in `runtime/guest/kl_tuned.h` gets
better defaults (`kl_untested_profile`, `runtime/guest/kl_driver.c`). The list
holds every title brought up before the profile existed (2026-10-17): every
x-id named in the runtime, the app or these docs, and every title in the AVP
Play catalogue's seed. Those keep exactly what they were tested with. A title
added later stays under the profile unless it is added to the list.
`KL_UNTESTED=0` turns the profile off; `=1` turns it on for any target. Each
switch below still wins over it. The run says so once:
`[klepton] <target>: no title here has been tested — the profile for untested
games is on`. Host test: `make untested`.

Under the profile:
- `KL_OVRP_UNANSWERED=unsupported`: an OVRPlugin entry point with no answer here
  says UNSUPPORTED instead of ending the run.
- `KL_JNI_SOFT_UNKNOWN=1` (`runtime/kl_jni.c`): a Java method or field with no
  answer here answers zero (nothing for void, 0 / false, a null object), each
  named in the log with `...answered with zero`. Only these two lookups;
  `KL_PERMISSIVE` (which also zeroes EGL and GL) is not involved.
- `KL_OVRP_GUEST_VERSION=<1.x.y>`: the OVRPlugin version the game ships, told
  to the game instead of 1.60.0. Read from the game's own `libOVRPlugin.so`
  when the app is built (`visionos/targets.py` `ovrp_guest_version`,
  `KLOvrpGuestVersion` in Info.plist), or from the library itself on a Mac host
  run. The rule: the one NUL-delimited `1.<minor>.<patch>` with a minor of 20
  or more. No such string, or more than one, leaves 1.60.0.
- `KL_PLAT_USER=1` with an id of this installation's own, and
  `KL_PLAT_IAP_EMPTY=1`: an offline player and an empty purchase list. Only the
  player's identity; ownership is still checked.
- `KL_ELF_TLSDESC=1`: TLSDESC relocations are served.
- For a Unity guest: `KL_GLFB_BIND_PINS=1` (on GLES; the pins are skipped for a
  number two uniforms of a program would share, and a full pin table is said)
  and `disable-gpu-skinning=1` in boot.config (`KL_UNITY_BOOT_EXTRA`). The boot
  config is read before Unity picks its graphics API, so a Vulkan title skins
  on the CPU too.

## Ghost Town (`x9531494193591052`, community PR #41)

Taken over from sibest's PR, scoped to this title; the proximity sensor from the
same PR is issue #42. Set by `kl_driver_env_defaults` for the title:
`KL_GHOSTTOWN_INPUT`, `KL_ELF_TLSDESC`, `KL_AUDIO_ONE_OUTPUT`,
`KL_GHOSTTOWN_OVERLAY_COLOR`, `KL_OVRP_ENQ_OWNS_OVERLAYS`; by its own defaults:
`KL_OVRP_ENQ_RETIRE`, `KL_OVRP_RECENTER_COUNT`, `KL_OVRP_ADAPTER_PHYS`,
`KL_VK_PARK_INSTANCE`, `KL_OVERLAY_TEXL` (off), `KL_OVRP_TEXPTR_INDIRECT`,
`KL_OVRP_SPACEWARP_TEX`, `KL_AAUDIO_MMAP_POLICY`; and the eye unification list.

- `KL_GHOSTTOWN_INPUT=0|1` — (`KleptonControllers.swift`) Options is the Quest
  Menu button, held and released as pressed (left: Start; right: Start and
  Back), instead of going through the metrics panel's chord; the right one held
  for a second recenters (`KleptonCompositor.pendingRecenter`).
- `KL_GHOSTTOWN_OVERLAY_COLOR=0|1` — an enqueued panel keeps the per-layer
  colour scale and offset the guest files with it (for Ghost Town's panels),
  applied in the overlay shader.
- `KL_AAUDIO_MMAP_POLICY=0|1` — answer `AAudio_getMMapPolicy`,
  `AAudio_setMMapPolicy`, `AAudio_getPlatformMMapPolicy`,
  `AAudio_getPlatformMMapExclusivePolicy` and `AAudio_isMMapSupported` (mmap
  unavailable, AUTO accepted), which the Oboe in Ghost Town asks for. Default 1
  for `x9531494193591052`; every other guest is told the names are not there.
- Its scenes are loose AssetBundles in `Android/obb/<package>`
  (`visionos/targets.py`, `GENERIC_OBB`), and its own Java class
  `DetectHeadphones` is answered by `runtime/jni/kl_jni_ghosttown.c` (no jack
  here, so no plug event is ever sent).

## Banners & Bastions (`x8387152738076399`)

A mixed-reality tabletop game on Unity 6000.3 (Vulkan) with Unity's OpenXR
plugin, Meta's MetaXRFeature, the Interaction SDK and MR Utility Kit — the
stack DreamSpace runs on. Set by `kl_driver_env_defaults` for the title:
`KL_OVRP_VERSION` (the version its own `libOVRPlugin.so` carries; told 1.60.0
its MetaXRFeature logged "OnInstanceCreate returned an error" and the Meta
half of the SDK stayed off: picture glued to the head, no controllers),
`KL_OVRP_UNANSWERED=unsupported` (as for the other titles told their own
version; the first it reached was `ovrp_SetControllerDrivenHandPoses`),
`KL_OVRP_PASSTHROUGH`, `KL_ELF_TLSDESC` (MR Utility Kit),
`KL_OVRP_SCENE_ROOM`, `KL_XR_LOCAL_FLOOR`, and the pipeline record and replay
of Demeo (`KL_VK_FOZ_*`) for the stutter of its first-drawn pipelines. The app
counts it among the mixed-reality games (`KleptonTuning.swift`,
`targetIsMixedReality`): no full-space frame-rate choice. Each can be switched
off from the launch environment as documented above.

## Test switches that are on from the installation (`KLEPTON_TEST_ON`)

- `KLEPTON_TEST_ON=<name>[,<name>…]` — a BUILD-time variable, read by
  `visionos/gen_xcodeproj.py`: the Test switches (by the name of their
  variable, e.g. `KL_XR_PREDICT,KL_METALFX_UPSCALE`) that this installation has
  on from its first start. It lands in Info.plist as `KLTestOn`;
  `KleptonTestSwitches.isOn` takes a choice made in the game's window first and
  this list second, so a switch named here can still be turned off in the game
  (and stays off), and one that is put back follows the installation again.
  Anything that is not a `KL_…` name is dropped. Unset: every switch is off
  until chosen, as before. AVP Play sets it from a game's own option *XR
  prediction* (`KL_XR_PREDICT`) and from its MetalFX setting
  (`KL_METALFX_UPSCALE`); the log line of such a switch ends in `(on from the
  installation)`. `KL_OVRP_ONE_POSE` and `KL_AUDIO_SINK2` in the list (set by
  older AVP Play versions for "Experimental: the new pose and audio
  implementations") are no longer switches and do nothing: both are always on.
  `KL_OVRP_EYE_MAX` in the list is no longer a switch either: it sets *Max
  resolution per eye* to 4608 at the first start, once (see `KL_OVRP_EYE_MAX`).
- `KLEPTON_TEST_ON_EXTRA=<name>[,<name>…]` — more names for the same list,
  for a tester's own install. AVP Play sets `KLEPTON_TEST_ON` itself (and
  drops a value from the shell), so a test copy that needs one more switch
  cannot name it there; `gen_xcodeproj.py` adds the names from this variable
  to `KLEPTON_TEST_ON`'s (same `KL_…` filter, duplicates dropped) and bakes
  both into `KLTestOn`. Unset: `KLTestOn` is byte for byte what it was. It does
  not make a test build by itself. Example (Moss test copy with the old depth
  reads, as the A/B control for `KL_FOVE_SUBPASS_HELPERS`):
  `KLEPTON_TEST_ON_EXTRA=KL_FOVE_SUBPASS_HELPERS avpplay install moss --bundle-id
  <live-id>.test --display-suffix " (Test)" --toolchain <worktree> …`.
- **Tester-only switches.** An entry marked `testBuildOnly` in
  `KleptonTestSwitches.entries` is listed and applied only in a test build
  (`KleptonTestBuild.on`: `KLTestBuild` in Info.plist or `KL_TEST_BUILD=1` at
  launch); a player's build never shows it and ignores it in `KLTestOn`. The
  first one is `KL_FOVE_SUBPASS_HELPERS=0` (an A/B control that turns a fix
  OFF, so it has no place in a player's Test group).
- `KLEPTON_FRAMEGEN=off|ahead|between` (build/install environment, read by
  `visionos/gen_xcodeproj.py`) — the frame generation a tester's install starts
  at: it replaces the target's `framegen` as `KLFrameGenDefault`; a choice made
  in the game's start window still wins. Unset: the target's default.
- `KLEPTON_RENDER_SCALE=<0.5..1.0>` (build/install environment, read by
  `visionos/gen_xcodeproj.py`) — the render scale the game starts at: it
  replaces the target's `render_scale` as `KL_RENDER_SCALE_DEFAULT` /
  `KLRenderScaleDefault`, so *Resolution* in the game starts there and a value
  chosen in the game still wins. AVP Play sets it with its MetalFX switch
  (75 % unless chosen otherwise). Unset: the target's default, byte for byte
  as before; a value outside 0.5..1.0 or not a number is said on stderr and
  ignored.
- `KL_METALFX_UPSCALE` in that list — not a Test switch of its own but the
  start value of *Settings › Picture › MetalFX upscaling*
  (`klepton.metalfxUpscale`, `KleptonMetalFX`): on from the first start, off
  again in the game if chosen there. The switch sits under *Resolution*
  wherever the device has Apple's spatial scaler; turned on there at 100 %
  it sets *Resolution* to 75 % from the next start and says so, and marks
  that it did (`klepton.metalfxLoweredResolution`, kept across starts). Turned
  off (there or with Create + Options) it sets 100 % back only while that mark
  stands; any move of the slider clears it, and a resolution the player chose
  is never changed (`KleptonMetalFXPreset`, `make mfxpreset`). While the game
  shows the room (a passthrough layer, `kl_ovrp_passthrough_live`) Apple's
  scaler alone would cover it with black: it writes alpha 1 into every pixel,
  whatever came in. So after the scaler one draw per eye writes the source's
  own alpha, sampled bilinearly, back into the copy (`KleptonMetalFXAlpha`,
  `make mfxalpha`); the log says `[metalfx] the game shows the room
  (passthrough): upscaling with the picture's alpha put back` and the panel
  reads `MetalFX on · MR alpha`. With `KL_MFX_MR_PAUSE=1` (below) it keeps the
  linear path instead. Either way the stored switch, the resolution and the
  mark stay as they are (`KleptonMetalFXPause`). Unnamed, the switch starts off, as
  before. AVP Play sets it from its setting "MetalFX: switch on upscaling
  automatically when installing". The compositor's `[metalfx] on` line then
  ends in `(on from the installation, KL_METALFX_UPSCALE)`; each place it
  keeps the linear path is said once as `[metalfx] linear path: <reason>`.
  Every path is upscaled: the eye pass (a shared 2-slice array, a texture per
  eye), a GL eye drawn through the display's rate map (only its physical
  part, the grid normalised to it), and every projection layer of an OpenXR
  guest, each eye from its own texture. `[metalfx] upscaling <what>` names
  each copy when the arrangement changes. The metrics panel (`KL_STATS_CARD`)
  shows *Render* (the size the game draws), *MetalFX* (on / off / linear and
  why), *Path* (engine and composite path) and *Output* (the size after it).
- `KL_MFX_MR_PAUSE=1` — MetalFX holds off while the game shows the room
  (passthrough live) instead of putting the picture's alpha back into its
  copies: the composite takes the linear path, the log says `[metalfx] paused
  while the game shows the room (passthrough, KL_MFX_MR_PAUSE)` and `[metalfx]
  passthrough ended — upscaling again`, the panel reads `MetalFX linear ·
  paused: MR`, and the settings say under the switch that MetalFX pauses —
  only while passthrough is live. The fallback should the alpha pass show
  halos or cost too much in a mixed-reality game. Unset or 0: the alpha pass.
- `KL_METALFX_BUDGET=<full eyes>` — the projection layers' share of MetalFX
  output a frame, in full eye pictures (1..16, default 4). A layer whose
  copies would go past it is drawn linear and said once (`budget` on the
  panel); the eye pass has no budget.
- `KL_METALFX_COLOR=hdr|linear|perceptual` — the scaler's colour processing
  mode, for A/B tests. Unset: RGBA16F `hdr` (the default, kept), the rest
  `perceptual`. An sRGB eye (`rgba8Unorm_srgb`, `bgra8Unorm_srgb`) is always
  `perceptual`, whatever this says: MetalFX makes no scaler for sRGB with
  `linear` or `hdr` (said once). Said once as
  `[metalfx] colour processing …`; any other value is said and ignored. Read
  once a run, at the first copy.

## The play area (`visionos/Sources/KleptonRoom*.swift`, `KleptonBoundary.swift`, `runtime/xr/kl_room.c`, issue #42)

- `KL_ROOM_GUARD=0|1` — the whole feature: "Measure room" beside Settings in
  the start window, Start only with a play area for the room the player is in
  (the room space checks it first: this game's world anchor as a guess, confirmed
  against what is in view with `kl_room_register_near`, or the obstacles in view
  matched against each measured room with `kl_room_register`; not found, the
  card asks "Measure room" or "Stationary"), and the edge shown in the game. A
  setting of each game (Settings › Play area, `klepton.room.enabled`); untouched,
  the installation's default `KLRoomGuardDefault` holds — on, unless the build had
  `KLEPTON_ROOM_GUARD=0` (AVP Play's setting for the games it installs). On the
  launch line it decides either way. Scripted starts (`KL_AUTOBOOT=1`) skip the
  check.
- `KL_ROOM_MODE=wall|room|off` — over the game's own setting (Settings › Play
  area): a see-through wall at the edge (default), the game turning
  see-through near it (mixed space only; in a full space the wall is drawn),
  or nothing.
- `KL_ROOM_STYLE=roomscale|stationary` — over the game's own setting (Settings ›
  Play area › Kind): the measured play area of the room, or a circle 1.5 m
  across round where the player is at the first frame (moved by recentring),
  with no room check at Start.
- `KLEPTON_SHARED_KEYCHAIN=0` — (`gen_xcodeproj.py`) build without the
  `keychain-access-groups` entitlement. The rooms are then kept by each game
  alone (`Application Support/klepton-rooms.json`) instead of in the team's
  shared group `<team>.avpplay.shared` (Info.plist `KLSharedKeychainGroup`).
- Log lines: `[room] …` — the store (`rooms shared through …`, `saved to …`),
  the scan (`suggested … m²`, `stroke … added|cut out`, `match against room …:
  score …, next best …`), the check (`this game's anchor for room … is back`,
  `recognised room … by its obstacles`, `not recognised within 12 s`) and the
  game (`play area: in room …`, `near the edge`).
- `make room` — `tests/t_room.c`: scan map, coverage, suggestion, drawing,
  distance field, outlines and room matching on a made-up 5 x 4 m room.

## Hand tracking for OVRPlugin guests (`runtime/xr/kl_hands.c`, `runtime/xr/kl_ovrp.c`)

ARKit's 27 hand joints, published by `KleptonControllers.swift` every frame
(`kl_ovrp_set_hand_joints`, predicted to the presentation time), answered as
OVRPlugin's hand tracking: `ovrp_GetHandState`/`2` (root pose = wrist, 24 local
bone rotations in OVR's convention, pinch strengths and bits, pointer pose,
hand scale, confidence), `ovrp_GetSkeleton` (a generic bind pose of our own),
`ovrp_GetSkeleton2/3` (the two OVR hand types, 70/84 bone slots),
`ovrp_GetHandTrackingEnabled`, `ovrp_GetControllerIsInHand`; hands are the
connected and active controller (0x60) while no Sense controller is tracked.
`ovrp_GetMesh` and `GetHandState3` stay refused. `tests/t_hands.c` (`make hands`) checks the
conversion on synthetic hands.

- `KL_OVRP_HANDS=0|1` — hand tracking for this guest. Default on for Elixir
  (`x3793077684043441`) and Angry Birds VR: Isle of Pigs (`x2718606324833775`),
  off everywhere else. A tracked Sense controller comes first: while one is
  there it is the input, without one the hands take over.
- `KL_OVRP_HANDS_ONLY=0|1` — hands are the input even with Sense controllers
  paired; a controller in a hand is that hand not seen. Default on for Elixir.
- `KL_OVRP_HANDS_AXIS=+x|-x|+y|-y|+z|-z` — which axis of ARKit's joint frame
  points out of the back of a finger. Normally found on a flat hand and logged
  (`[hands] … ARKit joint axis … is the back of the fingers`): -y left, +y right
  on visionOS 27.
- `KL_OVRP_HANDS_BESIDE=0|1` — the controllers stay the active input and each
  hand ARKit sees is only connected beside them (for a game that switches to
  hands with an option of its own). Default off.
- `KL_OVRP_HANDS_FIST=<deg>` — fist assist: a finger (index to little) bent more
  than this in all is bent further toward a full fist (170 -> 250 degrees);
  once engaged it holds down to 30 degrees less. Default 110 for Angry Birds
  (Meta's FingerPalmGrabAPI wants a Quest-tight fist for the slingshot), 0 off
  elsewhere.
- `KL_OVRP_HANDS_CURL=<gain>` — every finger bend times this (default 1). 1.3
  made claws of a relaxed hand in Angry Birds; the fist assist replaced it.
- `KL_HAND_SMOOTH=0` — no One Euro filter on the wrist in Surreal's hands mode
  (the only place it runs; `KleptonPoseSmoother`, see `docs/SURREAL.md`).
- `KL_HAND_SMOOTH_MIN=<Hz>` — its cutoff for a still hand, position and rotation
  (default 1). `KL_HAND_SMOOTH_BETA=<Hz per m/s>` — how fast the position cutoff
  rises with speed (default 8).
- Pinch (no switch): 1 at 3 cm thumb-to-fingertip, 0 at 5.5 cm (ARKit's tips
  sit inside the pads); on from 0.8, off below 0.3 for three frames; a fist (two
  of middle/ring/little tips within 4.5 cm of their knuckle) reports no pinch.
  A wrist that jumps more than 12 cm in one frame is held for up to 8 frames
  (hands covering each other); the pointer ray is smoothed by speed.
- `KL_OVRP_HANDS_HOLD_MS=<ms>` — a hand ARKit loses for this long stays where
  it was, tracked (default 250).
- `KL_OVRP_FAKE_HANDS=1` — Mac host runs: made-up hands in front of the head,
  the fingers closing and opening every four seconds. No frontend may publish
  hands at the same time.
- Log: `[hands] … first seen` (joint distances), a five-second line per hand
  (bend per finger, pinch, wrist, joints tracked) and five-second counts on both
  sides (`ARKit, last 5 s` in the frontend: frames seen/lost, hidden joints taken
  from ARKit's estimate or their last place; `guest, last 5 s`: reads of an
  untracked hand, frames held over a gap).

- `KL_GLFB_DENORM=0|1` (`runtime/gfx/kl_glfb.c`, default 1) — a subnormal float
  literal in an ES 3 shader (an HLSLcc integer constant such as 2.80259693e-45 =
  the int 2) is written as `intBitsToFloat(bits + kl_denorm_zero_)`. Metal's
  fast math flushes the literal to 0; in Elixir's potion-glass shader that is a
  loop's exit value, and the endless loop hung the GPU (the Mac's WindowServer
  went down with it, the headset picture stayed black).

## BRINK Traveler (`x3635172946605196`)

Community fix 84. Everything below is bound to this title unless it says otherwise.

- `KL_XR_DEPTH=0` — no `XR_KHR_composition_layer_depth` and no Unity depth
  submission, so no per-pixel depth for the real hands. Default on. Needs
  `KL_XR_LAYERS=0`, which this title defaults to.
- `KL_BRINK_REAL_HANDS=0` — keep the game's own hand mesh. Default: hidden, with
  tracking, colliders and the wrist UI kept.
- `KL_BRINK_UI_HEIGHT=<m>` — how far above the tracked palm the wrist menu sits
  (default 0.05, at most 0.12; outside that the menu is left where it is).
- `KL_BRINK_LOOK_PINCH=0` — no distance selection through visionOS's selection
  ray. Default on.
- This title also defaults `KL_OVERLAYS` and `KL_AUTOBOOT` to on, and sets
  `KL_FOVEATION=off`, `KL_OVRP_EYE_MAX=4096`, `KL_CP_QUALITY=1.0`,
  `KL_CP_FOVEATION=1` and `KL_AUDIO_PREFER=opensl`, each only if not given on the
  launch line.
- `KL_PERF_LOG=1` — any title: fresh fps, display rate, GPU and eye size to the
  log every fifth metrics tick.
- `KL_KEEP_START_WINDOW=1` — any title: the start window stays open after the
  immersive space does.
