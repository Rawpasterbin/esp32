# Local API smoke test for Worker v2.
# Usage:
#   npx wrangler dev --config wrangler.v2.toml --local --port 8787   (in another terminal)
#   powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1

$ErrorActionPreference = 'Stop'
$Base = if ($env:API_BASE) { $env:API_BASE } else { 'http://127.0.0.1:8787' }

$setupToken = if ($env:SETUP_TOKEN) {
  $env:SETUP_TOKEN
} else {
  (Get-Content (Join-Path $PSScriptRoot '..\.dev.vars') |
    Where-Object { $_ -match '^SETUP_TOKEN=' }) -replace '^SETUP_TOKEN=', ''
}
if (-not $setupToken) { throw 'SETUP_TOKEN not found (set env:SETUP_TOKEN or .dev.vars)' }

function Invoke-Api {
  param(
    [string]$Method,
    [string]$Path,
    $Body = $null,
    [hashtable]$Headers = @{}
  )

  $bodyFile = $null
  $headerFile = [System.IO.Path]::GetTempFileName()
  $outFile = [System.IO.Path]::GetTempFileName()

  try {
    $curlArgs = @('-sS', '-X', $Method, "$Base$Path", '-D', $headerFile, '-o', $outFile, '-w', '%{http_code}')
    foreach ($key in $Headers.Keys) {
      $curlArgs += @('-H', "$key`: $($Headers[$key])")
    }
    if ($null -ne $Body) {
      $bodyFile = [System.IO.Path]::GetTempFileName()
      $json = $Body | ConvertTo-Json -Depth 20 -Compress
      [System.IO.File]::WriteAllText($bodyFile, $json, (New-Object System.Text.UTF8Encoding($false)))
      $curlArgs += @('-H', 'Content-Type: application/json; charset=utf-8', '--data-binary', "@$bodyFile")
    }

    $status = & curl.exe @curlArgs
    if ($LASTEXITCODE -ne 0) { throw "curl failed with exit code $LASTEXITCODE" }

    $raw = [System.IO.File]::ReadAllText($outFile, [System.Text.Encoding]::UTF8)
    $json = $null
    if ($raw) { try { $json = $raw | ConvertFrom-Json } catch { } }

    $headerMap = @{}
    $headerText = [System.IO.File]::ReadAllText($headerFile)
    foreach ($line in ($headerText -split "`r?`n")) {
      if ($line -match '^([^:]+):\s*(.*)$') {
        $headerMap[$matches[1]] = $matches[2].Trim()
      }
    }

    return [pscustomobject]@{
      Status  = [int]$status
      Json    = $json
      Raw     = $raw
      Headers = $headerMap
    }
  } finally {
    Remove-Item $headerFile, $outFile -Force -ErrorAction SilentlyContinue
    if ($bodyFile) { Remove-Item $bodyFile -Force -ErrorAction SilentlyContinue }
  }
}

function Assert-Equal {
  param($Actual, $Expected, [string]$Label)
  if ("$Actual" -ne "$Expected") {
    throw "FAIL [$Label] expected '$Expected' but got '$Actual'"
  }
  Write-Host "PASS $Label" -ForegroundColor Green
}

function Assert-True {
  param($Condition, [string]$Label)
  if (-not $Condition) { throw "FAIL [$Label]" }
  Write-Host "PASS $Label" -ForegroundColor Green
}

# Wait for the local Worker.
$deadline = (Get-Date).AddSeconds(60)
$ready = $false
while ((Get-Date) -lt $deadline) {
  try {
    $health = Invoke-Api GET '/health'
    if ($health.Status -eq 200) { $ready = $true; break }
  } catch { }
  Start-Sleep -Milliseconds 500
}
if (-not $ready) { throw 'Worker did not become ready on ' + $Base }
Write-Host "Worker ready: $Base" -ForegroundColor Cyan

# 1. Setup (requires SETUP_TOKEN).
$setup = Invoke-Api POST '/v1/setup' -Headers @{ 'X-Setup-Token' = $setupToken } -Body @{
  householdName = 'Test House'
  password      = 'test-password-123'
  deviceName    = 'Test Valve'
}
Assert-Equal $setup.Status 201 'setup returns 201'
$inviteCode = $setup.Json.data.inviteCode
$deviceId = $setup.Json.data.deviceId
$deviceToken = $setup.Json.data.deviceToken
Assert-True ([bool]$inviteCode -and [bool]$deviceId -and [bool]$deviceToken) 'setup returns inviteCode/deviceId/deviceToken'

