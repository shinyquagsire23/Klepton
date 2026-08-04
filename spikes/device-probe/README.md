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

## Simulator results (rung 2, passed)

All green. Notably: 16KB pages; slot 5 free and *stays* free after all graphics
frameworks load; TLS fix 0 mismatches; **both frameworks dlopen fine, including the
64KB-segalign one**; runtime MSL compilation permitted.

P6 is the one to watch on device: the simulator reports `mprotect R+X` and
`MAP_JIT` as **allowed**, because simulator processes run on the macOS kernel with
no AMFI. On device these should be denied. If they aren't, that changes the
architecture — see PLANNING.md §1.
