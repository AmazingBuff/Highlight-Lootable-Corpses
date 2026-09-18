param(
    [string]$VcVars = '',
    [string]$Repository = (Resolve-Path "$PSScriptRoot/../..").Path
)
$ErrorActionPreference = 'Stop'
$buildPath = Join-Path $Repository 'build/icon_rendering'
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
cl.exe /nologo /std:c++20 /EHsc /W4 /WX /utf-8 /DNOMINMAX /I"$Repository/src" "$PSScriptRoot/smoke.cpp" "$PSScriptRoot/gpu.cpp" "$Repository/src/render/icon/icon_layout.cpp" "$Repository/src/render/icon/icon_geometry.cpp" /Fe:icon_smoke.exe /link d3d11.lib d3dcompiler.lib
if errorlevel 1 exit /b 1
icon_smoke.exe "$Repository"
exit /b %errorlevel%
"@
$commandPath = Join-Path $buildPath 'run.cmd'
[IO.File]::WriteAllText($commandPath, $command, [Text.Encoding]::Default)
Push-Location $buildPath
try {
    & cmd.exe /d /c $commandPath
    if ($LASTEXITCODE -ne 0) { throw "Icon smoke failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
Add-Type -AssemblyName System.Drawing
$bitmap = [Drawing.Bitmap]::new((Join-Path $buildPath 'preview.bmp'))
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$font = [Drawing.Font]::new('Segoe UI', 11)
$brush = [Drawing.SolidBrush]::new([Drawing.Color]::White)
try {
    $graphics.DrawString('Synthetic preview - production vertices + HLSL, WARP (not Skyrim)', $font, $brush, 12, 8)
    $labels = @('Single near', 'Single far', 'Group near', 'Group far')
    for ($column=0; $column -lt 4; $column++) { $graphics.DrawString($labels[$column], $font, $brush, (175 + $column*155), 40) }
    for ($row=0; $row -lt 3; $row++) { $graphics.DrawString(('Base size ' + @(5,10,20)[$row]), $font, $brush, 12, (115+$row*120)) }
    $bitmap.Save((Join-Path $buildPath 'preview.png'), [Drawing.Imaging.ImageFormat]::Png)
} finally {
    $brush.Dispose(); $font.Dispose(); $graphics.Dispose(); $bitmap.Dispose()
}
Write-Output "Preview: $buildPath/preview.png"
