Param(
    [Parameter(Mandatory=$false)]
    [String] $logName = "log.log",

    [Parameter(Mandatory=$false)]
    [Switch] $help
)

if ($help -eq $true) {
    Write-Output "`"NDK-Stack`" - Processes a tombstone using the debug .so to find file locations"
    Write-Output "`n-- Arguments --`n"

    Write-Output "LogName `t`t The file name of the tombstone to process"

    exit
}

if (Test-Path "./ndkpath.txt") {
    $NDKPath = Get-Content ./ndkpath.txt
} else {
    $NDKPath = $ENV:ANDROID_NDK_HOME
}

$stackScript = "$NDKPath/ndk-stack"
if (-not ($PSVersionTable.PSEdition -eq "Core")) {
    $stackScript += ".cmd"
}

Get-Content $logName | & $stackScript -sym ./build/debug/ > "$($logName)_processed.log"
