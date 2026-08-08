#!/bin/zsh
make build/m_boot
KL_GLFB_ERRSCAN=1 KL_OVRP_HANDS_IN_VIEW=1 KL_NET_OFFLINE=1 KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_VIEW=1 KL_GLFB=1 KL_LIFECYCLE=1 ./build/m_boot beatsaber/lib/arm64-v8a
#KL_VIEW=1 KL_GLFB=1 KL_GLFB_OUT=$(pwd)/fbo_dump KL_GLFB_DUMP_FBOS=1 KL_GLFB_OUT_EVERY=200 KL_LIFECYCLE=1 KL_FRAMES=2400 ./build/m_boot beatsaber/lib/arm64-v8a
#KL_DUMP_TEXTURES=tex_dump KL_TRACE_FS=1 KL_GLFB_EXPOSURE=1.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_FRAMES=20000 KL_LIFECYCLE=1 ./build/m_boot beatsaber/lib/arm64-v8a

#cd /Users/maxamillion/workspace/Klepton/visionos && KL_SKIP_STAGE=1 KL_FRAMES=30 timeout 900 ./run.sh device