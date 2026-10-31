Param(
    [Parameter(Mandatory = $false)]
    [Switch] $clean
)

& $PSScriptRoot/build.ps1 -clean:$clean

& adb push ./build/libsl2.so /sdcard/ModData/com.beatgames.beatsaber/Modloader

& adb shell am force-stop com.beatgames.beatsaber
& adb shell am start com.beatgames.beatsaber/com.unity3d.player.UnityPlayerActivity
