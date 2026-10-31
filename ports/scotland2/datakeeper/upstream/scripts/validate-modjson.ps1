$mod = "./mod.json"
$modTemplate = "./mod.template.json"
$qpmShared = "./qpm.shared.json"

if (Test-Path -Path $modTemplate) {
    $update = -not (Test-Path -Path $mod)
    if (-not $update) {
        $update = (Get-Item $modTemplate).LastWriteTime -gt (Get-Item $mod).LastWriteTime
    }
    if (-not $update -and (Test-Path -Path $qpmShared)) {
        $update = (Get-Item $qpmShared).LastWriteTime -gt (Get-Item $mod).LastWriteTime
    }

    if ($update) {
        & qpm qmod manifest
        if ($LASTEXITCODE -ne 0) {
            exit $LASTEXITCODE
        }
    }
}
elseif (-not (Test-Path -Path $mod)) {
    Write-Output "Error: mod.json and mod.template.json were not present"
    exit 1
}

$psVersion = $PSVersionTable.PSVersion.Major
if ($psVersion -ge 6) {
    $schemaUrl = "https://raw.githubusercontent.com/Lauriethefish/QuestPatcher.QMod/main/QuestPatcher.QMod/Resources/qmod.schema.json"
    Invoke-WebRequest $schemaUrl -OutFile ./mod.schema.json

    $schema = "./mod.schema.json"
    $modJsonRaw = Get-Content $mod -Raw
    $modSchemaRaw = Get-Content $schema -Raw

    Remove-Item $schema

    Write-Output "Validating mod.json..."
    if (-not ($modJsonRaw | Test-Json -Schema $modSchemaRaw)) {
        Write-Output "Error: mod.json is not valid"
        exit 1
    }
}
else {
    Write-Output "Could not validate mod.json with schema: powershell version was too low (< 6)"
}
exit
