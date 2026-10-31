
function Clean-Build-Folder {
    if (Test-Path -Path "build") {
        remove-item build -R
        new-item -Path build -ItemType Directory
    }
    else {
        new-item -Path build -ItemType Directory
    }
}

$NDKPath = Get-Content $PSScriptRoot/ndkpath.txt

# Clean-Build-Folder
# Build flamingo local Android test
& cmake -G "Ninja" -DCMAKE_BUILD_TYPE="Debug" -DTEST_BUILD=0 -DTEST_ON_ANDROID=1 -B build
& cmake --build ./build

$ExitCode = $LastExitCode

if (-not ($ExitCode -eq 0)) {
    $msg = "ExitCode: " + $ExitCode
    Write-Output $msg
    exit $ExitCode
}
