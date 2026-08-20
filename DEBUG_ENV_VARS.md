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

## ANGLE reference renderer (`runtime/gfx/kl_glfb.c`)

All of these require `KL_GLFB=1` to mean anything; without it the null driver
answers GL and kl_glfb never initializes.

- `KL_GLFB=1` — enable the one-eye reference renderer on ANGLE/Metal.
  Host-only; it exists to produce the known-good frame a real backend gets
  diffed against.
- `KL_EGL_EXTENSIONS=<space-separated list>` — override what
  `eglQueryString(EGL_EXTENSIONS)` answers. The default names the three we
  actually implement (`EGL_KHR_image_base`,
  `EGL_ANDROID_image_native_buffer`, `EGL_ANDROID_get_native_client_buffer`);
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
- `KL_GLFB_ERRPROBE=1` — bracket draws and blits with `glGetError` and print
  the first 20 with the state around them (draw FBO, program, viewport;
  `glCheckFramebufferStatus` on 0x506). Consumes the pending error. Also
  enables the blit-state log line.
- `KL_GLFB_DRAW_PROBE=1` — after each of the first 12 scene-sized draws (32+
  vertices), read back the centre 64x64 and report lit-pixel count and mean
  luma. `KL_GLFB_DRAW_PROBE_N` overrides the quota (0 = unlimited),
  `KL_GLFB_DRAW_PROBE_MIN` lowers the vert floor (the frame's last draws are
  small, and one of them may be the one you're hunting).
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
- `KL_GUEST_PATCH_OFF=<name>[,<name>]` — turn off one by name. The names are
  `vrchat-under-construction`, `vrchat-min-client-version`, `vrchat-multipass`
  and `vrchat-fbo-context-guard`. `vrchat-min-client-version` is the one to
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

## Device identity (`runtime/kl_jni.c`)

- `KL_BUILD_<FIELD>=<value>` — override one `android/os/Build` (or
  `Build$VERSION`) String constant, e.g. `KL_BUILD_MODEL="Pixel 6"`,
  `KL_BUILD_MANUFACTURER=Google`. The default is a Quest 2 and that is a
  settled decision — this is the A/B, not a new answer.

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
  on someone's desktop. They are knobs because neither is measured yet — on
  device the real values are available and wiring them is the honest fix; a
  fixed answer at least does not fluctuate in the meantime.

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

- `KL_TRACE_MUTEX=1` — log the first 200 mutex init/destroy events (guest
  address, slot/map index, tid, return address). How the slot-23 double-init
  was caught.

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
- `KL_OVRP_STAGES=<n>` — eye-swapchain depth (default 3, clamped to
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
  rects, which would be unservable any other way.
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
  anything that suspects the API choice.
- `KL_UE4_WAIT=<seconds>` — the pump budget for a NativeActivity guest, default
  5. There is no frame count on this door: the guest owns its own game thread
  and its own frame loop, so seconds are the only unit. **RE4 needs 300** — its
  one-time shader optimization takes about a minute of host wall clock and the
  eye reads `91/86100 lit` while it runs, which is a real frame that looks like
  a broken pipeline.
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
- `KL_XR_PREDICT=0` — return the head pose as it stands now from the pose
  entry points instead of predicting it to the `displayTime` the guest named.
  The prediction is the frontend's (a timestamped device-anchor query, walked
  back 5 ms at a time until the tracker answers), capped 50 ms ahead, and it is
  computed ONCE per (frame, instant): `xrLocateSpace`, `xrLocateViews` and the
  layer placement in `xrEndFrame` all read the same one, because a guest that
  renders from one of them and submits from another otherwise draws from one
  head and declares another. Off restores exactly what this did before the seam
  existed, which makes it the A/B for any motion artefact that appeared with it.
  No host frontend publishes a tracker at all, so on host this knob does
  nothing and every pose is the latched head.
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

The CoreAudio output sink behind `kl_opensl.c`'s buffer queue (which itself
reads no knobs).

- `KL_AUDIO=0` — no CoreAudio device at all: the OpenSL feeder goes back to
  pacing each buffer with `usleep` and dropping it, which is what this runtime
  did for its whole life before `kl_audio.c`. The A/B for anything that looks
  like an audio-induced timing change — and read **by value** (`kl_env_on`),
  since it defaults on.
- `KL_AAUDIO_BURST=<frames>` — the frames-per-callback the AAudio surface
  (`runtime/media/kl_aaudio.c`) reports and asks the guest's data callback to fill.
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
- `KL_TRACE_IMAGES=1` — log each loaded image's base and span, the stub pool
  mapping, and each emitted stub. Read by `kl_image.c`.

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
  CONTROLLER pose after `KL_XR_GRIP_PITCH`. `_L`/`_R` per hand. Default 0,0,0.

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
- `KL_EXIT_ON_BACKGROUND=0` — stay alive when the app is backgrounded. The
  **default is to `exit(0)`**, ALVR's shape: everything the guest holds across a
  suspend (the ARKit session, the Compositor Services layer, ANGLE's context and
  the eye swapchain, FMOD's player, a Unity engine that does not expect the
  display to leave) is either unresumable or expensive to re-establish, so every
  launch is made a first launch. Set `=0` when the headset has to come off with a
  capture or a debugger still attached. Either way the phase change is logged
  (`[app] scene phase -> …`).
- `KL_SYNC_GUEST=1` — drive the guest inline on the compositor thread, i.e.
  P5b's shape before the guest moved to its own thread. The clock P5.4's device numbers were taken
  against, and the A/B for anything that looks like a pacing regression.
  Default is the guest on its own thread, one frame per published pose.
- `KL_FULL=1` — `.full` immersion. `.mixed` is the default — the guest's world
  is opaque so passthrough shows through nowhere it matters, and seeing the
  room makes scale and IPD easier to judge. The scene manifest declares
  `UIImmersionStyleMixed` to match; keep the two in step.
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
- `KL_CP_EYE=<0|1>` — composite ONLY that eye, leaving the other black. The
  binocular-vs-temporal split for a doubled image: one second, halves the
  search space.
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
