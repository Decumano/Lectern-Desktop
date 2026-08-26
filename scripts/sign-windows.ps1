# Authenticode-signs the Windows binary and installers.
#
# You supply the certificate. This script never creates one: a self-signed
# certificate would produce a signature that no user's machine trusts and that
# SmartScreen treats exactly like an unsigned binary, so it would be strictly
# worse than shipping unsigned — it would only look signed.
#
# Two ways to provide the certificate, in order of preference:
#
#   1. Azure Trusted Signing / a hardware token, addressed by thumbprint:
#        -Thumbprint <sha1-thumbprint>
#      The key never leaves the token. This is what a code-signing certificate
#      issued after June 2023 requires anyway — CA/B Forum rules mandate
#      hardware key storage, so a plain .pfx from a public CA no longer exists.
#
#   2. A .pfx file, for an internal or legacy certificate:
#        -PfxPath <path> -PfxPassword <password>
#      In CI, pass the .pfx as a base64 secret and write it to a temp file.
#
# Timestamping is not optional: without it every signature expires when the
# certificate does, and already-shipped installers start warning.
#
# Usage:
#   scripts/sign-windows.ps1 -Path build/Release/lectern.exe -Thumbprint ABC123...
#   scripts/sign-windows.ps1 -Path build/Lectern-0.12.11-windows-x64-setup.exe -PfxPath cert.pfx -PfxPassword $env:PFX_PASSWORD
[CmdletBinding()]
param(
  [Parameter(Mandatory = $true)]
  [string[]]$Path,

  [string]$Thumbprint,
  [string]$PfxPath,
  [string]$PfxPassword,

  [string]$TimestampUrl = 'http://timestamp.digicert.com',
  [string]$Description = 'Lectern'
)

$ErrorActionPreference = 'Stop'

if (-not $Thumbprint -and -not $PfxPath) {
  Write-Error 'Provide either -Thumbprint or -PfxPath. Refusing to produce an unsigned "signed" build.'
}

# signtool ships with the Windows SDK, which is not on PATH by default.
$signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
if (-not $signtool) {
  $candidates = Get-ChildItem -Path "${env:ProgramFiles(x86)}\Windows Kits\10\bin" `
      -Filter signtool.exe -Recurse -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -match '\\x64\\' } |
    Sort-Object FullName -Descending
  if (-not $candidates) {
    Write-Error 'signtool.exe not found. Install the Windows SDK signing tools.'
  }
  $signtool = $candidates[0]
}
Write-Host "Using $($signtool.Source ?? $signtool.FullName)"

$common = @(
  'sign',
  '/fd', 'SHA256',
  '/td', 'SHA256',
  '/tr', $TimestampUrl,
  '/d', $Description,
  '/v'
)

if ($Thumbprint) {
  $common += @('/sha1', $Thumbprint)
} else {
  if (-not (Test-Path $PfxPath)) { Write-Error "No such file: $PfxPath" }
  $common += @('/f', $PfxPath)
  if ($PfxPassword) { $common += @('/p', $PfxPassword) }
}

foreach ($target in $Path) {
  if (-not (Test-Path $target)) { Write-Error "No such file: $target" }
  Write-Host "==> signing $target"
  & ($signtool.Source ?? $signtool.FullName) @common $target
  if ($LASTEXITCODE -ne 0) { Write-Error "signtool failed for $target" }
}

# Verifying afterwards catches a signature that applied but won't chain — for
# example a certificate whose intermediate is missing from the machine.
foreach ($target in $Path) {
  Write-Host "==> verifying $target"
  & ($signtool.Source ?? $signtool.FullName) verify /pa /v $target
  if ($LASTEXITCODE -ne 0) { Write-Error "verification failed for $target" }
}

Write-Host 'All files signed and verified.'
