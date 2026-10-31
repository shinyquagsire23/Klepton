param (
    [Parameter(Mandatory=$false)]
    [Switch]$useDebug,
    [Parameter(Mandatory=$false)]
    [Switch]$log
)

& ./build.ps1
if ($useDebug.IsPresent) {
    & adb push build/debug/libsongdownloader.so /sdcard/ModData/com.beatgames.beatsaber/Modloader/mods/libsongdownloader.so
} else {
    & adb push build/libsongdownloader.so /sdcard/ModData/com.beatgames.beatsaber/Modloader/mods/libsongdownloader.so
}

& adb shell am force-stop com.beatgames.beatsaber
& adb shell am start com.beatgames.beatsaber/com.unity3d.player.UnityPlayerActivity
if ($log.IsPresent) {
    & ./log.ps1
}