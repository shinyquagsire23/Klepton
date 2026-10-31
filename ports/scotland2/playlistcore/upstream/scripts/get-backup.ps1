Write-Output "Pulling playlists from Quest"

if (Test-Path Backup/Playlists) {
    Remove-Item -Recurse Backup/Playlists
}

New-Item -ItemType Directory -Path Backup/Playlists | Out-Null
adb pull /sdcard/ModData/com.beatgames.beatsaber/Mods/PlaylistManager/Playlists/ Backup/ | Out-Null
