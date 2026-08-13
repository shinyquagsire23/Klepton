# Environment variables

Every environment variable the tooling reads — the C/Swift sources, the mains,
and the wrapper scripts — found by grepping for `getenv`/`kl_env_*`/
`ProcessInfo` and reading each usage site. Grouped by area. The source is the
authority; CLAUDE.md keeps no knob table of its own and points here. Unless an entry
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
- `KL_ALARM=<s>` — the watchdog around each lifecycle call (default 20s) and
  around the whole frame pump (default 60s). Widen it when the question is
  what the guest is waiting on.
- `KL_PERMISSIVE=1` — unimplemented JNI/GL/OVRPlugin/OVRPlatform calls return
  0 instead of aborting. Read by `kl_jni.c` (via `kl_jni_set_permissive`),
  `kl_egl.c`, `kl_ovrp.c`, `kl_ovrplat.c`. Scouting only — the guest carries
  on with answers we invented. Does not apply to the DRM entitlement guard.
- `KL_DYLIB_DIR=<dir>` — (`runtime/kl_image.c`) prefer `<dir>/<name>.dylib`
  (klepton-ld output) over the ELF when loading each guest library — how
  `make bootdylib*` runs the translated images. Unset, the runtime ELF loader
  maps the `.so` directly.

## GL / null driver (`runtime/kl_egl.c`)