# 2. Setup cannot be replayed with a bad token.
$badSetup = Invoke-Api POST '/v1/setup' -Headers @{ 'X-Setup-Token' = 'wrong' } -Body @{ householdName = 'x'; password = 'test-password-123' }
Assert-Equal $badSetup.Status 401 'setup rejects bad setup token'

# 3. Join with invite code + password.
$join = Invoke-Api POST '/v1/auth/join' -Body @{
  inviteCode  = $inviteCode
  password    = 'test-password-123'
  displayName = 'Phone A'
}
Assert-Equal $join.Status 201 'join returns 201'
$token = $join.Json.data.token
$householdId = $join.Json.data.household.id
Assert-True ([bool]$token -and [bool]$householdId) 'join returns session + household'

# 4. Wrong password is rejected.
$badLogin = Invoke-Api POST '/v1/auth/login' -Body @{ inviteCode = $inviteCode; password = 'wrong-password' }
Assert-Equal $badLogin.Status 401 'login rejects wrong password'

# 5. State requires auth.
$noAuth = Invoke-Api GET "/v1/households/$householdId/state"
Assert-Equal $noAuth.Status 401 'state requires bearer token'

# 6. Read state.
$auth = @{ Authorization = "Bearer $token" }
$state1 = Invoke-Api GET "/v1/households/$householdId/state" -Headers $auth
Assert-Equal $state1.Status 200 'state returns 200'
Assert-Equal $state1.Json.data.revision 1 'state starts at revision 1'
Assert-Equal $state1.Json.data.mode 'manual' 'state starts in manual mode'

# 7. Update config with baseRevision 1.
$config = @{
  mode          = 'auto'
  baseRevision  = 1
  schedules     = @(
    @{
      id      = 1
      name    = 'Morning'
      start   = '06:00'
      end     = '06:30'
      days    = @($true, $true, $true, $true, $true, $false, $false)
      enabled = $true
    }
  )
}
$put1 = Invoke-Api PUT "/v1/households/$householdId/config" -Headers $auth -Body $config
Assert-Equal $put1.Status 200 'PUT config with revision 1 returns 200'
Assert-Equal $put1.Json.data.revision 2 'revision increments to 2'

# 8. Stale write is rejected with 409 and current state.
$put2 = Invoke-Api PUT "/v1/households/$householdId/config" -Headers $auth -Body $config
Assert-Equal $put2.Status 409 'stale PUT returns 409'
Assert-Equal $put2.Json.error.code 'CONFLICT' '409 body has CONFLICT code'
Assert-Equal $put2.Json.error.details.current.revision 2 '409 includes current revision'

# 9. ETag / 304.
$etag = [string]$state1.Headers['ETag']
$state2 = Invoke-Api GET "/v1/households/$householdId/state" -Headers $auth
$newEtag = [string]$state2.Headers['ETag']
$notModified = Invoke-Api GET "/v1/households/$householdId/state" -Headers @{ Authorization = "Bearer $token"; 'If-None-Match' = $newEtag }
Assert-Equal $notModified.Status 304 'state returns 304 when ETag matches'

# 10. Create command (idempotent).
$cmdBody = @{ deviceId = $deviceId; action = 'open'; idempotencyKey = 'test-idem-1' }
$cmd1 = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $auth -Body $cmdBody
Assert-Equal $cmd1.Status 201 'create command returns 201'
$commandId = $cmd1.Json.data.commandId
Assert-Equal $cmd1.Json.data.status 'pending' 'new command is pending'
$cmd2 = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $auth -Body $cmdBody
Assert-Equal $cmd2.Json.data.commandId $commandId 'idempotency key returns same command'

