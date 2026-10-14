param(
    [string]$SdkPath,
    [string]$ToolsPath,
    [switch]$Flash,
    [switch]$Menuconfig,
    [string]$Port
)

$ErrorActionPreference = 'Stop'
if ($Flash -and $Menuconfig) {
    throw 'Choose either -Flash or -Menuconfig for one invocation.'
}

if (-not $SdkPath) {
    if ($env:ESP8266_RTOS_SDK_PATH) {
        $SdkPath = $env:ESP8266_RTOS_SDK_PATH
    } elseif ($env:IDF_PATH -and (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $env:IDF_PATH) 'ESP8266_RTOS_SDK'))) {
        $SdkPath = Join-Path (Split-Path -Parent $env:IDF_PATH) 'ESP8266_RTOS_SDK'
    } elseif (Test-Path -LiteralPath 'C:\esp\v6.1\ESP8266_RTOS_SDK') {
        $SdkPath = 'C:\esp\v6.1\ESP8266_RTOS_SDK'
    } else {
        throw 'SDK not found. Run install-esp8266-sdk.ps1 or pass -SdkPath.'
    }
}

$SdkPath = [System.IO.Path]::GetFullPath($SdkPath)
if (-not (Test-Path -LiteralPath (Join-Path $SdkPath 'tools\idf.py'))) {
    throw "idf.py was not found under the SDK path: $SdkPath"
}

$toolsPath = if ($ToolsPath) {
    $ToolsPath
} elseif ($env:ESP8266_RTOS_TOOLS_PATH) {
    $env:ESP8266_RTOS_TOOLS_PATH
} else {
    Join-Path (Split-Path -Parent $SdkPath) 'tools-esp8266'
}
$toolsPath = [System.IO.Path]::GetFullPath($toolsPath)
$pythonEnvironmentPath = Join-Path $toolsPath 'python_env'

$pythonEnvironments = Get-ChildItem -LiteralPath $pythonEnvironmentPath `
    -Directory -Filter 'rtos3.4_py*_env' -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending
if (-not $pythonEnvironments) {
    throw "SDK Python environment not found. Run install-esp8266-sdk.ps1 for: $SdkPath"
}
$env:IDF_PATH = $SdkPath
$env:IDF_TOOLS_PATH = $toolsPath
$env:IDF_PYTHON_ENV_PATH = $pythonEnvironments[0].FullName
$idfTools = Join-Path $SdkPath 'tools\idf_tools.py'
$sdkPython = Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe'
if (-not (Test-Path -LiteralPath $sdkPython)) {
    throw "SDK Python executable not found: $sdkPython"
}
if ($Flash -and -not $Port) {
    throw 'Specify the serial port when flashing, for example -Flash -Port COM3.'
}

# Import the SDK tool paths into this process only; leave the user's ESP-IDF
# environment and persistent PATH unchanged.
$exportOutput = & $sdkPython $idfTools export
if ($LASTEXITCODE -ne 0) {
    throw 'Could not load the ESP8266 SDK tool environment.'
}
foreach ($line in $exportOutput) {
    if ($line -match '^SET "([^=]+)=(.*)"$') {
        $name = $matches[1]
        $value = $matches[2].Replace('%PATH%', $env:PATH)
        Set-Item -Path "Env:$name" -Value $value
    }
}

$idfPy = Join-Path $SdkPath 'tools\idf.py'
Push-Location $PSScriptRoot
try {
    if ($Menuconfig) {
        & $sdkPython $idfPy menuconfig
        if ($LASTEXITCODE -ne 0) {
            throw 'ESP8266 project menuconfig failed.'
        }
        return
    }

    & $sdkPython $idfPy build
    if ($LASTEXITCODE -ne 0) {
        throw 'ESP8266 firmware build failed.'
    }

    if ($Flash) {
        & $sdkPython $idfPy -p $Port flash
        if ($LASTEXITCODE -ne 0) {
            throw "Flashing the firmware to $Port failed."
        }
    }
} finally {
    Pop-Location
}
