# Klepton

Running Android ARM64 VR APKs on Apple Vision Pro.

Both platforms are aarch64, so there is no emulation here — guest code executes
natively and Klepton supplies the OS personality and ABI translation around it.
Closer to WINE than to QEMU.

```bash
make check     # full regression sweep
```

- **[CLAUDE.md](CLAUDE.md)** — orientation: current state, layout, and the traps.
- **[PLANNING.md](PLANNING.md)** — architecture, milestones, spike results, risks.

Status: guest ELF images load, relocate and execute; Unity's IL2CPP runtime
initialises with 87 managed assemblies. NDK/JNI, graphics and the XR runtime are next.
