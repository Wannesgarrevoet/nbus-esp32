<#
.SYNOPSIS
  Build the firmware and push it to the device over the network (no USB).

.DESCRIPTION
  Builds with a pinned FQBN and uploads via ElegantOTA. The board options are not
  optional: compiling with bare defaults produces an image that runs and networks
  correctly but sends its console to UART0 instead of USB, which is invisible until you
  next plug a cable in.

  CDCOnBoot=cdc         Serial goes to USB. Defaults to Disabled (UART0 pins).
  PartitionScheme=default  Two 1.2 MB app slots (so OTA works) + 1.5 MB filesystem for
                        fault logs. The partition table is NOT part of an OTA image, so
                        changing it requires physical USB access. Only revisit it with a
                        cable in hand.

  These were briefly pinned in a firmware/sketch.yaml build profile instead. Do not go
  back to that: declaring a profile makes arduino-cli stage its own private copy of the
  platform and SDK under Arduino15/internal, and that copy compiles the core as C++20.
  Under C++20 <memory> drags in <system_error>, which this RISC-V toolchain cannot
  resolve (its error_constants.h ships only in the picolibc tree, which the SDK's include
  flags do not reference). The result is that every build touching WebServer.h fails.
  Passing --fqbn keeps the Boards Manager platform, which builds correctly.

  ElegantOTA v3 splits the upload into two requests:
    GET  /ota/start?mode=fw&hash=<md5>   arms Update and registers the expected hash
    POST /ota/upload                     multipart body with the image

  The hash matters. Without it the device accepts whatever arrives and reboots into it;
  with it, a truncated or corrupted transfer is rejected before it can replace a working
  image on a device that may be parked somewhere inconvenient.

.PARAMETER Host
  Device address. Reachable over Tailscale via the subnet route advertised for the
  camper LAN, so this works from anywhere.

.PARAMETER User
  Optional HTTP basic-auth user, if the device has OTA credentials configured.

.EXAMPLE
  pwsh tools/flash-ota.ps1
  pwsh tools/flash-ota.ps1 -DeviceHost 192.168.1.20 -User nbus
#>
param(
  [string]$DeviceHost = "192.168.1.20",
  [string]$User = "",
  [string]$Password = "",
  [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
$bin  = Join-Path $repo "build\firmware.ino.bin"

$fqbn = "esp32:esp32:esp32c3:CDCOnBoot=cdc,PartitionScheme=default"

if (-not $SkipBuild) {
  Write-Host "building $fqbn..."
  & arduino-cli compile --fqbn $fqbn --output-dir (Join-Path $repo "build") (Join-Path $repo "firmware")
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
}
if (-not (Test-Path $bin)) { throw "no image at $bin" }

$size = (Get-Item $bin).Length
$md5  = (Get-FileHash -Path $bin -Algorithm MD5).Hash.ToLower()
Write-Host "image $size bytes, md5 $md5"

# Confirm the device is up before arming Update, so a typo'd address fails cheaply
# rather than half way through an upload.
$auth = @{}
if ($User) {
  $sec = ConvertTo-SecureString $Password -AsPlainText -Force
  $auth["Credential"] = New-Object System.Management.Automation.PSCredential($User, $sec)
}
$probe = Invoke-WebRequest -Uri "http://$DeviceHost/" -TimeoutSec 10 -UseBasicParsing @auth
Write-Host "device responded HTTP $($probe.StatusCode)"

# Record what is running now. "HTTP 200 afterwards" only proves *something* is alive —
# a failed OTA that rolls back to the previous image answers exactly the same way.
# Comparing uptime before and after is what actually distinguishes the two.
$before = $null
try {
  $before = (Invoke-WebRequest -Uri "http://$DeviceHost/status" -TimeoutSec 10 `
                               -UseBasicParsing @auth).Content | ConvertFrom-Json
  Write-Host "running $($before.version) ($($before.build)), up $($before.uptime_s)s"
} catch {
  Write-Host "no /status yet (pre-0.4.0 firmware) - cannot verify by version"
}

$start = Invoke-WebRequest -Uri "http://$DeviceHost/ota/start?mode=fw&hash=$md5" `
                           -TimeoutSec 20 -UseBasicParsing @auth
if ($start.Content.Trim() -ne "OK") { throw "ota/start refused: $($start.Content)" }

# curl.exe rather than Invoke-WebRequest: Windows PowerShell 5.1 has no -Form, and
# hand-rolling a multipart body for a 1.1 MB binary is a needless way to corrupt it.
$curlArgs = @("-s", "--fail", "-m", "300", "-F", "file=@$bin", "http://$DeviceHost/ota/upload")
if ($User) { $curlArgs = @("-u", "${User}:${Password}") + $curlArgs }
$result = & curl.exe @curlArgs
if ($LASTEXITCODE -ne 0) { throw "upload failed (curl exit $LASTEXITCODE)" }
Write-Host "upload: $result"

Write-Host "waiting for reboot..."
Start-Sleep -Seconds 12
try {
  $after = (Invoke-WebRequest -Uri "http://$DeviceHost/status" -TimeoutSec 15 `
                              -UseBasicParsing @auth).Content | ConvertFrom-Json
  Write-Host "back up: $($after.version) ($($after.build)), up $($after.uptime_s)s, heap $($after.heap_free)"
  Write-Host "bus: $($after.frames) frames seen, ring $($after.ring_used)/$($after.ring_size)"

  # An uptime that did not reset means the device never rebooted, so whatever is
  # answering is still the old image regardless of what the upload reported.
  if ($before -and $after.uptime_s -ge $before.uptime_s) {
    Write-Warning "uptime did not reset ($($before.uptime_s)s -> $($after.uptime_s)s) - the new image may not be running"
  } elseif ($before -and $after.build -eq $before.build) {
    Write-Warning "rebooted, but build stamp is unchanged - did you rebuild?"
  } else {
    Write-Host "update confirmed" -ForegroundColor Green
  }
} catch {
  Write-Warning "device did not answer /status yet: $($_.Exception.Message)"
}
