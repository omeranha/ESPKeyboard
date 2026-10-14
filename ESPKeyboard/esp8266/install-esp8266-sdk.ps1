param(
    [string]$SdkPath,
    [string]$ToolsPath
)

$ErrorActionPreference = 'Stop'

if (-not $SdkPath) {
    if ($env:IDF_PATH -and (Test-Path -LiteralPath $env:IDF_PATH)) {
        $installRoot = Split-Path -Parent $env:IDF_PATH
    } elseif (Test-Path -LiteralPath 'C:\esp\v6.1\esp-idf') {
        $installRoot = 'C:\esp\v6.1'
    } else {
        $installRoot = Join-Path $HOME 'esp'
    }
    $SdkPath = Join-Path $installRoot 'ESP8266_RTOS_SDK'
}

if (-not $ToolsPath) {
    $ToolsPath = Join-Path (Split-Path -Parent $SdkPath) 'tools-esp8266'
}

$SdkPath = [System.IO.Path]::GetFullPath($SdkPath)
$ToolsPath = [System.IO.Path]::GetFullPath($ToolsPath)

$git = Get-Command git.exe -ErrorAction SilentlyContinue
if (-not $git) {
    throw 'Git for Windows is required. Install Git and rerun this script.'
}

$pythonCommand = Get-Command py.exe -ErrorAction SilentlyContinue
if ($pythonCommand) {
    $pythonPath = & $pythonCommand.Source -3 -c 'import sys; print(sys.executable)'
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not locate Python 3 using the Windows py launcher.'
    }
} else {
    $pythonCommand = Get-Command python.exe -ErrorAction SilentlyContinue
    if (-not $pythonCommand) {
        throw 'Python 3 is required. Install Python and rerun this script.'
    }
    $pythonPath = & $pythonCommand.Source -c 'import sys; print(sys.executable)'
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not locate the Python interpreter.'
    }
}
$pythonPath = ($pythonPath | Select-Object -Last 1).Trim()

if (Test-Path -LiteralPath $SdkPath) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkPath '.git'))) {
        throw "SDK path exists but is not a Git checkout: $SdkPath"
    }
    $sdkTag = & $git.Source -C $SdkPath describe --tags --exact-match HEAD
    if ($LASTEXITCODE -ne 0 -or $sdkTag.Trim() -ne 'v3.4') {
        throw "SDK path must be an ESP8266_RTOS_SDK v3.4 checkout. Existing path left untouched: $SdkPath"
    }
    Write-Host "Using existing ESP8266 RTOS SDK v3.4 checkout: $SdkPath"
} else {
    $parent = Split-Path -Parent $SdkPath
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    & $git.Source clone --branch v3.4 --depth 1 https://github.com/espressif/ESP8266_RTOS_SDK.git $SdkPath
    if ($LASTEXITCODE -ne 0) {
        throw 'Cloning ESP8266_RTOS_SDK v3.4 failed.'
    }
}

$env:IDF_PATH = $SdkPath
$env:IDF_TOOLS_PATH = $ToolsPath

# Fetch the SDK components used by the build. Skip CoAP's nested TinyDTLS
# submodule; it is optional and its retired upstream URL redirects.
$requiredSubmodules = @(
    'components/json/cJSON',
    'components/lwip/lwip',
    'components/mbedtls/mbedtls',
    'components/mqtt/esp-mqtt'
)
foreach ($submodule in $requiredSubmodules) {
    & $git.Source -C $SdkPath submodule update --init --depth 1 $submodule
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to install SDK dependency submodule: $submodule"
    }
}

$idfTools = Join-Path $SdkPath 'tools\idf_tools.py'
& $pythonPath $idfTools install xtensa-lx106-elf cmake mconf ninja idf-exe ccache
if ($LASTEXITCODE -ne 0) {
    throw 'Installing the ESP8266 compiler and build tools failed.'
}

& $pythonPath $idfTools install-python-env
if ($LASTEXITCODE -ne 0) {
    throw 'Creating the ESP8266 SDK Python environment failed.'
}

$pythonEnvironments = Get-ChildItem -LiteralPath (Join-Path $ToolsPath 'python_env') `
    -Directory -Filter 'rtos3.4_py*_env' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending
if (-not $pythonEnvironments) {
    throw 'The SDK Python environment was not created.'
}
$sdkPython = Join-Path $pythonEnvironments[0].FullName 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $sdkPython)) {
    throw "Python executable not found in the SDK environment: $sdkPython"
}

# SDK v3.4 imports pkg_resources, which was removed from newer setuptools.
& $sdkPython -m pip install 'setuptools<81'
if ($LASTEXITCODE -ne 0) {
    throw 'Installing the SDK-compatible setuptools version failed.'
}

Write-Host ''
Write-Host 'ESP8266 RTOS SDK v3.4.0 is installed.'
Write-Host "SDK:   $SdkPath"
Write-Host "Tools: $ToolsPath"
Write-Host ''
Write-Host 'Build this project with:'
if ($ToolsPath -eq (Join-Path (Split-Path -Parent $SdkPath) 'tools-esp8266')) {
    Write-Host "  .\build.ps1 -SdkPath `"$SdkPath`""
} else {
    Write-Host "  .\build.ps1 -SdkPath `"$SdkPath`" -ToolsPath `"$ToolsPath`""
}
