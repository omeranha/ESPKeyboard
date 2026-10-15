<#
Builds and flashes the NONOS receiver on Windows. No make, MSYS2 or WSL.

The upstream espusb Makefile compiles and links the whole firmware in a single
compiler invocation, so this script expands the same flags and runs that one
command, then esptool's elf2image, then write_flash. Outputs land in build/.

Requires the layout below under -EspRoot, which install-sdk.ps1 creates:
    xtensa-lx106-elf/   GCC 4.8.5 cross toolchain
    sdk/                ESP8266 NONOS SDK 1.5.4   (nothing newer works)
    esptool/esptool.py

Examples:
    .\build.ps1
    .\build.ps1 -Flash -Port COM6
#>
param(
    [string]$EspRoot    = 'C:\esp\espusb-sdk',
    [string]$GccVersion = '4.8.5',
    [switch]$Flash,
    [string]$Port,
    # 460800 fails to enter the bootloader on this board; 115200 is reliable.
    [int]$BurnBaud = 115200,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'

function To-Posix([string]$p) { return ($p -replace '\\', '/') }

if ($Flash -and -not $Port) { throw 'Specify -Port when using -Flash, for example -Flash -Port COM6.' }

$project   = $PSScriptRoot
$buildDir  = Join-Path $project 'build'
$root      = (Resolve-Path -LiteralPath $EspRoot).Path
if ($project -match ' ' -or $root -match ' ') {
    throw 'This build does not support paths containing spaces.'
}

$gccDir   = Join-Path $root 'xtensa-lx106-elf'
$cc       = Join-Path $gccDir 'bin\xtensa-lx106-elf-gcc.exe'
$nm       = Join-Path $gccDir 'bin\xtensa-lx106-elf-nm.exe'
$sizeTool = Join-Path $gccDir 'bin\xtensa-lx106-elf-size.exe'
$sdk      = Join-Path $root 'sdk'
$xtLib    = Join-Path $sdk 'lib'
$xtGccLib = Join-Path $gccDir "lib\gcc\xtensa-lx106-elf\$GccVersion\libgcc.a"
$linkScr  = Join-Path $sdk 'ld\eagle.app.v6.ld'
$esptool  = Join-Path $root 'esptool\esptool.py'

foreach ($required in @($cc, $xtGccLib, $linkScr, $esptool)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing build input: $required`nRun install-sdk.ps1 first."
    }
}

if ($Clean) {
    Remove-Item -Recurse -Force -LiteralPath $buildDir -ErrorAction SilentlyContinue
    Write-Host 'Cleaned build/.' -ForegroundColor Green
    if (-not $Flash) { return }
}
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

$elf = 'build/image.elf'
$fw1 = 'build/image.elf-0x00000.bin'
$fw2 = 'build/image.elf-0x40000.bin'

$srcs = @(
    'src/user_main.c'
    'src/usb.c'
    'src/usb_asm_1bit.S'
)

$includes = @((To-Posix (Join-Path $sdk 'include')), 'src', '.')

# LIBS in the order upstream links them (lwip really is listed twice), plus
# espnow for the radio link.
$libs = @('main', 'lwip', 'ssl', 'upgrade', 'net80211', 'wpa', 'phy', 'lwip', 'crypto', 'pp', 'espnow')

$ccArgs = @('-mlongcalls', '-Os', '-std=gnu99', '-Wall', '-DICACHE_FLASH')
# PIN_FUNC_SELECT in the SDK's eagle_soc.h mixes << and | without parentheses,
# which -Wall reports at every call site. Silence just that so real warnings
# from our own code stay visible.
$ccArgs += '-Wno-parentheses'
$ccArgs += ($includes | ForEach-Object { "-I$_" })
$ccArgs += $srcs
$ccArgs += '-flto'
$ccArgs += @('-Wl,--relax', '-Wl,--gc-sections', '-nostdlib')
$ccArgs += "-L$(To-Posix $xtLib)"
$ccArgs += ($libs | ForEach-Object { To-Posix (Join-Path $xtLib "lib$_.a") })
$ccArgs += (To-Posix $xtGccLib)
$ccArgs += @('-T', (To-Posix $linkScr))
$ccArgs += "-B$(To-Posix $xtLib)"
$ccArgs += @('-o', $elf)

Push-Location $project
try {
    $responseFile = Join-Path $buildDir 'build.rsp'
    Set-Content -LiteralPath $responseFile -Value $ccArgs -Encoding ASCII

    Write-Host "Compiling and linking with GCC $GccVersion ..." -ForegroundColor Cyan
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $cc "@$responseFile" 2>&1 | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    Remove-Item -LiteralPath $responseFile -Force -ErrorAction SilentlyContinue
    if ($code -ne 0) { throw "Compile/link failed with exit code $code." }

    # The USB interrupt may not touch flash. Confirm the handler, the C routines
    # the assembly jumps to, and the tables they read all landed in IRAM
    # (0x4010xxxx) and DRAM (0x3ffexxxx). The descriptors are "static const", so
    # LTO may rename or localise them; check the linker's .rodata bounds
    # instead, which is the property that actually matters -- if .rodata were
    # flash-mapped, every descriptor byte read from the interrupt would trap
    # into the load/store emulation handler.
    $symbols = & $nm -n $elf
    foreach ($check in @(
            @{ Name = 'gpio_intr';            Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_reinstate';        Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_send_data';        Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_setup'; Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_sof';   Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_in';    Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_out';   Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_data';  Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_pid_handle_ack';   Prefix = '4010'; Where = 'IRAM' }
            @{ Name = 'usb_ramtable';         Prefix = '3ffe'; Where = 'DRAM' }
            @{ Name = 'usb_internal_state';   Prefix = '3ffe'; Where = 'DRAM' }
            @{ Name = '_rodata_start';        Prefix = '3ffe'; Where = 'DRAM' }
            @{ Name = '_rodata_end';          Prefix = '3ffe'; Where = 'DRAM' })) {
        $line = $symbols | Select-String -Pattern "\s$($check.Name)$" | Select-Object -First 1
        if (-not $line) { throw "Symbol $($check.Name) not found in $elf." }
        $addr = ($line.Line -split '\s+')[0]
        if (-not $addr.StartsWith($check.Prefix)) {
            throw "$($check.Name) is at 0x$addr but must be in $($check.Where) (0x$($check.Prefix)....)."
        }
        Write-Host ("  {0,-20} 0x{1}  {2}" -f $check.Name, $addr, $check.Where) -ForegroundColor DarkGray
    }

    # IRAM is the scarce resource: anything off the interrupt path belongs in
    # flash, so report the headroom rather than waiting for a link failure.
    if (Test-Path -LiteralPath $sizeTool) {
        $textSize = ((& $sizeTool -A $elf | Select-String -Pattern '^\.text').Line -split '\s+')[1]
        if ($textSize) {
            $free = 32768 - [int]$textSize
            $msg = "  IRAM .text{0,14} / 32768 bytes ({1} free)" -f $textSize, $free
            if ($free -lt 1024) { Write-Host $msg -ForegroundColor Yellow }
            else { Write-Host $msg -ForegroundColor DarkGray }
        }
    }

    $python = Get-ChildItem -Path 'C:\esp\v6.1\tools-esp8266\python_env' -Directory -Filter 'rtos3.4_py*_env' `
        -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending |
        ForEach-Object { Join-Path $_.FullName 'Scripts\python.exe' } | Where-Object { Test-Path $_ } |
        Select-Object -First 1
    if (-not $python) { $python = (Get-Command python -ErrorAction Stop).Source }

    Write-Host 'Converting ELF to flash images ...' -ForegroundColor Cyan
    $ErrorActionPreference = 'Continue'
    & $python $esptool elf2image $elf 2>&1 | ForEach-Object { Write-Host $_ }
    $code = $LASTEXITCODE
    $ErrorActionPreference = $previous
    if ($code -ne 0) { throw "elf2image failed with exit code $code." }

    # esptool 2.4 writes the boot image as "image.elf-", dropping the
    # "0x00000.bin" suffix esptool 1.x used. Normalise it so the flash offsets
    # stay obvious.
    if (-not (Test-Path -LiteralPath $fw1) -and (Test-Path -LiteralPath 'build/image.elf-')) {
        Move-Item -LiteralPath 'build/image.elf-' -Destination $fw1 -Force
    }
    foreach ($f in @($fw1, $fw2)) {
        if (-not (Test-Path -LiteralPath $f)) { throw "elf2image did not produce $f." }
    }
    $magic = [System.IO.File]::ReadAllBytes((Join-Path $project $fw1))[0]
    if ($magic -ne 0xE9) { throw ("$fw1 does not start with the 0xE9 image magic (got 0x{0:x2})." -f $magic) }
    Get-Item $fw1, $fw2 | Select-Object Length, Name | Format-Table | Out-String | Write-Host

    if ($Flash) {
        Write-Host "Flashing $Port at $BurnBaud ..." -ForegroundColor Cyan
        $ErrorActionPreference = 'Continue'
        & $python $esptool -b $BurnBaud --port $Port write_flash -fm dio 0x00000 $fw1 0x40000 $fw2 2>&1 | ForEach-Object { Write-Host $_ }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $previous
        if ($code -ne 0) { throw "Flashing failed with exit code $code." }
    }

    Write-Host 'Done.' -ForegroundColor Green
} finally {
    Pop-Location
}
