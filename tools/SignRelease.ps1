param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$Files
)

$ErrorActionPreference = 'Stop'

function Find-SignTool {
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    if (-not (Test-Path $sdkRoot)) { throw 'signtool.exe was not found. Install the Windows SDK signing tools.' }

    $candidates = Get-ChildItem -Path $sdkRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending |
        ForEach-Object { Join-Path $_.FullName 'x64\signtool.exe' } |
        Where-Object { Test-Path $_ }
    $tool = $candidates | Select-Object -First 1
    if (-not $tool) { throw 'signtool.exe was not found in the Windows SDK.' }
    return $tool
}

$thumbprint = ($env:VMP_SIGN_CERT_SHA1 -replace '\s','')
$pfxPath = $env:VMP_SIGN_PFX
$pfxPassword = $env:VMP_SIGN_PFX_PASSWORD
$timestampUrl = if ($env:VMP_TIMESTAMP_URL) { $env:VMP_TIMESTAMP_URL } else { 'http://timestamp.digicert.com' }

if (-not $thumbprint -and -not $pfxPath) {
    throw 'Signing is not configured. Set VMP_SIGN_CERT_SHA1 or VMP_SIGN_PFX.'
}
if ($thumbprint -and $pfxPath) {
    throw 'Configure only one signing identity: VMP_SIGN_CERT_SHA1 or VMP_SIGN_PFX.'
}
if ($pfxPath -and -not (Test-Path $pfxPath)) {
    throw "PFX file not found: $pfxPath"
}

$signtool = Find-SignTool
$identityArgs = @()
if ($thumbprint) {
    $identityArgs = @('/sha1', $thumbprint)
} else {
    $identityArgs = @('/f', (Resolve-Path $pfxPath).Path)
    if ($pfxPassword) { $identityArgs += @('/p', $pfxPassword) }
}

foreach ($file in $Files) {
    if (-not (Test-Path $file)) { throw "File to sign not found: $file" }
    $resolved = (Resolve-Path $file).Path
    $args = @('sign','/fd','SHA256','/td','SHA256','/tr',$timestampUrl) + $identityArgs + @($resolved)
    & $signtool @args
    if ($LASTEXITCODE -ne 0) { throw "Signing failed for: $resolved" }

    & $signtool verify /pa /q $resolved
    if ($LASTEXITCODE -ne 0) { throw "Signature verification failed for: $resolved" }
    Write-Host "Signed and verified: $resolved"
}
