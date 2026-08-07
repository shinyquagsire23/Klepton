# Environment variables

Every environment variable the host tooling reads, found by grepping the C
sources for `getenv` and reading each usage site. Grouped by area. The source
is the authority; CLAUDE.md's knob table is a curated subset. Unless an entry
says otherwise, presence of the variable (any value, even empty-but-set for the
C `getenv` checks) turns the knob on, the default is off, and the reader is the
`t_boot` binary via the runtime it links.

## Boot / recon (`tests/t_boot.c`)

- `KL_RECON_CHILD=1` — marks the re-exec'd recon child, so `main` skips the
  guard test and the fork and goes straight to the guest run. Set by t_boot on
  itself across the `execl`; not meant to be set by hand.
- `KL_NOFORK=1` — run the recon in-process instead of in the re-exec'd child.
  Required under lldb (macOS lldb follows neither fork nor exec), at the cost
  of the AGX abort being masked by the debugger's signal traffic.
- `KL_SKIP_GUARD_TEST=1` — skip the DRM-guard self-test, which misreads under
  a debugger (the forked child's abort is intercepted and never reads as
  SIGABRT to `waitpid`). The guard in `kl_ovrplat.c` itself is unaffected.
- `KL_FAULT_WAIT=1` — park in t_boot's fault handler on SIGSEGV/SIGBUS/
  SIGABRT/SIGALRM and print the pid, for post-mortem `lldb -p` attach.
- `KL_LIFECYCLE=1` — drive the UnityPlayer lifecycle sequence
  (nativeRecreateGfxState, nativeResume, nativeRender) instead of stopping
  after initJni.
- `KL_FRAMES=N` — after the lifecycle, pump N `nativeRender` calls, ticking
  the Choreographer before each frame and draining the posted-task queue
  between them. One frame is engine setup and draws nothing. Default 0.
- `KL_ALARM=<s>` — the watchdog around each lifecycle call (default 20s) and
  around the whole frame pump (default 60s). Widen it when the question is
  what the guest is waiting on.
- `KL_PERMISSIVE=1` — unimplemented JNI/GL/OVRPlugin/OVRPlatform calls return
  0 instead of aborting. Read by `kl_jni.c` (via `kl_jni_set_permissive`),
  `kl_egl.c`, `kl_ovrp.c`, `kl_ovrplat.c`. Scouting only — the guest carries
  on with answers we invented. Does not apply to the DRM entitlement guard.

## GL / null driver (`runtime/kl_egl.c`)

- `KL_DUMP_SHADERS=<dir>` — after the frame pump, write every captured
  `glShaderSource` text into `<dir>`. Read by t_boot; the dump runs in
  `kl_egl.c`. The only place Unity's GLSL ES exists in plain text.
- `KL_DUMP_TEXTURES=<dir>` — write every uncompressed 8-bit
  `glTexSubImage2D` upload as a PNG under `<dir>`. Armed before the guest runs
  (uploads happen all through init). Compressed uploads (ETC2/ASTC) are
  skipped rather than guessed at.

## ANGLE reference renderer (`runtime/kl_glfb.c`)

All of these require `KL_GLFB=1` to mean anything; without it the null driver
answers GL and kl_glfb never initializes.

- `KL_GLFB=1` — enable the one-eye reference renderer on ANGLE/Metal.
  Host-only; it exists to produce the known-good frame a real backend gets
  diffed against.
- `KL_ANGLE_DIR=<dir>` — where to load `libEGL.dylib`/`libGLESv2.dylib` from.
  Default: `vendor/out/Debug` when its libEGL is present, else Google Chrome's
  framework Libraries dir. Also read by spikes s09, s10, s11.
- `KL_ANGLE_BACKEND=gl` — select ANGLE's OpenGL backend instead of Metal (the
  default, asked for by name). Only the exact string `gl` switches. Also read
  by spikes s09, s10, s11.
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
  `glBindFramebuffer`, `glFramebufferTexture2D`) with thread ids.
- `KL_GLFB_NOSRGB=1` — allocate `GL_RGBA8` where the guest asked for
  `GL_SRGB8_ALPHA8`. A probe; a captured frame is then wrong (un-decoded
  sRGB).
- `KL_GLFB_NOSWIZZLE=1` — drop the four `GL_TEXTURE_SWIZZLE_*` parameters.
  Single-channel textures then sample as red; a probe only.
- `KL_GLFB_ERRPROBE=1` — bracket draws and blits with `glGetError` and print
  the first 20 with the state around them (draw FBO, program, viewport;
  `glCheckFramebufferStatus` on 0x506). Consumes the pending error. Also
  enables the blit-state log line.
