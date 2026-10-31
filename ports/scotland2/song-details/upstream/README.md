# SongDetails for quest
This repo contains a port of SongDetails for use on oculus quest with BeatSaber mods.

# Build tips
When cloning the repo make sure to clone with submodules, as a submodule is used for the gzip decompression

## Requirements
 - QPM-Rust
 - VCPKG
 - Android NDK

I'm not going to give all the build instructions, since most are the same for beat saber mods, you just have to make sure you have a vcpkg install and that you run

```shell
vcpkg install --triplet arm64-android
```

Then you should be able to update the VCPKG path in the CMakeLists.txt file so that it can find the protobuf package, this is obviously not the most neat way of doing this but it's what worked for now till I improve it