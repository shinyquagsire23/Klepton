Param(
    [Parameter(Mandatory=$false)]
    [String] $fileName = "RecentCrash.log",

    [Parameter(Mandatory=$false)]
    [Switch] $analyze,

    [Parameter(Mandatory=$false)]
    [Switch] $help
)

if ($help -eq $true) {
    Write-Output "`"Pull-Tombstone`" - Finds and pulls the most recent tombstone from your quest, optionally analyzing it with ndk-stack"
    Write-Output "`n-- Arguments --`n"

    Write-Output "-FileName `t The name for the output file, defaulting to RecentCrash.log"
    Write-Output "-Analyze `t Runs ndk-stack on the file after pulling"

    exit
}

$global:currentDate = get-date
$global:recentDate = $Null
$global:recentTombstone = $Null

for ($i = 0; $i -lt 3; $i++) {
    $stats = & adb shell stat /sdcard/Android/data/com.beatgames.beatsaber/files/tombstone_0$i
    $date = (Select-String -Input $stats -Pattern "(?<=Modify: )\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}(?=.\d{9})").Matches.Value
    if([string]::IsNullOrEmpty($date)) {
        Write-Output "Failed to pull tombstone, exiting..."
        exit 1;
    }
    $dateObj = [datetime]::ParseExact($date, "yyyy-MM-dd HH:mm:ss", $Null)
    $difference = [math]::Round(($currentDate - $dateObj).TotalMinutes)
    if ($difference -eq 1) {
        Write-Output "Found tombstone_0$i $difference minute ago"
    } else {
        Write-Output "Found tombstone_0$i $difference minutes ago"
    }
    if (-not $recentDate -or $recentDate -lt $dateObj) {
        $recentDate = $dateObj
        $recentTombstone = $i
    }
}

Write-Output "Latest tombstone was tombstone_0$recentTombstone"

& adb pull /sdcard/Android/data/com.beatgames.beatsaber/files/tombstone_0$recentTombstone $fileName

if ($analyze) {
    & $PSScriptRoot/ndk-stack.ps1 -logName:$fileName
}