- `KL_GLFB_DRAW_PROBE=1` — after each of the first 12 scene-sized draws (32+
  vertices), read back the centre 64x64 and report lit-pixel count and mean
  luma. `KL_GLFB_DRAW_PROBE_N` overrides the quota (0 = unlimited),
  `KL_GLFB_DRAW_PROBE_MIN` lowers the vert floor (the frame's last draws are
  small, and one of them may be the one you're hunting).
- `KL_GLFB_BLIT_PROBE=1` — read back the source before and the destination
  after each `glBlitFramebuffer`, and put `glClear`/`glClearColor`/
  `glInvalidateFramebuffer`/renderbuffer-storage on the same timeline as the
  draws (one line each, FBO/program/viewport/texture named). The instrument
  that answered "draws lit, blit reads black".
- `KL_GLFB_NO_INVALIDATE=1` — don't forward `glInvalidateFramebuffer`.
  ANGLE/Metal actually discards (memoryless attachments), so this tests
  whether an invalidate is the eraser. Diagnostic only.
- `KL_GLFB_DUMP_PROGRAM=N` — at link time, print the captured sources of the
  shaders attached to program N (turns the timeline's `program=N` into text).
- `KL_GLFB_TEX_LIMIT=N` — perform only the first N `glTexSubImage2D` uploads
  and drop the rest; storage allocations still happen, so only texel contents
  go missing. Bisects the upload stream in situ. Default unlimited.
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
- `KL_GLFB_OUT=<dir>` — request a PNG capture on each `eglSwapBuffers`. Read
  by `kl_egl.c`'s SwapBuffers; the readback is serviced from the next call the
  context-owning thread makes (or immediately when the swap arrives on that
  thread), because the swap itself does not.
- `KL_GLFB_OUT_EVERY=N` — throttle the PNG capture to every Nth swap (default
  1). Applies to the file path only; a registered frame sink gets every swap.
- `KL_GLFB_DUMP_FBOS=1` — alongside each `KL_GLFB_OUT` capture, write one PNG
  per live FBO (`frame_NNN_fbM.png`, same tone map). The intermediates — MSAA
  scene target, eye textures, the R11F bloom pyramid (blitted to RGBA16F for
  the read) — become inspectable, not just lit-counts. Reads are clipped to
  the `KL_GLFB_SIZE` buffers, so oversized attachments dump their top-left.
- `KL_GLFB_DUMP_SINK=<dir>` — with a frame sink registered (the viewer), write
  every 100th sink buffer to `<dir>/sink_NNNNN.png`. A black window with
  content in these files is an SDL-side problem; black files are capture-side.
- `KL_GLFB_RAWSTATS=0` — silence the per-60-captures line reporting the eye
  texture's raw (pre-tone-map) min/max/mean. On by default; it is how the
  "very dark" picture was shown to be 1e-3 linear content, not a bad curve.
- `KL_GLFB_EXPOSURE=<x>` — multiply the linear eye-texture value by <x> before
  the debug tone map (default 1). Makes the loading screen watchable; it is a
  viewing aid, not a fix — the content really is ~1e-3 while loading runs.
- `KL_GLFB_GAMMA=<g>` — override the tone map's encode exponent (default
  1/2.2). Lower brightens mid-tones.

## Networking (`runtime/kl_shim.c`)

The socket layer translates bionic→Darwin: `SOL_SOCKET` level + option numbers
in setsockopt/getsockopt, the `sa_len` byte and `AF_INET6` (10 vs 30) in every
sockaddr carrier, and the addrinfo list (same struct layout on both — only the
sockaddrs inside need converting). Without all three, Unity's Ping and the
gamelift region probes failed EINVAL in a retry storm.

- `KL_TRACE_NET=1` — log `socket`/`getaddrinfo`/`connect` with arguments and
  durations. Read by `kl_shim.c`.
- `KL_NET_OFFLINE=1` — getaddrinfo fails `EAI_NONAME` and connect fails
  `ENETUNREACH` immediately: a headset with no network. The guest takes its
  own offline path (curl reports "Could not resolve host", the GameLift
  region probe retries 4× per region and gives up) and the run completes.
  The abort this used to cause was *not* a DNS bug — it was the empty
  `dl_iterate_phdr` breaking the guest's unwinder, so the `SocketException`
  could never reach its handler. See CLAUDE.md trap 8.
- `KL_NO_DL_PHDR=1` — restore the old empty `dl_iterate_phdr`. Any guest
  throw then aborts in `_Unwind_RaiseException` instead of being caught;
  this is the A/B for trap 8.

## Loader diagnostics (`runtime/kl_shim.c`, `runtime/kl_libc.c`)

Built for the loading-pace investigation; all default off.

- `KL_TRACE_IO=1` — once a second: cumulative guest `fread`/`read` bytes,
  fread attributed per open file. This is how the "slow asset loading"
  hypothesis died: bulk reads stop within seconds; the only continuing trickle
  is libunity re-reading `/proc/cpuinfo` once per frame.
- `KL_TRACE_SLEEP=1` — once a second: usleep count/mean/max.
- `KL_TRACE_FUTEX=1` — once a second: futex waits split timeout-vs-woken, and
  wakes. The loading crawl came with zero timeouts — the wake path is healthy.
- `KL_USLEEP_CAP=<usec>` — clamp every guest usleep. Proved the ~5 ms polling
  loop is a watcher, not the loading pacer.
- `KL_X18_MAP=<file>` — append "veneer_addr site_addr" per patched x18 site,
  so a guest pc captured in a shim maps back to its call site.
- `KL_TRACE_IMAGES=1` — per-image base/span; needed to turn a guest pc into a
  file offset (`pc - image base`, e.g. the connect-NULL site at
  libunity+0x966964).

## Clock + condvar (bionic→Darwin translation, always on) and their diagnostics

These are fixes, not knobs: bionic clock ids differ from Darwin's, and bionic
condition variables default to CLOCK_MONOTONIC while Darwin conds speak only
CLOCK_REALTIME. Forwarding either verbatim poisons guest time:

- `klb_clock_gettime`/`klb_clock_getres` (`runtime/kl_libc.c`) translate clock
  ids (1/6→6, 4→4, 7/9→8, 2/3→12, 0/5/8→0). Before this, the guest's
  `clock_gettime(CLOCK_MONOTONIC=1)` failed EINVAL and libunity's time
  functions (libunity+0x883c58, +0x883c8c — no return-value check) computed
  with the unwritten buffer: stack-garbage "now", hours to days off.
- `klb_pthread_cond_timedwait` (`runtime/kl_pthread.c`) rebases
  monotonic-based abstimes (ts->tv_sec < 1e8) onto realtime before forwarding.
  Before this, libil2cpp's ConditionVariableImpl built abstimes from a
  monotonic tick source and Darwin compared them against realtime — class-init
  waiters slept until now+hours and never re-checked the class state (the
  loading-pace deadlock: Dns waiters parked on fully-initialised classes).

- `KL_TRACE_TIME=1` — log the first 40 `clock_gettime`/`gettimeofday` calls
  with caller address and result. How the unchecked-failure sites were found.
- `KL_TRACE_CONDWAIT=1` — log the first 60 guest `pthread_cond_timedwait`
  calls: tid, return address, ts and delta-from-now. A delta of hours is the
  signature of the poisoned abstime; sane runs read 1 ms – 1 s.
- `KL_CONDWAIT_CAP_MS=<ms>` — clamp every guest condvar abstime to now+ms.
  Diagnostic only (proves a stuck waiter just needs to re-check its
  predicate); the rebase above is the fix.

## Mutex map (`runtime/kl_pthread.c`) — deadlock instruments

Mutexes are keyed by guest *address* (a stale slot index in reused or
memcpy'd storage used to alias two logical mutexes onto one host object —
the capture-path deadlock). Owner tracking is always on; these print from
t_boot's fault handler on every fatal path:

- mutex owners: guest address, holder tid (+thread name, ** EXITED ** if the
  holder died holding it), lock return address, and the holder's current
  backtrace — a hang names the parked owner, not just the waiters.
- mutex waiters: who is blocked on which guest mutex and from where.

- `KL_TRACE_MUTEX=1` — log the first 200 mutex init/destroy events (guest
  address, slot/map index, tid, return address). How the slot-23 double-init
  was caught.

## Guest-thread sampler (`runtime/kl_sample.c`, `runtime/kl_il2cpp.c`)

Built to name the loop the loading-pace arc kept measuring: samples every
guest thread's pc and x29-chained backtrace with thread_get_state, resolves
pcs to module+offset (host frames to host symbols — a thread parked in
`__psynch_cvwait` reads as itself), and resolves libil2cpp generated-code pcs
to managed method names via global-metadata.dat plus the Il2CppCodeGenModule
table recovered from the loaded image (metadata v24.0 only, verified against
Beat Saber 1.28). Read by t_boot; sampling wraps the frame pump, and the
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

## Managed-side probe (`runtime/kl_mprobe.c`)

Calls Unity's **own C#** from the host, through the IL2CPP embedding API that
libil2cpp exports (domain → assembly → image → class → `MethodInfo` →
`il2cpp_runtime_invoke`). Every other instrument measures the native side of the
boundary; this one measures the far side, which is where a wrongly-encoded
status answer actually shows up. It is what found the `ovrpResult`-vs-`ovrpBool`
trap (CLAUDE.md trap 10) and named `MenuControllers` as the disabled object
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
- `KL_PROBE_TYPES=<a,b,...>` — types to census, as `Namespace.Type` (bare name
  for the global namespace); default is the menu pointer chain
  (`VRUIControls.VRPointer,…VRInputModule,…VRLaserPointer,…VRGraphicRaycaster`).
  Prints `active/total`: `FindObjectsOfType` sees only components on **active**
  objects, `Resources.FindObjectsOfTypeAll` includes inactive ones, and the gap
  between the two is the measurement — `VRLaserPointer=0/1` means the laser
  exists and is switched off.

**One caveat, recorded so it is not trusted.** The probe also prints
`eventsystem: overUI=...`, which stays `no` even when the pointer works:
`EventSystem.IsPointerOverGameObject()`'s zero-argument overload asks about
pointer id −1 (the mouse), while this title's `VRInputModule` uses its own id.
It is a blind detector, not evidence that the ray misses.

## XR runtime / controllers (`runtime/kl_ovrp.c`)

Poses live in the space the guest asked for with `ovrp_SetTrackingOriginType`.
Beat Saber asks for **FloorLevel**, so y=0 is the floor and a head belongs at
standing eye height — `kl_ovrp_tracking_origin()` reports which space is in
force, and a frontend that reports an eye-level head into a floor-level world
puts the camera on the ground with its hands underneath it.

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

## Viewer (`runtime/kl_view.c`, `tests/t_boot.c`)

- `KL_POKE_CAP=<n>` — at frame-pump start, overwrite libunity's texture-unit
  cap (the value its `SetTexture` path checks before logging "Invalid texture
  unit!"; read via the singleton at libunity+0x122e340, field +0xe8) with n.
  **Default is 64**, matching the 32-sampler ANGLE rebuild: Unity defaults
  the cap to 32 without ever querying GL for it, while its HLSLCC-baked
  sampler bindings reach unit 35 on the post passes. `KL_POKE_CAP_OFF=1`
  leaves the field alone.

- `KL_VIEW=1` — interactive one-eye viewer on SDL3; WASD+mouse-look drives
  the head pose ovrp reports. Requires `KL_GLFB=1`, runs the guest in-process
  on a spawned thread (no guard test, no re-exec), and pumps frames until the
  window closes instead of `KL_FRAMES` times.
  Controls: mouse-look aims, **mouse-L = right index trigger** (the menu
  click), mouse-R = grip, `Z`/`X` = A/B, `C`/`V` = X/Y, `G`/`H` = left
  trigger/grip, arrows = right thumbstick, WASD/`R`/`F` move.
- `KL_VIEW_AIM_AT_EYE=1` — put both hands at the head position. Beat Saber's
  menu pointer (`VRUIControls.VRGraphicRaycaster`) casts from the controller
  transform, and the default head-relative hands sit 0.25 m below your gaze,
  so the ray lands 0.25 m low at any panel distance. With this set the ray is
  the gaze ray and the viewport centre is a crosshair. Off by default: the
  offset is the honest emulation and is what puts the in-game controller
  models where a body would hold them.

## Audio

No environment variables. `kl_opensl.c` reads none.

## x18 (`runtime/kl_x18.c`)

- `KL_X18=0` — disable x18 veneering (also `n`/`N`, matched on the first
  character). The survey counts sites either way, so an A/B run compares like
  with like. Default on.

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
- `KL_TRACE_IMAGES=1` — log each loaded image's base and span, the stub pool
  mapping, and each emitted stub. Read by `kl_image.c`.

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

## Host environment pass-through

Not knobs — the shim reads these to answer the guest with host truth.

- `TMPDIR` — `kl_libc.c` builds the synthetic `/proc` tree under
  `$TMPDIR/klepton-proc.XXXXXX`, falling back to `/tmp/`.
- `LANG` — `kl_jni.c` derives `Locale.getDefault()`'s language and country
  from it (`en_US.UTF-8` becomes en/US). Empty, unset, or `C*` falls back to
  en/US.
