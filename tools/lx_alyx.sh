#!/bin/bash
# Run Half-Life: Alyx's Linux ARM64 launcher on the host through the Linux
# process runtime (runtime/linux/). Arguments are hlvr's own.
#
#   tools/lx_alyx.sh -novr -vulkan +map startup
#
# Expects, beside this checkout: ../alyx/game — Steam depots 546564 (binaries)
# and 546561 (content) merged into one tree — and ../steamrt/root/files (Steam
# Linux Runtime 4, arm64), translated with tools/lx_translate.sh into
# build/lx-alyx and build/lx.
#
# There is no window system: SDL is told to use its offscreen video driver, whose
# Vulkan surface is VK_EXT_headless_surface, and Klepton's Vulkan layer answers
# that with its own swapchain (KL_VK_WIDTH/KL_VK_HEIGHT size it; KL_VK_OUT=<dir>
# dumps presented frames as PNG, every KL_VK_OUT_EVERY-th — set that, a frame is
# half a megabyte).
#
# KL_LX_TRACE=1 traces syscalls; KL_LX_FAULT_STRINGS=1 prints the text on the
# stack when the guest dies, which is usually the reason it gave.
cd "$(dirname "$0")/.."
mkdir -p build/lx-tmp/xdg build/lx-home

# Two launch lines worth knowing:
#
#   tools/lx_alyx.sh -novr -vulkan +map startup
#       flat, into the offscreen window's swapchain (vk_*.png under KL_VK_OUT)
#   tools/lx_alyx.sh -vr -vulkan +hlvr_auto_dismiss_loading 1 +map startup
#       VR; the eyes the game submits are dumped as xr_f*_eye{0,1}.png
#
# hlvr_auto_dismiss_loading matters for an unattended run: after a load Alyx
# pauses behind its loading screen, with compositor rendering suspended, until
# the player pulls a trigger — and with no controller that is forever, which
# looks exactly like a game that submits frames nobody composites.
#
# VR. The game speaks OpenVR and finds its runtime the way it would find
# SteamVR: through ~/.config/openvr/openvrpaths.vrpath. The runtime named there
# is xrizer (OpenVR over OpenXR, built by ../guestbuild/build-xrizer.sh in the
# Linux VM), whose OpenXR is Klepton's own runtime, served as
# libopenxr_loader.so.1. Without the build this is simply absent and -vr fails
# the way it does with no headset software installed.
# LX_ALYX_MSAA=2|4 chooses the eye targets' sample count the only way the engine
# takes it: a fidelity table of its own, read in preference to the archive's
# (visionos/Sources/KleptonAlyx.swift has the story and writes the same file on
# the headset). LX_ALYX_SCALE=<s> makes every level that one scale, which is what
# the headset's table does: the engine allocates its targets for the LARGEST
# level in the table whichever one it draws. With neither, the file is removed
# and the game's own table applies: 2x at level 0, 4x above, scales 0.78 to 1.2.
LADDER=../alyx/game/hlvr/vr_fidelity_levels_override.csv
rm -f "$LADDER"
if [ -n "${LX_ALYX_MSAA:-}${LX_ALYX_SCALE:-}" ]; then
    { echo "hlvr,default,vr_msaa,vr_render_target_scale,min_gpu_memory,min_width,min_height,"
      n=0; for s in 0.78 0.78 0.84 0.9 0.95 1 1.08 1.2 1.2; do
          d=; [ $n = 5 ] && d=1
          echo "$n,$d,${LX_ALYX_MSAA:-4},${LX_ALYX_SCALE:-$s},,,,"; n=$((n + 1)); done
      echo "end,,,,,,,"; } > "$LADDER"
fi

XRIZER=../guestbuild/xrizer
VR_MOUNT=()
if [ -e "$XRIZER/bin/linuxarm64/vrclient.so" ]; then
    tools/lx_translate.sh build/lx-xrizer "$XRIZER/bin/linuxarm64"
    mkdir -p build/lx-home/.config/openvr
    cat > build/lx-home/.config/openvr/openvrpaths.vrpath <<'JSON'
{
  "config": ["/home/user/.local/share/Steam/config"],
  "external_drivers": null,
  "jsonid": "vrpathreg",
  "log": ["/home/user/.local/share/Steam/logs"],
  "runtime": ["/opt/xrizer"],
  "version": 1
}
JSON
    VR_MOUNT=(--mount-ro "/opt/xrizer=$XRIZER" --dylibs build/lx-xrizer)
fi
exec ./build/m_linux --mount /usr=../steamrt/root/files --mount /lib=../steamrt/root/files/lib \
     --mount /bin=../steamrt/root/files/bin --mount /etc=../steamrt/root/files/etc \
     --dylibs build/lx --dylibs build/lx-alyx \
     --mount /game=../alyx/game \
     --mount /tmp=build/lx-tmp --mount /home/user=build/lx-home "${VR_MOUNT[@]}" \
     --env LD_LIBRARY_PATH=/game/bin/linuxsteamrtarm64 --env HOME=/home/user --env USER=user --env XDG_RUNTIME_DIR=/tmp/xdg \
     --env SDL_VIDEODRIVER=offscreen --env SDL_AUDIODRIVER=${LX_ALYX_AUDIO:-dummy} --env SDL_AUDIO_DRIVER=${LX_ALYX_AUDIO:-dummy} \
     --env SDL_AUDIO_DISK_OUTPUT_FILE=/dev/klepton-audio --env SDL_AUDIO_DISK_TIMESCALE=0 $LX_ALYX_ENV \
     --cwd /game /game/bin/linuxsteamrtarm64/hlvr "$@"