- `KL_DUMP_SHADERS=<dir>` — after the frame pump, write every captured
  `glShaderSource` text into `<dir>`. Read by m_boot; the dump runs in
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
- `KL_EGL_TRACE=1` — log every EGL entry point the guest actually REACHES, in
  order, with the interesting arguments. A census, not a diagnostic: it says
  nothing about what we answered. It exists because every EGL symbol binds at
  LOAD time (libEGL.so is a DT_NEEDED of libunity), so "resolved" says nothing
  about "called" — and a guest that fails a graphics capability check *before*
  it touches EGL is otherwise indistinguishable from one whose call we answered
  wrongly. That is how BONELAB's renderer was identified: Beat Saber's per-API
  probe runs `eglChooseConfig` -> `eglCreateContext` (GLES 3) ->
  `eglDestroyContext`, and BONELAB's stops after `eglInitialize` because the
  only API in its list is Vulkan (`notes/BONELAB.md`).
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
  thread), because the swap itself does not. **Also on `xrEndFrame`** (SL-13):
  an OpenXR guest never calls `eglSwapBuffers` at all — measured 0 across a 45 s
  streaming run — so this knob, and both frontend seams with it, were silently
  inert on the whole VR path.
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
  On `build/m_slink`'s **VR** front door it does the same for the OpenXR
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
- `KL_GLFB_LOG_UNITS=1` — log texture-unit traffic (the "Invalid texture
  unit!" neighbourhood — see `KL_POKE_CAP` under Viewer).

## Networking (`runtime/kl_shim.c`)

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
  could never reach its handler. See trap 8 (CLAUDE.md's traps index; the
  full record is in `notes/TRAPS.md`).
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
- `KL_TRACE_SYSCONF=1` — every `sysconf()` the guest asks and the answer given.
  What to reach for when a guest sizes a thread pool or a heap to zero.
- `KL_TRACE_THREADS=1` — one line per guest `pthread_create`: entry, arg, stack
  size, detached, result. The entry pointer symbolises against the load
  addresses phase 1 prints, so it names which subsystem the thread belongs to.
- `KL_TRACE_FUTEX=1` — once a second: futex waits split timeout-vs-woken, and
  wakes. The loading crawl came with zero timeouts — the wake path is healthy.
- `KL_USLEEP_CAP=<usec>` — clamp every guest usleep. Proved the ~5 ms polling
  loop is a watcher, not the loading pacer.
- `KL_X18_MAP=<file>` — append "veneer_addr site_addr" per patched x18 site,
  so a guest pc captured in a shim maps back to its call site.
- `KL_TRACE_IMAGES=1` — per-image base/span; needed to turn a guest pc into a
  file offset (`pc - image base`, e.g. the connect-NULL site at
  libunity+0x966964).

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

- `KL_PLAT_CLOUD_DIR=<dir>` (`runtime/kl_ovrplat.c`) — where
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

## Device identity (`runtime/kl_jni.c`)

- `KL_BUILD_<FIELD>=<value>` — override one `android/os/Build` (or
  `Build$VERSION`) String constant, e.g. `KL_BUILD_MODEL="Pixel 6"`,
  `KL_BUILD_MANUFACTURER=Google`. The default is a Quest 2 and that is a
  settled decision (CLAUDE.md) — this is the A/B, not a new answer.

  The override is applied at the single source both readers go through, so
  `Build.MODEL` over JNI and `ro.product.model` through
  `__system_property_get` cannot disagree; making them disagree is the bug the
  `g_sysprops` mapping in `kl_libc.c` exists to prevent.

  It matters more than "Oculus branches": libshell's `BIsVRHeadset()` is
  literally `"<ro.product.manufacturer> <ro.product.model>"` matched against
  `Oculus Quest` / `Pico ` / `HTC VIVE `. (Measured: on `steamlink-vr.apk` it
  does *not* change the 2D→VR handoff — see notes/STEAMLINK.md SL-7.)

- `KL_BATTERY_CHARGING=1` / `KL_BATTERY_LEVEL=<0..100>` — what
  `BatteryManager.isCharging()` and `getIntProperty(BATTERY_PROPERTY_CAPACITY)`
  answer. Defaults: not charging, 95.

  These are **telemetry, not control**: Steam Link reads them through
  `IsHmdBatteryCharging()`/`GetHmdBatteryLevel()` and publishes them to the host
  as device properties, which the Steam client shows beside the headset. Nothing
  in the streaming path branches on them, so a wrong answer costs a wrong number
  on someone's desktop. They are knobs because neither is measured yet — on
  device the real values are available and wiring them is the honest fix; a
  fixed answer at least does not fluctuate in the meantime.

## Video decode (`runtime/kl_vtdec.c`, `runtime/kl_mediandk.c`)

The decoder is VideoToolbox and it is either there or it is not; what would
otherwise be a knob is a gate instead (`make hevc`, in `make check`). Worth
knowing rather than setting: output is **BGRA**, not the decoder's native NV12,
because the guest samples the frame through a `samplerExternalOES` whose whole
promise is that the YUV→RGB conversion has already happened. See
`runtime/kl_vtdec.h`.

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
- `KL_NO_REBASE=1` — disable the monotonic→realtime abstime rebase in
  `klb_pthread_cond_timedwait` entirely. Diagnostic for the capture deadlock;
  breaks class-init waiters.

## Mutex map (`runtime/kl_pthread.c`) — deadlock instruments

Mutexes are keyed by guest *address* (a stale slot index in reused or
memcpy'd storage used to alias two logical mutexes onto one host object —
the capture-path deadlock). Owner tracking is always on; these print from
m_boot's fault handler on every fatal path:

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

## Managed-side probe (`runtime/kl_mprobe.c`)

Calls Unity's **own C#** from the host, through the IL2CPP embedding API that
libil2cpp exports (domain → assembly → image → class → `MethodInfo` →
`il2cpp_runtime_invoke`). Every other instrument measures the native side of the
boundary; this one measures the far side, which is where a wrongly-encoded
status answer actually shows up. It is what found the `ovrpResult`-vs-`ovrpBool`
trap (trap 10 in CLAUDE.md's traps index / `notes/TRAPS.md`) and named
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
  stack, so it is a dead end there and `notes/BONELAB.md` says not to retry it.
  Independent of `KL_PROBE_INPUT`; runs once, at the first tick.
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
- `KL_OVRP_IPD=<m>` — force a symmetric head→eye separation, overriding
  whatever the frontend pushed. The guest's IPD arrives *only* through
  `ovrp_GetNodePoseState` nodes 0/1 (PLANNING §12.17), so this is the A/B for
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
- `KL_OVRP_STAGES=<n>` — eye-swapchain depth (default 3, clamped to
  1..max). `=1` restores the single-buffered behaviour every pre-§12.19
  measurement was taken against, and its tearing; each extra stage costs a
  full-size RGBA16F two-slice eye texture (~160 MB at map resolution). The
  A/B in both directions (PLANNING §12.19).
- `KL_OVRP_LATCH=0` — restore the live per-call pose read instead of latching
  head+hands once per frame at `ovrp_Update2` (the guest's real per-step latch
  point). The A/B if the pinning is ever suspected of costing latency;
  controller *buttons* stay live either way.
- `KL_OVRP_EYE_CANT=0` — ignore the per-eye rotation the frontend pushes
  (`kl_ovrp_set_eye_rotation`) and restore the dropped-cant behaviour as the
  A/B. Identity by default, so host and headless runs are unchanged.
- `KL_OVRP_QUEST_FOV=1` — keep the synthetic symmetric 90° frustum and the
  Quest 2's 72 Hz instead of the display's own measured numbers (the visionOS
  compositor's priming pass measures and logs both either way). A free A/B if
  the real numbers send Unity somewhere unexpected.
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
    the viewer.** `tests/t_mtl_provider.m` is compiled into `m_boot` and
    `m_slink` (it is a host stand-in for Compositor Services, not a test-only
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
- `KL_VRR_ZONES=<n>` — how many equal screen-space zones each axis of that
  synthetic curve is divided into, default 16, capped at 64. Also the unwarp
  grid's cell count, which is what makes the unwarp exact rather than a sampling
  of the curve (`kl_reproject.h`). Same "ignored on device when the display's
  curve was sampled" caveat — there the count comes from the drawable map's
  physical granularity.
- `KL_OVRP_EYE_SCALE=<x>` — scale the per-eye render target size the guest is
  told to use (`ovrp_GetEyeTextureSize`), default 1.0, accepted in 0.05..4.0.
  Applies to whatever the frontend measured off the display
  (`kl_ovrp_set_eye_texture_size`); with nothing pushed the size stays the
  Quest 2's 2290x2400 and this does nothing. The cheapest resolution knob
  there is — Unity then applies its **own** ~1.2x on top of the result.
- `KL_OVRP_EYE_MAX=<px>` — cap on the longer axis of that size, default 3072,
  0 to disable. Vision Pro's logical per-eye resolution is far larger than a
  Quest's, and the guest allocates stages x eyes of RGBA16F at whatever it is
  told, so an uncapped number turns straight into hundreds of MiB and into fill
  cost. The aspect ratio is preserved when it clamps, because the frustum
  tangents pushed alongside would otherwise disagree with the picture's shape.
  Every change logs the requested size, the applied size and the resulting
  swapchain MiB.
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
  **On the macOS viewer this needs `KL_VIEW_TIMEWARP=1`**: the default viewer
  path is the plain blit, which has no unwarp grid and therefore no crop.

### Haptics (`runtime/kl_ovrp.c` — the seam that runs OUT of the guest)

The guest queues an amplitude envelope through OVRPlugin's buffered haptics API
(320 Hz, one byte a sample); `kl_ovrp_haptics_pull` is an **envelope follower**
that reports what came due since the frontend last asked. Platform-independent
— the visionOS playback knobs are in the visionOS section.

- `KL_HAPTICS_MIN_MS=<ms>` — how long a level is held after its samples run
  out, default 32. ALVR's number and ALVR's reason: *"controllers can't do 10ms
  vibrations"*. It is a floor on the DRIVE, not on how long we wait before
  reporting one — waiting was the first design and it lost note cuts entirely
  (PLANNING §12.20). The same knob sets the floor on a discrete pulse's
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
  headset runs at, 30..240. It defaults to the Quest 2's 72, which is the
  device we describe everywhere else, and on the host there is no panel to
  measure; on visionOS the compositor's priming pass pushes the real number.
  It matters beyond pacing for Steam Link: the VR client publishes this list to
  the Steam host as `VTE_AVAILABLE_FRAMETIMES_US`, the host asks for a rate,
  and the negotiation is visible in the guest's own log
  (`Server requested refresh rate 90.0 was not available. Using 72.0`).
- `KL_XR_CAPTURE_LAYER=N` — which projection layer `KL_GLFB_OUT` reads (default
  0, `runtime/kl_openxr.c`). Steam Link's VR client submits **four** projection
  layers a frame — two 1536x1536 pairs for its panels and two 2290x2400 pairs
  for the eyes — and nothing in the submission says which holds what, so this
  moves the capture without a rebuild. That matters because a streaming run
  costs a fresh Steam pairing to repeat. The `[xr] layer N eye M <- swapchain`
  lines say what each one is, and mark the captured one.
- `KL_XR_REFRESH_EXT=0` — stop advertising `XR_FB_display_refresh_rate`
  (`runtime/kl_openxr.c`), putting the runtime back to before SL-11. The A/B
  for anything that changes when the client can answer the host's rate
  question at all: without the extension the client publishes an EMPTY rate
  list, and a host told the client can present at no rate never starts sending
  video.

## Reprojection (`runtime/kl_reproject.c`)

The composite/timewarp pass — one file, compiled by both compositors
(`KleptonCompositor.swift` on device, `kl_view_mtl.m` in the viewer).

- `KL_REPROJECT_DEPTH=<m>` — how far out the reprojection quad is placed
  (default 2.0 m). It must stay well clear of reverse-Z 0 or **visionOS
  discards the frame** — 500 m is depth 0.0002 and is invisible (PLANNING
  §12.16). Within that it is free, and it is one of the two knobs that set
  apparent scale.
- `KL_REPROJECT_MODE=off|inverse` — the bisection the pass never had. `off`
  corrects nothing (the delta is dropped and the pass becomes the frustum fit
  alone — if instability survives this, the timewarp is not causing it);
  `inverse` applies the delta backwards (if THIS is the stable one, a sign is
  wrong upstream and the question becomes which input). Both are wrong
  pictures by construction; diagnostic only.
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

## Viewer (`runtime/kl_view.c`, `mains/m_boot.c`)

- `KL_POKE_CAP=<n>` — at frame-pump start, overwrite libunity's texture-unit
  cap (the value its `SetTexture` path checks before logging "Invalid texture
  unit!"; read via the singleton at libunity+0x122e340, field +0xe8) with n.
  **Default is 64**, matching the 32-sampler ANGLE rebuild: Unity defaults
  the cap to 32 without ever querying GL for it, while its HLSLCC-baked
  sampler bindings reach unit 35 on the post passes. `KL_POKE_CAP_OFF=1`
  leaves the field alone.

  **Both offsets were measured against Unity 2019.4 and apply to no other
  build**, so the poke is now GATED on the version stamp `m_boot` reads out of
  the mapped libunity (`unity_version()`). On anything else it skips and says
  so by name. Setting `KL_POKE_CAP=<n>` explicitly ALSO forces it past that
  gate — do that only with re-measured offsets, since the write is a 4-byte
  store through whatever the stale offset happens to point at. Beat Saber
  1.6.0 (Unity 2018.4.4f1) found this the expensive way: the unguarded read
  took the lifecycle down with a SIGSEGV inside `recon_run`, which reads like a
  shim bug rather than an expired constant. **Marked for deletion** — see the
  TODO on `poke_texture_unit_cap()` in `mains/m_boot.c` for the two fixes that
  would retire it, neither of which needs an offset into anything.

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
  `runtime/kl_mono.c` and is driven from `kl_slink_sdl_pump`, i.e. the Android
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
  compositor shows the wrong picture.
- `KL_VIEW_TIMEWARP=1` — composite the viewer's frame through the reprojection
  pass, against the pose the guest *actually rendered it with* (`kl_ovrp`'s
  stage-keyed record) instead of the current one — mouse-look motion between
  the guest's frame and the composite is corrected here exactly as head motion
  is on device. Default off: the plain blit is the path that reached gameplay,
  and it is the A/B. Prints the delta in degrees every 120 composites — 0.00
  means the guest is keeping up and the pass is a blit, which is a proven
  identity (`make reproject`).
  **Being off is why the viewer is not a stand-in for the device composite.**
  The visionOS compositor has no blit path — it is always the reprojection pass
  — so everything that lives only in that pass (the unwarp grid, the foveation
  correction, the render-viewport crop, `visible`, the sRGB decode) is
  unexercised by a default viewer run. "It looks right in the viewer" is
  therefore not evidence about any of them; turn this on before comparing the
  two, and the picture the viewer shows is then the picture the device's pass
  computes.

## Audio (`runtime/kl_audio.c`)

The CoreAudio output sink behind `kl_opensl.c`'s buffer queue (which itself
reads no knobs). See PLANNING §12.18.

- `KL_AUDIO=0` — no CoreAudio device at all: the OpenSL feeder goes back to
  pacing each buffer with `usleep` and dropping it, which is what this runtime
  did for its whole life before `kl_audio.c`. The A/B for anything that looks
  like an audio-induced timing change — and read **by value** (`kl_env_on`),
  since it defaults on.
- `KL_AAUDIO_BURST=<frames>` — the frames-per-callback the AAudio surface
  (`runtime/kl_aaudio.c`) reports and asks the guest's data callback to fill.
  Default 240, which is 5 ms at 48 kHz and the unit Steam Link's own jitter
  buffer is configured in multiples of. Clamped to 32..8192. AAudio is a *pull*
  API, unlike OpenSL's buffer queue, so this is the size of every call INTO the
  guest; the device is still the clock, because the feeder paces on
  `kl_audio_write`. There is no knob for the input direction: capture streams
  are refused by design (no microphone is presented — see `kl_aaudio.h`), which
  the guest logs as `AAUDIO_ERROR_UNAVAILABLE` and carries on from.
- `KL_AUDIO_DUMP=<path>` — tee exactly the frames the render callback will
  hand the hardware into a WAV. The only way to check *what* is being played
  rather than merely that something is: counts and peak levels cannot tell
  music from a wrong resample ratio or swapped channels. Records what the
  producer accepted, contiguously, so pauses are elided and the file is
  shorter than the run.
- `KL_AUDIO_LATENCY_MS=<n>` — how much audio the ring aims to hold (default
  80, clamped 10..500). This is the whole latency budget — the ring itself is
  a second long so a hiccup cannot wrap it, but the *fill target* is what the
  producer blocks against. Raised automatically if FMOD's buffer turns out
  larger than half of it.
- `KL_AUDIO_TRACE=1` — one line every 200 writes: input/output frame counts,
  ring fill in frames and ms, underruns. What to turn on when the sound is
  present but wrong.
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

## x18 (`runtime/kl_x18.c`)

- `KL_X18=0` — disable x18 veneering (also `n`/`N`, matched on the first
  character). The survey counts sites either way, so an A/B run compares like
  with like. Default on. This is the A/B that identified the veneer pass as
  the cause of Steam Link's failing TLS handshake: with it off the handshake
  completed, with it on the guest answered `bad_record_mac`.
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
- `KL_TRACE_IMAGES=1` — log each loaded image's base and span, the stub pool
  mapping, and each emitted stub. Read by `kl_image.c`.

## Steam Link (`mains/m_slink.c`, `build_run_slink.sh`)

The wrapper's flags map onto these: `--gap` → `KL_GAP_ONLY=1 KL_NOFORK=1`,
`--main` → `KL_SLINK_MAIN=1 KL_GLFB=1 KL_NOFORK=1`, `--view` adds `KL_VIEW=1`.
See PLANNING §11.

- `KL_SLINK_MAIN=1` — run phase 4 at all (onCreate → `nativeRunMain` →
  `SDL_main` on its own thread). SL-1 (chain binds, `JNI_OnLoad`) stays the
  unconditional gate; `KL_VIEW=1` implies this.
- `KL_SLINK_SHELL=1` — open the OTHER front door: the 2D **configuration
  frontend** (`libshell_arm64-v8a.so` -> `main`, Qt6) instead of the streaming
  client (`libmain.so` -> `SDL_main`). VR APK only — the old one ships Qt5 with
  the stock `qtforandroid` QPA, the VR one ships Qt6 with Valve's own `qvirtual`,
  which imports no JNI at all. `./build_run_slink.sh --shell` sets it and picks
  the tree. The shell draws its own UI with no Steam host on the network; the
  client draws nothing at all without one.
- `KL_SLINK_VR=1` — open the THIRD front door: `libvrlink_scene.so`, the
  **OpenXR NativeActivity** (`ANativeActivity_onCreate`), instead of either SDL3
  half. Not a chain — its `DT_NEEDED` is entirely Android system libraries we
  shim, so one guest library is the whole working set, and `libopenxr_loader.so`
  is deliberately NOT loaded because it is replaced by `runtime/kl_openxr.c`.
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
  own (PLANNING §11.14).
- `KL_SLINK_ARGS="<space-separated argv>"` — `SDL_main`'s own options, which
  the real activity fills from the launching intent's `sArgs` extra. Without
  it the streaming client is being asked to stream nothing (PLANNING §11.12).
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
  shell building after the host authorized (notes/STEAMLINK.md); a synthetic
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
  after all that is trap 19, not a bad token — see notes/TRAPS.md.
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
- `KL_XR_GRIP_PITCH=<degrees>` — rotate the CONTROLLER pose about its X axis,
  positive tilting forward up. **Default +35, confirmed by eye on a headset
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
  about the same axis (-20.6 Touch, -10 Pico, -5 Vive), which is why -35 was
  tried first and was wrong by twice the angle. Those are the guest's
  grip-to-*device* offsets, applied on its side to a pose it already has; this
  is the correction from the frontend's hilt frame *into* the grip pose the
  guest expects, and the two run opposite ways. A plausible source that gives
  the wrong answer — do not re-derive from it. A controller off by twice the
  angle rather than merely still wrong is the tell for a sign error.
- `KL_XR_AIM_PITCH=<degrees>` — the EXTRA offset between the aim ray and the
  grip, applied only to `.../input/aim/pose`. **Default 0**: the real aim-vs-grip
  angle of this input source has not been measured, and the frontend's hilt
  frame already points roughly where a hand points. Separate from the knob above
  because they answer different questions, and conflating them is the bug that
  produced a knob which did nothing.

  Both are read once and printed together at the first `xrCreateActionSpace`
  (`[xr] controller pose: grip pitched -35.0 deg …`) — **not** lazily when a
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

## Vulkan (`runtime/kl_vulkan.c`) — the synthetic libvulkan.so

BONELAB's graphics API (`notes/BONELAB.md`). Needs `make mvk`; with no MoltenVK
vendored the whole path refuses by name and none of these do anything.

- `KL_VK_OUT=<dir>` — write each submitted frame there as a PNG. On this guest
  that is the OVRPlugin eye layer, one file per eye
  (`vk_f<frame>_s<stage>_eye<n>.png`), written at `ovrp_EndFrame4`; on a flat
  Vulkan guest it is the swapchain image at `vkQueuePresentKHR`. Unset means no
  capture and no readback cost.
- `KL_VK_OUT_EVERY=N` — capture every Nth frame (default 1). A full eye pair is
  ~21 MB of readback, so 1 is only for short runs.
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

## MoltenVK vendoring gate (`tests/t_mvk.c`, `make mvk-check`)

The Vulkan side of BONELAB (`notes/BONELAB.md`). Not part of the runtime yet —
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
except its own control vars (next section). See PLANNING §12.

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
  required there for the reason SL-12 records (see `notes/VISIONOS.md`, "The eye
  geometry has ONE source").
- `KL_AUTOBOOT=0` — autoboot is the default too; `=0` restores the
  Boot-button-only shape, for attaching a debugger or starting a GPU capture
  before the guest runs.
- `KL_EXIT_ON_BACKGROUND=0` — stay alive when the app is backgrounded. The
  **default is to `exit(0)`**, ALVR's shape: everything the guest holds across a
  suspend (the ARKit session, the Compositor Services layer, ANGLE's context and
  the eye swapchain, FMOD's player, a Unity engine that does not expect the
  display to leave) is either unresumable or expensive to re-establish, so every
  launch is made a first launch. Set `=0` when the headset has to come off with a
  capture or a debugger still attached. Either way the phase change is logged
  (`[app] scene phase -> …`).
- `KL_SYNC_GUEST=1` — drive the guest inline on the compositor thread, i.e.
  P5b's shape before §12.12. The clock P5.4's device numbers were taken
  against, and the A/B for anything that looks like a pacing regression.
  Default is the guest on its own thread, one frame per published pose.
- `KL_FULL=1` — `.full` immersion. `.mixed` is the default — the guest's world
  is opaque so passthrough shows through nowhere it matters, and seeing the
  room makes scale and IPD easier to judge. The scene manifest declares
  `UIImmersionStyleMixed` to match; keep the two in step.
- `KL_OVERLAYS=1` — put the system's persistent overlays back. Both immersive
  spaces pass `.persistentSystemOverlays(.hidden)` by default: the Home
  indicator and the hand-gesture affordance beneath it are drawn by the system
  *over* the guest, and both guests put interactive content exactly where it
  lands — Beat Saber's lower menu row, Steam Link's dashboard toolbar. It also
  reappears on every hand raise, which for a hand-driven title is continuous.
  `.hidden` is a request rather than a guarantee (the system still shows the
  indicator when it considers it mandatory), so turn this on when the question
  is whether the system still thinks our space is on screen at all.
- `KL_TEMPLATE=1` — the **floor test**: Apple's `MetalImmersiveTemplate` with a
  blue fragment shader in its own immersive space, sharing nothing with the
  compositor (`KleptonTemplate.swift`). Runs INSTEAD of booting the guest —
  the known-good pixel on that display; reach for it before doubting
  `KleptonCompositor`. `KL_TPL_MIXED=1` puts it in `.mixed`, `KL_TPL_NOMSAA=1`
  renders straight into the drawable instead of resolving.
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
- `KL_SENSE_PITCH=<deg>` — just the X term of that rotation, which is the one
  that has ever needed changing. Default **-35**: the magnitude is Beat Saber's
  own in-game controller adjustment, the sign is device-measured (+35 pitched
  the hilts backward — the game applies its adjustment in Unity's left-handed
  frame). It stacks with ALVR's +5.037° PSVR2 model tilt, so the default pair is
  `-29.963,0,0` and `0.002,0,-0.01`. If a playtest leaves about five degrees
  forward, try `KL_SENSE_PITCH=-45.037` (= -40.04 total, Beat Saber's own
  Oculus Touch constant).
- `KL_SENSE_VEL_FRAME=world` — read `AccessoryAnchor.velocity`/`angularVelocity`
  as already being in tracking space. Default treats them as accessory-local
  and rotates them by the grip orientation, which is what ALVR does; Apple
  documents neither, and the two differ only in direction.
- `KL_HAND_MIRROR=1` — left hand back on ALVR's *mirrored* wrist→grip
  constant. The default left constant is `R · Rx(180)`, forced by four
  playtests (PLANNING §12.17c).
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
- `KL_PINCH_TRACE=0` — silence the pinch trace, which is **on by default**
  during bring-up: one line every 2 s per hand — closest pinch reached, axis
  value produced, frames asserted, or `no skeleton`. Those four distinguish
  the four ways "the trigger does not click" can be true.
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
  frame — which is what a note cut felt like on device before §12.20. A hand
  also falls back to this on its own if the continuous player cannot be made
  or started, and says so in the log.
- `KL_CP_PROBE=<n>` — bisection ladder for a dark compositor: 1 = clear only
  (colour cycles R/G/B so a constant field cannot be misread), 2 = flat
  magenta quad (geometry only), 3 = sampled with alpha forced to 1, 4 =
  full-viewport blit. `KL_CP_NOFENCE=1` skips the guest fence wait; the
  `alive:` line reports `cmdbuf done/committed` so "committed but never
  executed" is visible.
- `KL_CP_EYE=<0|1>` — composite ONLY that eye, leaving the other black. The
  binocular-vs-temporal split for a doubled image: one second, halves the
  search space (PLANNING §12.19).
- `KL_CP_AMPLIFY=0` — one render pass per eye instead of the single
  vertex-amplified composite. Takes foveation down with it (the rate map
  needs the amplified pass).
- `KL_CP_FORMAT=16` — drawable back to `rgba16Float`; the default is 8-bit
  `bgra8Unorm_srgb` (ALVR's choice — half the composite bandwidth, hardware
  sRGB encode on write). The guest's eye textures stay RGBA16F either way.
- `KL_CP_FOVEATION=0` — no variable-rasterization-rate map and no
  `maxRenderQuality` ceiling (the quality knob is only accepted alongside
  foveation).
- `KL_CP_QUALITY=<0..1>` — the render quality to request (default 1.0). The
  ceiling costs memory and per-frame GPU time; the *running* quality is set
  separately by the render loop.
- `KL_CP_LOSSY=1` — lossy texture compression on the drawable-adjacent
  textures; an experiment, default off.

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
- `KL_LOG_OUT=<file>` — where the pulled log lands (device runs default
  `/tmp/klepton-device.log`; `build_run_slink.sh` uses it too, default
  `/tmp/slink.log`).
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
