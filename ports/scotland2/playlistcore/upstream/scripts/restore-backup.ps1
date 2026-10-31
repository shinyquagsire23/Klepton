Write-Output "Pushing playlist backups to Quest"

$dir = adb shell ls /sdcard/ModData/com.beatgames.beatsaber/Mods/PlaylistManager/

if ($dir.Contains("Playlists")) {
    adb shell rm -r /sdcard/ModData/com.beatgames.beatsaber/Mods/PlaylistManager/Playlists/ | Out-Null
}

adb push Backup/Playlists/. /sdcard/ModData/com.beatgames.beatsaber/Mods/PlaylistManager/Playlists | Out-Null
