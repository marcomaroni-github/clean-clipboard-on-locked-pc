$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$srcFile     = Join-Path $projectRoot "src\main.c"
$distDir     = Join-Path $projectRoot "dist"
$outExe      = Join-Path $distDir "CleanClipboardOnLock.exe"
$objFile     = Join-Path $distDir "main.obj"

if (-not (Test-Path $distDir)) {
    New-Item -ItemType Directory -Path $distDir | Out-Null
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe non trovato. Installa Visual Studio C++ Build Tools."
}

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) {
    throw "Compilatore MSVC C/C++ (VC.Tools.x86.x64) non trovato."
}

$vcvars = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"

# Compile pure Win32 C without CRT (/Zl /GS- /NODEFAULTLIB) for minimum binary & RAM footprint
$compileCmd = "`"$vcvars`" >nul && cl.exe /nologo /W4 /O1 /Os /GF /Gy /GS- /Zl /TC `"$srcFile`" /Fo`"$objFile`" /Fe`"$outExe`" /link /NOLOGO /NODEFAULTLIB /SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup /OPT:REF /OPT:ICF /INCREMENTAL:NO kernel32.lib user32.lib advapi32.lib wtsapi32.lib"

Write-Host "Compilazione di CleanClipboardOnLock.exe in corso..." -ForegroundColor Cyan
cmd.exe /c $compileCmd
if ($LASTEXITCODE -ne 0) {
    throw "Compilazione fallita con codice $LASTEXITCODE"
}

if (Test-Path $objFile) {
    Remove-Item $objFile -Force
}

$sizeBytes = (Get-Item $outExe).Length
$sizeKB    = [math]::Round($sizeBytes / 1KB, 2)
Write-Host "Compilazione completata: $outExe ($sizeKB KB / $sizeBytes byte)" -ForegroundColor Green
