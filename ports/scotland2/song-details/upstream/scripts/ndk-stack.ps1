if (Test-Path "./ndkpath.txt")
{
    $NDKPath = Get-Content ./ndkpath.txt
} else {
    $NDKPath = $ENV:ANDROID_NDK_HOME
}

$stackScript = "$NDKPath/ndk-stack"
if (-not ($PSVersionTable.PSEdition -eq "Core")) {
    $stackScript += ".cmd"
}

Get-Content ./log.log | & $stackScript -sym ./build/debug/ > log_unstripped.log