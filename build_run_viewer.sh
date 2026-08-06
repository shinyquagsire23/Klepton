#!/bin/zsh
make build/t_boot
KL_GLFB_EXPOSURE=10.0 KL_GLFB_GAMMA=0.45 KL_POKE_CAP=64 KL_VIEW=1 KL_GLFB=1 KL_LIFECYCLE=1 ./build/t_boot beatsaber/lib/arm64-v8a
#KL_VIEW=1 KL_GLFB=1 KL_GLFB_OUT=$(pwd)/fbo_dump KL_GLFB_DUMP_FBOS=1 KL_GLFB_OUT_EVERY=200 KL_LIFECYCLE=1 KL_FRAMES=2400 ./build/t_boot beatsaber/lib/arm64-v8a