# 11. Device sync returns config + pending command.
$deviceAuth = @{ Authorization = "Bearer $deviceToken" }
$sync1 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{
  valve = $false; mode = 'auto'; rssi = -62; uptimeSeconds = 120; firmwareVersion = '2.0.0-test'; lastCommandId = $null; fault = $null
}
Assert-Equal $sync1.Status 200 'device sync returns 200'
Assert-Equal $sync1.Json.data.revision 2 'device sees revision 2'
Assert-Equal $sync1.Json.data.command.id $commandId 'device receives pending command'

# 12. Ack the command; next sync has no pending command.
$ack = Invoke-Api POST "/v1/devices/commands/$commandId/ack" -Headers $deviceAuth -Body @{ status = 'acked'; error = $null }
Assert-Equal $ack.Status 200 'ack returns 200'
$sync2 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{ valve = $true; mode = 'auto'; firmwareVersion = '2.0.0-test' }
Assert-Equal $sync2.Status 200 'second device sync returns 200'
Assert-True ($null -eq $sync2.Json.data.command) 'acked command is not returned again'

# 13. Bad device token is rejected.
$badDevice = Invoke-Api POST '/v1/devices/sync' -Headers @{ Authorization = 'Bearer dev_wrong' } -Body @{}
Assert-Equal $badDevice.Status 401 'device sync rejects bad token'

# 14. Status is visible to the app.
$state3 = Invoke-Api GET "/v1/households/$householdId/state" -Headers $auth
Assert-Equal $state3.Status 200 'state after sync returns 200'
Assert-True ([bool]$state3.Json.data.devices[0].lastSeenAt) 'state exposes device lastSeenAt'
Assert-Equal $state3.Json.data.devices[0].valve $true 'state exposes valve status from device'

# 15. Android write path: If-Match with a quoted ETag drives the revision.
$ifMatchConfig = @{
  mode         = 'manual'
  baseRevision = 2
  schedules    = @(
    @{
      id      = 1
      name    = 'Morning'
      start   = '06:00'
      end     = '06:30'
      days    = @($true, $true, $true, $true, $true, $false, $false)
      enabled = $true
    }
  )
}
$put3 = Invoke-Api PUT "/v1/households/$householdId/config" -Headers @{
  Authorization = "Bearer $token"
  'If-Match'    = '"rev-2"'
} -Body $ifMatchConfig
Assert-Equal $put3.Status 200 'PUT config with If-Match rev-2 returns 200'
Assert-Equal $put3.Json.data.revision 3 'If-Match write increments revision to 3'

# 16. A stale If-Match is rejected even if baseRevision looks current.
$put4 = Invoke-Api PUT "/v1/households/$householdId/config" -Headers @{
  Authorization = "Bearer $token"
  'If-Match'    = '"rev-2"'
} -Body $ifMatchConfig
Assert-Equal $put4.Status 409 'stale If-Match returns 409'
Assert-Equal $put4.Json.error.details.current.revision 3 'stale If-Match reports current revision'

# 17. Unquoted/weak validators are tolerated (Android sends quoted, but be lenient).
$put5 = Invoke-Api PUT "/v1/households/$householdId/config" -Headers @{
  Authorization = "Bearer $token"
  'If-Match'    = 'W/"rev-3"'
} -Body @{
  mode         = 'auto'
  baseRevision = 3
  schedules    = $ifMatchConfig.schedules
}
Assert-Equal $put5.Status 200 'weak If-Match validator is accepted'
Assert-Equal $put5.Json.data.revision 4 'weak If-Match write increments revision to 4'

# 18. Invalid schedule time is rejected without changing the revision.
$badConfig = Invoke-Api PUT "/v1/households/$householdId/config" -Headers $auth -Body @{
  mode         = 'auto'
  baseRevision = 4
  schedules    = @(
    @{
      id      = 1
      name    = 'Bad'
      start   = '25:00'
      end     = '06:30'
      days    = @($true, $true, $true, $true, $true, $false, $false)
      enabled = $true
    }
  )
}
Assert-Equal $badConfig.Status 422 'invalid HH:MM returns 422'
Assert-Equal $badConfig.Json.error.code 'VALIDATION' 'invalid schedule has VALIDATION code'

# 19. The app can list command history and see the acked command.
$list1 = Invoke-Api GET "/v1/households/$householdId/commands?limit=10" -Headers $auth
Assert-Equal $list1.Status 200 'command list returns 200'
$listedAck = $list1.Json.data.commands | Where-Object { $_.commandId -eq $commandId }
Assert-True ($null -ne $listedAck) 'command list includes the earlier command'
Assert-Equal $listedAck.status 'acked' 'command list shows acked status'

