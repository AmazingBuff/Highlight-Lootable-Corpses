param(
    [string]$VcVars = '',
    [string]$Repository = (Resolve-Path "$PSScriptRoot/../..").Path
)
$ErrorActionPreference = 'Stop'
$buildPath = Join-Path $Repository 'build/mask_rendering'
New-Item -ItemType Directory -Force $buildPath | Out-Null
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    if (-not $VcVars) {
        $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
        if (Test-Path -LiteralPath $vswhere) {
            $install = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($install) { $VcVars = Join-Path $install 'VC/Auxiliary/Build/vcvars64.bat' }
        }
    }
    if (-not $VcVars -or -not (Test-Path -LiteralPath $VcVars)) {
        throw 'Run in an x64 MSVC developer shell or pass -VcVars with the installed vcvars64.bat path.'
    }
}
$setup = if ($VcVars) { "call `"$VcVars`"`r`nif errorlevel 1 exit /b 1" } else { '' }
$command = @"
@echo off
$setup
cl.exe /nologo /std:c++20 /EHsc /W4 /utf-8 /DNOMINMAX /I"$Repository/src" "$PSScriptRoot/smoke.cpp" /Fe:mask_smoke.exe /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
mask_smoke.exe "$Repository"
exit /b %errorlevel%
"@
$commandPath = Join-Path $buildPath 'run.cmd'
[IO.File]::WriteAllText($commandPath, $command, [Text.Encoding]::Default)
Push-Location $buildPath
try {
    & cmd.exe /d /c $commandPath
    if ($LASTEXITCODE -ne 0) { throw "Mask smoke failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
