Commands for fomplete fonctionnal beatsaber v1.40.8 on apple vision pro : 
```bash
brew install pkg-config sdl3 apktool
apktool d -f -o beatsaber beatsaber.apk
make mvk
make check
make angle-all
make xros
```
put `main.1716.com.beatgames.beatsaber.obb' to '~/Library/Application Support/Klepton/userdata/<target>/beatsaber/obb/'
```bash
./build_run_viewer.sh beatsaber
```
1. delete `~/Library/Application Support/Klepton/userdata/<target>/beatsaber/files/PlayerData.dat`

2. rename`~/Library/Application Support/Klepton/userdata/<target>/beatsaber/files/PlayerData.dat.tmp` to `~/Library/Application Support/Klepton/userdata/<target>/beatsaber/files/PlayerData.dat`
```bash
./build_run_vpro.sh  beatsaber # -> return an error in AVP
KLEPTON_TARGET=beatsaber KLT_STAGE_FILES="files" visionos/stage_assets.sh <target>
./build_run_vpro.sh  beatsaber
```