# 20. Firmware auto-ack path: sync reports lastCommandId, D1 marks it acked.
$cmdBody2 = @{ deviceId = $deviceId; action = 'close'; idempotencyKey = 'test-idem-2' }
$cmd3 = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $auth -Body $cmdBody2
Assert-Equal $cmd3.Status 201 'second command returns 201'
$commandId2 = $cmd3.Json.data.commandId

$sync3 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{
  valve = $true; mode = 'auto'; firmwareVersion = '2.1.0-test'; lastCommandId = $null
}
Assert-Equal $sync3.Json.data.command.id $commandId2 'device receives the close command'

# The device applies it and reports it on the next sync (MQTT owns TLS in between).
$sync4 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{
  valve = $false; mode = 'auto'; firmwareVersion = '2.1.0-test'; lastCommandId = $commandId2
}
Assert-Equal $sync4.Status 200 'sync with lastCommandId returns 200'

$list2 = Invoke-Api GET "/v1/households/$householdId/commands?limit=10" -Headers $auth
$autoAcked = $list2.Json.data.commands | Where-Object { $_.commandId -eq $commandId2 }
Assert-Equal $autoAcked.status 'acked' 'sync lastCommandId auto-acks the command'
Assert-True ([bool]$autoAcked.ackedAt) 'auto-ack records ackedAt'

$sync5 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{
  valve = $false; mode = 'auto'; firmwareVersion = '2.1.0-test'; lastCommandId = $commandId2
}
Assert-True ($null -eq $sync5.Json.data.command) 'auto-acked command is not returned again'

# 21. A newer command supersedes older pending ones (latest wins, no stale open).
$cmdA = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $auth -Body @{
  deviceId = $deviceId; action = 'open'; idempotencyKey = 'test-idem-a'
}
Assert-Equal $cmdA.Status 201 'first queued command returns 201'
$cmdB = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $auth -Body @{
  deviceId = $deviceId; action = 'close'; idempotencyKey = 'test-idem-b'
}
Assert-Equal $cmdB.Status 201 'second queued command returns 201'

$ttlMinutes = ([datetime]$cmdB.Json.data.expiresAt - [datetime]$cmdB.Json.data.createdAt).TotalMinutes
Assert-True ($ttlMinutes -ge 19) 'command TTL covers the 15-minute device sync interval'

$list3 = Invoke-Api GET "/v1/households/$householdId/commands?limit=10" -Headers $auth
$superseded = $list3.Json.data.commands | Where-Object { $_.commandId -eq $cmdA.Json.data.commandId }
Assert-Equal $superseded.status 'expired' 'older pending command is expired'
Assert-Equal $superseded.error 'superseded' 'older command records superseded reason'

$sync6 = Invoke-Api POST '/v1/devices/sync' -Headers $deviceAuth -Body @{
  valve = $false; mode = 'auto'; firmwareVersion = '2.1.0-test'; lastCommandId = $commandId2
}
Assert-Equal $sync6.Json.data.command.id $cmdB.Json.data.commandId 'device receives only the newest command'

$ackB = Invoke-Api POST "/v1/devices/commands/$($cmdB.Json.data.commandId)/ack" -Headers $deviceAuth -Body @{ status = 'acked'; error = $null }
Assert-Equal $ackB.Status 200 'newest command ack returns 200'

# 22. A second household is fully isolated from the first.
$setupB = Invoke-Api POST '/v1/setup' -Headers @{ 'X-Setup-Token' = $setupToken } -Body @{
  householdName = 'Other House'
  password      = 'other-password-456'
  deviceName    = 'Other Valve'
}
Assert-Equal $setupB.Status 201 'second setup returns 201'
$inviteCodeB = $setupB.Json.data.inviteCode
$deviceIdB = $setupB.Json.data.deviceId
$deviceTokenB = $setupB.Json.data.deviceToken

