# Scan the working tree for common secret patterns.
# Usage:
#   powershell -ExecutionPolicy Bypass -File scripts/scan-secrets.ps1
#   powershell -ExecutionPolicy Bypass -File scripts/scan-secrets.ps1 -Root <repo>
# Exits 1 when a likely secret is found.
#
# This does not scan git history. For history use:
#   git log -S '<secret>' --all
# or git-filter-repo with a private replacements file.

param(
  [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
$Skip = '\\.git\\|\\build\\|\\node_modules\\|\\releases\\.*\\.apk$|\\.png$|\\.log$|\\.apk$|\\.keystore$|\\.jks$|\\.p12$|\\.pfx$'
$TextExt = @('.md', '.kt', '.kts', '.java', '.xml', '.js', '.json', '.toml', '.ps1', '.ino', '.txt', '.yml', '.yaml', '.properties', '.gradle', '.html', '.css', '.sql')

$Patterns = @(
  @{ Name = 'GitHub PAT'; Regex = 'github_pat_[A-Za-z0-9_]{20,}' },
  @{ Name = 'GitHub token'; Regex = 'gh[pousr]_[A-Za-z0-9]{20,}' },
  @{ Name = 'AWS access key'; Regex = 'AKIA[0-9A-Z]{16}' },
  @{ Name = 'OpenAI key'; Regex = 'sk-[A-Za-z0-9]{20,}' },
  @{ Name = 'Private key'; Regex = '-----BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY-----' },
  @{ Name = 'MQTT credential assignment'; Regex = '(?i)MQTT_(USER|PASS)\s*=\s*["'']?(?!<|changeme|change_me|your-|xxx)[^"''\s]+' },
  @{ Name = 'Device token'; Regex = '\bdev_[A-Za-z0-9_-]{20,}' },
  @{ Name = 'Session token'; Regex = '\bsess_[A-Za-z0-9_-]{20,}' },
  @{ Name = 'Setup token'; Regex = '(?i)SETUP_TOKEN\s*=\s*["'']?(?!<|changeme|change_me|replace)[A-Za-z0-9_-]{12,}' },
  @{ Name = 'Invite code literal'; Regex = '(?i)(?:inviteCode|invite_code)\s*[:=]\s*["''][A-Z0-9]{4}-?[A-Z0-9]{4}["'']' },
  @{ Name = 'Password literal'; Regex = '(?i)(?:password|passwd|pwd)\s*[:=]\s*["''][^"'']{8,}["'']' },
  @{ Name = 'HiveMQ cluster host'; Regex = '[a-z0-9]{20,}\.s[0-9]\.eu\.hivemq\.cloud' },
  @{ Name = 'Cloudflare Pages URL'; Regex = 'https://[a-z0-9-]+\.pages\.dev' },
  @{ Name = 'Windows user path'; Regex = 'C:\\Users\\[A-Za-z0-9._-]+' }
)

$Findings = @()
Get-ChildItem $Root -Recurse -File -ErrorAction SilentlyContinue |
  Where-Object {
    $TextExt -contains $_.Extension.ToLower() -and
    $_.FullName -notmatch $Skip -and
    $_.Length -lt 2000000
  } |
  ForEach-Object {
    $path = $_.FullName
    $lineNo = 0
    Get-Content -LiteralPath $path -ErrorAction SilentlyContinue | ForEach-Object {
      $lineNo++
      $line = $_
      foreach ($pattern in $Patterns) {
        if ($line -match $pattern.Regex) {
          # Ignore obvious test fixtures/placeholders.
          if ($line -match 'definitely-wrong|wrong-password|old-password|new-password|test-password|other-password|ABCD-1234|example\.com|CHANGE_ME|<[^>]+>') { continue }
          $Findings += [pscustomobject]@{
            Type = $pattern.Name
            File = $path.Replace($Root + '\', '')
            Line = $lineNo
          }
        }
      }
    }
  }

if ($Findings.Count -eq 0) {
  Write-Host 'No likely secrets found in the working tree.' -ForegroundColor Green
  exit 0
}

$Findings | Format-Table -AutoSize
Write-Host ''
Write-Host "Found $($Findings.Count) likely secret(s). Do not commit them." -ForegroundColor Red
exit 1
