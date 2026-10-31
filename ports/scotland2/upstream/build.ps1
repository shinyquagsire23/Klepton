Param(
    [Parameter(Mandatory = $false)]
    [Switch] $clean
)

if ($clean.IsPresent) {
    if (Test-Path -Path "build") {
        Remove-Item build -R
    }
}
if (-not (Test-Path -Path "build")) {
    New-Item -Path build -ItemType Directory
}

& cmake -G "Ninja" -DCMAKE_BUILD_TYPE="RelWithDebInfo" -B build
& cmake --build ./build

$ExitCode = $LastExitCode

# Post build, we actually want to transform the compile_commands.json file such that it has no \\ characters and ONLY has / characters
(Get-Content -Path build/compile_commands.json) |
ForEach-Object { $_ -Replace '\\\\', '/' } | ForEach-Object { $_ -Replace '/\\"', '\\\"' } | Set-Content -Path build/compile_commands.json

exit $ExitCode
