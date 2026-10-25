# Wrapper around the locally installed ESP-IDF 6.0 toolchain.
# Same layout as D:\src\appsDM\testspi, but with an explicit Python from the
# ESP-IDF virtualenv (the machine-wide "python" is a Microsoft Store stub).
#
#   .\idf.ps1 --version
#   .\idf.ps1 set-target esp32c3
#   .\idf.ps1 build
#   .\idf.ps1 -p COM6 flash
#   .\idf.ps1 -p COM6 monitor
#   .\idf.ps1 -p COM6 flash monitor
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $IdfArgs
)

$ErrorActionPreference = 'Continue'

$IdfPath   = 'C:\Users\User\esp\v6.0\esp-idf'
$ToolsPath = 'C:\Users\User\.espressif'
$IdfPython = Join-Path $ToolsPath 'python_env\idf6.0_py3.11_env\Scripts\python.exe'

if (-not (Test-Path $IdfPython)) {
    Write-Error "ESP-IDF python not found: $IdfPython"
    exit 1
}

$env:IDF_PATH = $IdfPath
$env:IDF_TOOLS_PATH = $ToolsPath

# activate.py --export prints PowerShell code that sets PATH, IDF_PYTHON_ENV_PATH
# and the toolchain variables; dot-source it just like export.ps1 does.
$exports = & $IdfPython (Join-Path $IdfPath 'tools\activate.py') --export 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Error 'activate.py --export failed'
    exit $LASTEXITCODE
}
. $exports

& $IdfPython (Join-Path $IdfPath 'tools\idf.py') @IdfArgs
exit $LASTEXITCODE
