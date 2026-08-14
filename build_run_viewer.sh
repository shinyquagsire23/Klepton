#!/bin/zsh
# The interactive loop. Its output is now KEPT — ${KL_LOG:-/tmp/viewer.log} — as
# well as shown, because this is the one run in the project that only ever
# existed in a terminal's scrollback, and it is also the only one that can reach
# the parts of the game a pointer drives. A crash here used to leave nothing but
# an OS .ips, which names the faulting pc and NOT the frame chain, the guest
# image, the managed method, or any of the subsystem reports kl_fault.c prints.
#
# Through `script` and not a bare pipe: with stdout a pipe, stdio buffers in
# full blocks, and a death by SIGNAL loses the buffer — which is exactly the
# report you wanted (CLAUDE.md, "Operating the M4 loop"). A pty forces line
# buffering. The SDL window and the mouse are unaffected; only stdio moves.
#
# Which guest, as the first argument, defaulting to beatsaber —
# `./build_run_viewer.sh superhot`. The name goes straight to m_boot, which
# resolves it through the same target table the visionOS build reads, so the
# libraries, the APK, the assets and the userdata directory move together.
# The log is named after it for the same reason build_run_vpro.sh's is: two
# targets writing one path means the second run's evidence overwrites the first.
make build/m_boot || exit 1
TARGET="${1:-beatsaber}"
LOG="${KL_LOG:-/tmp/viewer-$TARGET.log}"
echo "[viewer] $TARGET, logging to $LOG"
# KL_POKE_CAP is deliberately NOT set here, and that is a fix rather than an
# omission. Setting it means "the offsets have been re-measured for this Unity
# version" — poke_texture_unit_cap falls back to the FIRST row's offsets for an
# unlisted version when it is set, and then dereferences whatever it reads out
# of libunity at Beat Saber's offset. This script sets it for EVERY target, so
# it turned that deliberate override into a blind default: on VRChat (Unity
# 2022, unlisted) the viewer died in a wild dereference before its first frame,
# with the fault inside recon_run — i.e. reading as a bug in the pump. Left
# unset, the version table does its own job: Beat Saber still gets its measured
# row and the same poke of 64, and an unlisted version SKIPS and says so.
# Pass KL_POKE_CAP=<n> in the environment to force it, which is what it is for.
KL_GLFB_ERRSCAN=1 KL_OVRP_HANDS_IN_VIEW=1 KL_NET_OFFLINE=0 KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_VIEW=1 KL_GLFB=1 KL_LIFECYCLE=1 \
  script -q /dev/null ./build/m_boot "$TARGET" 2>&1 | tee "$LOG"
echo "[viewer] exit ${pipestatus[1]:-?} — report: LC_ALL=C grep -a -n 'fault:' $LOG"
#KL_VIEW=1 KL_GLFB=1 KL_GLFB_OUT=$(pwd)/fbo_dump KL_GLFB_DUMP_FBOS=1 KL_GLFB_OUT_EVERY=200 KL_LIFECYCLE=1 KL_FRAMES=2400 ./build/m_boot beatsaber/lib/arm64-v8a
#KL_DUMP_TEXTURES=tex_dump KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_FRAMES=20000 KL_LIFECYCLE=1 ./build/m_boot beatsaber/lib/arm64-v8a

#cd /Users/maxamillion/workspace/Klepton/visionos && KL_SKIP_STAGE=1 KL_FRAMES=30 timeout 900 ./run.sh device