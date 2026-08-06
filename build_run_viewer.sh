#!/bin/zsh
make build/t_boot
KL_VIEW=1 KL_GLFB=1 KL_LIFECYCLE=1 ./build/t_boot beatsaber/lib/arm64-v8a