$joinB = Invoke-Api POST '/v1/auth/join' -Body @{
  inviteCode  = $inviteCodeB
  password    = 'other-password-456'
  displayName = 'Phone B'
}
Assert-Equal $joinB.Status 201 'second household join returns 201'
$tokenB = $joinB.Json.data.token
$householdIdB = $joinB.Json.data.household.id
$memberIdB = $joinB.Json.data.member.id
$authB = @{ Authorization = "Bearer $tokenB" }

$crossState = Invoke-Api GET "/v1/households/$householdId/state" -Headers $authB
Assert-Equal $crossState.Status 403 'second household cannot read first state'
$crossConfig = Invoke-Api PUT "/v1/households/$householdId/config" -Headers $authB -Body @{
  mode = 'auto'; baseRevision = 4; schedules = @()
}
Assert-Equal $crossConfig.Status 403 'second household cannot write first config'
$crossList = Invoke-Api GET "/v1/households/$householdId/commands" -Headers $authB
Assert-Equal $crossList.Status 403 'second household cannot list first commands'
$crossCommand = Invoke-Api POST "/v1/households/$householdId/commands" -Headers $authB -Body @{
  deviceId = $deviceId; action = 'open'; idempotencyKey = 'cross-house'
}
Assert-Equal $crossCommand.Status 403 'second household cannot command first device'

# 23. Device tokens are scoped to their own device and household.
$crossAck = Invoke-Api POST "/v1/devices/commands/$($cmdB.Json.data.commandId)/ack" -Headers @{
  Authorization = "Bearer $deviceTokenB"
} -Body @{ status = 'acked' }
Assert-Equal $crossAck.Status 404 'device cannot ack another household command'
$syncB = Invoke-Api POST '/v1/devices/sync' -Headers @{ Authorization = "Bearer $deviceTokenB" } -Body @{
  valve = $false; mode = 'manual'; firmwareVersion = '2.1.0-test'
}
Assert-Equal $syncB.Status 200 'second device sync returns 200'
Assert-Equal $syncB.Json.data.revision 1 'second device sees its own revision'
Assert-True ($null -eq $syncB.Json.data.command) 'second device does not see first household command'

# 24. /v1/me returns identity and logout revokes the session.
$meB = Invoke-Api GET '/v1/me' -Headers $authB
Assert-Equal $meB.Status 200 'me returns 200'
Assert-Equal $meB.Json.data.household.id $householdIdB 'me returns the right household'
$logoutB = Invoke-Api POST '/v1/auth/logout' -Headers $authB
Assert-Equal $logoutB.Status 200 'logout returns 200'
$meAfterLogout = Invoke-Api GET '/v1/me' -Headers $authB
Assert-Equal $meAfterLogout.Status 401 'revoked token is rejected'
$stateAfterLogout = Invoke-Api GET "/v1/households/$householdIdB/state" -Headers $authB
Assert-Equal $stateAfterLogout.Status 401 'revoked token cannot read state'

# 25. Login with the stored memberId reuses the member and issues a new token.
$reloginB = Invoke-Api POST '/v1/auth/login' -Body @{
  inviteCode = $inviteCodeB; password = 'other-password-456'; memberId = $memberIdB
}
Assert-Equal $reloginB.Status 200 'login with memberId returns 200'
Assert-Equal $reloginB.Json.data.member.id $memberIdB 'login reuses the existing member'
$stateB = Invoke-Api GET "/v1/households/$householdIdB/state" -Headers @{
  Authorization = "Bearer $($reloginB.Json.data.token)"
}
Assert-Equal $stateB.Status 200 'new session can read its household state'
$authBNew = @{ Authorization = "Bearer $($reloginB.Json.data.token)" }

# 26. Household password rotation revokes other sessions but not the caller.
$joinB2 = Invoke-Api POST '/v1/auth/join' -Body @{
  inviteCode = $inviteCodeB; password = 'other-password-456'; displayName = 'Phone B2'
}
Assert-Equal $joinB2.Status 201 'second session for household B returns 201'
$authB2 = @{ Authorization = "Bearer $($joinB2.Json.data.token)" }

