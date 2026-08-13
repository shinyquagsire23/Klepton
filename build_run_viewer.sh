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
make build/m_boot || exit 1
LOG="${KL_LOG:-/tmp/viewer.log}"
echo "[viewer] logging to $LOG"
KL_GLFB_ERRSCAN=1 KL_OVRP_HANDS_IN_VIEW=1 KL_NET_OFFLINE=1 KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_VIEW=1 KL_GLFB=1 KL_LIFECYCLE=1 \
  script -q /dev/null ./build/m_boot beatsaber/lib/arm64-v8a 2>&1 | tee "$LOG"
echo "[viewer] exit ${pipestatus[1]:-?} — report: LC_ALL=C grep -a -n 'fault:' $LOG"
#KL_VIEW=1 KL_GLFB=1 KL_GLFB_OUT=$(pwd)/fbo_dump KL_GLFB_DUMP_FBOS=1 KL_GLFB_OUT_EVERY=200 KL_LIFECYCLE=1 KL_FRAMES=2400 ./build/m_boot beatsaber/lib/arm64-v8a
#KL_DUMP_TEXTURES=tex_dump KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_FRAMES=20000 KL_LIFECYCLE=1 ./build/m_boot beatsaber/lib/arm64-v8a

#cd /Users/maxamillion/workspace/Klepton/visionos && KL_SKIP_STAGE=1 KL_FRAMES=30 timeout 900 ./run.sh device