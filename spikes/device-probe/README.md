# Klepton on-device probe

Batches every device-only unknown into one self-contained run.

## Run it

```bash
./run_device.sh          # builds, signs, installs, launches, prints report
```

Vision Pro must be awake, unlocked, and paired. The report also renders on-screen
with a Share button. To re-run in the simulator instead:

```bash
./mkframeworks.sh && python3 gen_xcodeproj.py
xcodebuild -project KleptonProbe.xcodeproj -scheme KleptonProbe \
  -destination 'platform=visionOS Simulator,name=Apple Vision Pro' \
  -derivedDataPath build/dd CODE_SIGNING_ALLOWED=NO build
xcrun simctl install <UDID> build/dd/Build/Products/Debug-xrsimulator/KleptonProbe.app
xcrun simctl launch --console-pty <UDID> dev.klepton.probe
```

Team ID is auto-detected from your Apple Development cert; override with
`KLEPTON_TEAM=XXXXXXXXXX` or `KLEPTON_BUNDLE_ID=...` if signing fails.

## Probes

| # | Probe | Answers |
|---|---|---|
| P1 | page size, `pthread_mutex_t`/`sem_t` sizes | S0.4 — bionic struct-size divergence |
| P2 | `mrs tpidrro_el0` / `tpidr_el0` from EL0 | are system-register reads trapped? |
| P3 | Darwin TSD slot dump | is bionic's slot 5 (`STACK_GUARD`) free? |
| P4 | `TPIDR_EL0` volatility | does the macOS clobber finding hold? |
| P5 | `TPIDRRO_EL0`+slot 5 canary under preemption | **the klepton-ld TLS fix** |
| P6 | `mmap`/`mprotect` RWX, `MAP_JIT` | confirms the AOT premise |
| P7 | `dlopen` embedded frameworks (incl. 64KB segalign) | **S0.2 delivery mechanism** |
| P8 | `MTLDevice.makeLibrary(source:)` + timing | gates MoltenVK |
| P9 | slot 5 after Metal/ARKit/CompositorServices/RealityKit | S0.1 residual |
| P10 | 64 threads @ 512KB stacks | Unity thread-count sanity |
| P12 | `dlopen` + relocate + **run** a `klepton-ld`-emitted dylib | **M1b / port rung P3** — does AMFI accept a Mach-O no Apple linker touched? |

## Simulator results (rung 2, passed)

All green. Notably: 16KB pages; slot 5 free and *stays* free after all graphics
frameworks load; TLS fix 0 mismatches; **both frameworks dlopen fine, including the
64KB-segalign one**; runtime MSL compilation permitted.

P6 is the one to watch on device: the simulator reports `mprotect R+X` and
`MAP_JIT` as **allowed**, because simulator processes run on the macOS kernel with
no AMFI. On device these should be denied. If they aren't, that changes the
architecture — see PLANNING.md §1.

## Device results (rung 3)

Run on a physical Vision Pro (`RealityDevice17,1`), development-signed, with
`csops` reporting `CS_VALID/CS_HARD/CS_KILL/CS_ENFORCEMENT` all set and no
debugger attached.

- **P12 passed** — AMFI accepted the hand-emitted dylib, its text mapped `r-x`,
  all 1795 relocations matched the host's counts, and the guest opus roundtrip
  returned byte-identical results (389 bytes, 960 samples, energy 46696349).
  This is PLANNING §12.3(1), and it is what unblocks the rest of the port.
- **P6/P11: W^X is NOT enforced.** `mprotect R+X` and plain `mmap RWX` both map
  *and execute*, though `MAP_JIT` is denied — the opposite of the intuition. The
  named-stub pool therefore needs no redesign. Same development-signed caveat.
- P9: slot 5 still free after Metal, ARKit, CompositorServices and RealityKit.

P12 validates itself before it is believed: the identical code is run on the
host (`clang -I Sources probes.c` with a fake bundle) and must produce the same
relocation counts and the same roundtrip. A probe that walked a different set of
relocations would report a mismatch rather than a plausible pass.