$badRotate = Invoke-Api POST "/v1/households/$householdIdB/password" -Headers $authBNew -Body @{
  currentPassword = 'wrong-password'; newPassword = 'new-password-789'
}
Assert-Equal $badRotate.Status 401 'password rotation rejects a wrong current password'
Assert-Equal $badRotate.Json.error.code 'INVALID_PASSWORD' 'wrong current password uses INVALID_PASSWORD, not session expiry'
$shortRotate = Invoke-Api POST "/v1/households/$householdIdB/password" -Headers $authBNew -Body @{
  currentPassword = 'other-password-456'; newPassword = 'short'
}
Assert-Equal $shortRotate.Status 422 'password rotation rejects a short new password'
$crossRotate = Invoke-Api POST "/v1/households/$householdIdB/password" -Headers $auth -Body @{
  currentPassword = 'test-password-123'; newPassword = 'new-password-789'
}
Assert-Equal $crossRotate.Status 403 'another household cannot change the password'

$rotate = Invoke-Api POST "/v1/households/$householdIdB/password" -Headers $authBNew -Body @{
  currentPassword = 'other-password-456'; newPassword = 'new-password-789'
}
Assert-Equal $rotate.Status 200 'password rotation returns 200'

$oldLogin = Invoke-Api POST '/v1/auth/login' -Body @{
  inviteCode = $inviteCodeB; password = 'other-password-456'
}
Assert-Equal $oldLogin.Status 401 'old household password no longer works'
$newLogin = Invoke-Api POST '/v1/auth/login' -Body @{
  inviteCode = $inviteCodeB; password = 'new-password-789'
}
Assert-Equal $newLogin.Status 200 'new household password works'
$revokedB2 = Invoke-Api GET '/v1/me' -Headers $authB2
Assert-Equal $revokedB2.Status 401 'password rotation revokes other sessions'
$meBAfterRotate = Invoke-Api GET '/v1/me' -Headers $authBNew
Assert-Equal $meBAfterRotate.Status 200 'the rotating session stays valid'

# 27. Device token rotation invalidates the old token immediately.
$crossTokenRotate = Invoke-Api POST "/v1/households/$householdIdB/devices/$deviceIdB/rotate-token" -Headers $auth
Assert-Equal $crossTokenRotate.Status 403 'another household cannot rotate the device token'
$missingRotate = Invoke-Api POST "/v1/households/$householdIdB/devices/does-not-exist/rotate-token" -Headers $authBNew
Assert-Equal $missingRotate.Status 404 'device token rotation rejects an unknown device'

$rotateToken = Invoke-Api POST "/v1/households/$householdIdB/devices/$deviceIdB/rotate-token" -Headers $authBNew
Assert-Equal $rotateToken.Status 200 'device token rotation returns 200'
$newDeviceTokenB = $rotateToken.Json.data.deviceToken
Assert-True ([bool]$newDeviceTokenB -and $newDeviceTokenB -ne $deviceTokenB) 'rotation returns a new device token'

$oldSync = Invoke-Api POST '/v1/devices/sync' -Headers @{
  Authorization = "Bearer $deviceTokenB"
} -Body @{ valve = $false; mode = 'manual' }
Assert-Equal $oldSync.Status 401 'old device token is rejected'
$newSync = Invoke-Api POST '/v1/devices/sync' -Headers @{
  Authorization = "Bearer $newDeviceTokenB"
} -Body @{ valve = $false; mode = 'manual' }
Assert-Equal $newSync.Status 200 'new device token works'

# 28. Repeated failed logins are rate limited per client IP.
# This intentionally trips the limiter for the current IP. If the suite is
# re-run within a minute, clear auth_attempts first:
#   npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote `
#     --command "DELETE FROM auth_attempts;"
$limitedStatus = 0
for ($i = 0; $i -lt 6; $i++) {
  $limited = Invoke-Api POST '/v1/auth/login' -Body @{
    inviteCode = $inviteCodeB; password = 'definitely-wrong'
  }
  $limitedStatus = $limited.Status
  if ($limitedStatus -eq 429) { break }
}
Assert-Equal $limitedStatus 429 'repeated failed logins are rate limited'
$blockedCorrect = Invoke-Api POST '/v1/auth/login' -Body @{
  inviteCode = $inviteCodeB; password = 'new-password-789'
}
Assert-Equal $blockedCorrect.Status 429 'rate limit blocks a correct password during the window'

Write-Host ''
Write-Host 'All API smoke tests passed.' -ForegroundColor Cyan
