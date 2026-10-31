Param(
    [Parameter(Mandatory=$false)]
    [String] $qmodName="",

    [Parameter(Mandatory=$false)]
    [Switch] $help
)

if ($help -eq $true) {
    Write-Output "`"createqmod`" - Creates a .qmod file with your compiled libraries and mod.json."
    Write-Output "`n-- Arguments --`n"

    Write-Output "-QmodName `t The file name of your qmod"

    exit
}

$mod = "./mod.json"

$modJson = Get-Content $mod -Raw | ConvertFrom-Json

if ($qmodName -eq "") {
    $qmodName = $modJson.name
}
Write-Output "Creating qmod with name ${qmodName}.qmod"

$filelist = @($mod)

$cover = "./" + $modJson.coverImage
if ((-not ($cover -eq "./")) -and (Test-Path $cover)) {
    $filelist += ,$cover
}

foreach ($mod in $modJson.modFiles) {
    $path = "./build/" + $mod
    if (-not (Test-Path $path)) {
        $path = "./extern/libs/" + $mod
    }
    if (-not (Test-Path $path)) {
        Write-Output "Error: could not find dependency: $path"
        exit 1
    }
    $filelist += $path
}

foreach ($mod in $modJson.lateModFiles) {
    $path = "./build/" + $mod
    if (-not (Test-Path $path)) {
        $path = "./extern/libs/" + $mod
    }
    if (-not (Test-Path $path)) {
        Write-Output "Error: could not find dependency: $path"
        exit 1
    }
    $filelist += $path
}

foreach ($lib in $modJson.libraryFiles) {
    $path = "./build/" + $lib
    if (-not (Test-Path $path)) {
        $path = "./extern/libs/" + $lib
    }
    if (-not (Test-Path $path)) {
        Write-Output "Error: could not find dependency: $path"
        exit 1
    }
    $filelist += $path
}

$zip = $qmodName + ".zip"
$qmod = $qmodName + ".qmod"

Compress-Archive -Path $filelist -DestinationPath $zip -Update
Move-Item $zip $qmod -Force
return $LastExitCode
