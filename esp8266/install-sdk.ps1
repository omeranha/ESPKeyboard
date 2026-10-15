<#
Installs the toolchain and SDK this project needs, on Windows, without MSYS2,
Cygwin or WSL.

Two pinned pieces:
  * ESP8266 NONOS SDK 1.5.4. The software USB driver does not work on anything
    newer -- upstream espusb states that SDKs past 1.5.x are incompatible, and
    the reason is interrupt entry latency, not API changes.
  * xtensa-lx106-elf GCC 4.8.5, the version espusb's user.cfg names in
    ESP_GCC_VERS, so libgcc.a sits at the path the link expects.

Neither is on Espressif's GitHub (only v2.0.0 and later are tagged there), so
both come from the nodemcu/espressif-sdk-archive mirror. esptool.py is taken
from the ESP8266 RTOS SDK that is already installed, along with its Python
environment, which already has pyserial.

This writes only under -EspRoot and does not touch PATH or any existing
ESP-IDF / RTOS SDK install.
#>
param(
    [string]$EspRoot = 'C:\esp\espusb-sdk',
    [string]$RtosSdkPath = 'C:\esp\v6.1\ESP8266_RTOS_SDK',
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$mirror = 'https://github.com/nodemcu/espressif-sdk-archive/raw/master/'
$sdkZip = 'esp_iot_sdk_v1.5.4_16_05_20.zip'
$tcZip  = 'xtensa-lx106-elf-win32-1.22.0-88-gde0bdc1-4.8.5.zip'

# Sizes are checked because the mirror stores some entries as Git LFS pointers,
# which download as a ~134 byte text stub instead of the archive.
$expected = @{ $sdkZip = 3251308; $tcZip = 33959416 }

New-Item -ItemType Directory -Force -Path (Join-Path $EspRoot 'dl') | Out-Null

foreach ($file in @($sdkZip, $tcZip)) {
    $target = Join-Path $EspRoot "dl\$file"
    if ((Test-Path -LiteralPath $target) -and -not $Force) {
        Write-Host "Already downloaded: $file" -ForegroundColor DarkGray
    } else {
        Write-Host "Downloading $file ..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri ($mirror + $file) -OutFile $target
    }
    $size = (Get-Item -LiteralPath $target).Length
    if ($size -ne $expected[$file]) {
        throw "$file is $size bytes, expected $($expected[$file]). The mirror may have returned a Git LFS pointer."
    }
}

Write-Host 'Extracting ...' -ForegroundColor Cyan
$tmpSdk = Join-Path $EspRoot 'tmp-sdk'
$tmpTc  = Join-Path $EspRoot 'tmp-tc'
Remove-Item -Recurse -Force $tmpSdk, $tmpTc -ErrorAction SilentlyContinue
Expand-Archive -Path (Join-Path $EspRoot "dl\$sdkZip") -DestinationPath $tmpSdk -Force
Expand-Archive -Path (Join-Path $EspRoot "dl\$tcZip")  -DestinationPath $tmpTc  -Force

$sdkDest = Join-Path $EspRoot 'sdk'
$tcDest  = Join-Path $EspRoot 'xtensa-lx106-elf'
Remove-Item -Recurse -Force $sdkDest, $tcDest -ErrorAction SilentlyContinue
Move-Item (Join-Path $tmpSdk 'ESP8266_NONOS_SDK') $sdkDest
Move-Item (Join-Path $tmpTc 'xtensa-lx106-elf') $tcDest
Remove-Item -Recurse -Force $tmpSdk, $tmpTc -ErrorAction SilentlyContinue

$esptoolSrc = Join-Path $RtosSdkPath 'components\esptool_py\esptool\esptool.py'
if (-not (Test-Path -LiteralPath $esptoolSrc)) {
    throw "esptool.py not found at $esptoolSrc. Pass -RtosSdkPath to point at your ESP8266 RTOS SDK."
}
New-Item -ItemType Directory -Force -Path (Join-Path $EspRoot 'esptool') | Out-Null
Copy-Item $esptoolSrc (Join-Path $EspRoot 'esptool\esptool.py') -Force

Write-Host 'Verifying ...' -ForegroundColor Cyan
& (Join-Path $tcDest 'bin\xtensa-lx106-elf-gcc.exe') --version | Select-Object -First 1
foreach ($need in @(
        'xtensa-lx106-elf\lib\gcc\xtensa-lx106-elf\4.8.5\libgcc.a'
        'sdk\ld\eagle.app.v6.ld'
        'sdk\include\espnow.h'
        'sdk\lib\libmain.a'
        'esptool\esptool.py')) {
    if (-not (Test-Path -LiteralPath (Join-Path $EspRoot $need))) { throw "Missing after install: $need" }
    Write-Host "  ok  $need" -ForegroundColor DarkGray
}

Write-Host "Installed into $EspRoot" -ForegroundColor Green
Write-Host 'Next: .\build.ps1 -Flash -Port COM6' -ForegroundColor Green
