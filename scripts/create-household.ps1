# Create a production household + device and save the one-time device token.
#
# Usage:
#   $env:SETUP_TOKEN = '...'
#   powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
#     -HouseholdName 'บ้านของเรา'
#
# The password is prompted as a SecureString when -Password is omitted.
# Output is written outside the repo (default C:\Temp\opencode) and contains
# the device token. Never commit that file.

param(
  [Parameter(Mandatory = $true)]
  [string]$HouseholdName,

  [string]$Password,

  [string]$DeviceName = 'วาล์วน้ำ',

  [string]$ApiBase = 'https://water-valve-api-v2.czqwertyyy.workers.dev',

  [string]$OutFile = 'C:\Temp\opencode\water-valve-household.json',

  [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ApiBase = $ApiBase.TrimEnd('/')

if (-not $Password) {
  $secure = Read-Host -AsSecureString 'Household password (min 8 characters)'
  $bstr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
  try {
    $Password = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
  } finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
  }
}
if ($Password.Length -lt 8) {
  throw 'password must be at least 8 characters'
}

$setupToken = $env:SETUP_TOKEN
if (-not $setupToken) {
  $devVars = Join-Path $PSScriptRoot '..\.dev.vars'
  if (Test-Path $devVars) {
    $line = Get-Content $devVars |
      Where-Object { $_ -match '^SETUP_TOKEN=' } |
      Select-Object -First 1
    if ($line) { $setupToken = ($line -replace '^SETUP_TOKEN=', '').Trim() }
  }
}
if (-not $setupToken) {
  throw 'SETUP_TOKEN not found (set env:SETUP_TOKEN or add it to .dev.vars)'
}

if ((Test-Path $OutFile) -and -not $Force) {
  throw "refusing to overwrite $OutFile (use -Force)"
}

$payload = @{
  householdName = $HouseholdName
  password      = $Password
  deviceName    = $DeviceName
} | ConvertTo-Json

try {
  $response = Invoke-RestMethod `
    -Method Post `
    -Uri "$ApiBase/v1/setup" `
    -Headers @{ 'X-Setup-Token' = $setupToken } `
    -ContentType 'application/json; charset=utf-8' `
    -Body $payload
} catch {
  $detail = $_.ErrorDetails.Message
  throw "setup failed: $($_.Exception.Message) $detail"
}

$data = $response.data
if (-not $data.deviceToken -or -not $data.inviteCode) {
  throw 'setup response did not include deviceToken/inviteCode'
}

$outDir = Split-Path -Parent $OutFile
if ($outDir -and -not (Test-Path $outDir)) {
  New-Item -ItemType Directory -Path $outDir | Out-Null
}

$record = [pscustomobject]@{
  apiBase       = $ApiBase
  householdId   = $data.householdId
  householdName = $HouseholdName
  inviteCode    = $data.inviteCode
  memberId      = $data.memberId
  deviceId      = $data.deviceId
  deviceToken   = $data.deviceToken
  createdAt     = (Get-Date).ToUniversalTime().ToString('o')
}
$record | ConvertTo-Json -Depth 5 | Set-Content -Path $OutFile -Encoding UTF8

# Best effort: only the current Windows user may read/write the token file.
try {
  & icacls.exe $OutFile /inheritance:r /grant:r "$($env:USERNAME):(R,W)" | Out-Null
} catch {
  Write-Warning "Could not restrict ACL on $OutFile"
}

Write-Host ''
Write-Host 'Household created.' -ForegroundColor Green
Write-Host "  Household : $($record.householdName)"
Write-Host "  Household ID: $($record.householdId)"
Write-Host "  Invite code : $($record.inviteCode)"
Write-Host "  Device ID   : $($record.deviceId)"
Write-Host "  Token file  : $OutFile"
Write-Host ''
Write-Host 'Firmware provisioning (serial 115200):' -ForegroundColor Cyan
Write-Host "  set token $($record.deviceToken)"
Write-Host "  set url $ApiBase"
Write-Host '  set apn internet'
Write-Host '  set mqttuser <hivemq-device-user>'
Write-Host '  set mqttpass <hivemq-device-password>'
Write-Host '  set maxopen 30'
Write-Host '  sync'
Write-Host ''
Write-Host 'Keep the token file secret; it is not stored in the repository.' -ForegroundColor Yellow
Write-Host 'The household password is not written to disk.' -ForegroundColor Yellow
