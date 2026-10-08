# Windows entry point of the installer (started by install.bat).
#
# 1. Installs the build tools with winget (Git, CMake, Ninja, Python, LLVM, Visual Studio 2022 Build Tools),
#    the same set as upstream mk8-recomp's scripts/setup-windows.ps1.
# 2. Runs scripts/install.py inside the Visual Studio build environment. That script does the 10 steps,
#    exactly as on Linux.
#
# Nothing here downloads a game, keys or firmware.
#
#   install.bat            normal install
#   install.ps1 -CI        build check without a game (GitHub Actions; the tools are preinstalled there)

[CmdletBinding()]
param([switch]$CI)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location -LiteralPath $Root

function Say ($m)  { Write-Host "   $m" }
function Fail ($m) {
    Write-Host "`n  X $m" -ForegroundColor Red
    Write-Host '  Fix the problem above and run install.bat again: it resumes where it stopped.'
    exit 1
}
function Refresh-Path {
    $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path', 'User')
}

# Windows ships zero-byte "App Execution Alias" stubs (python.exe opens the Store); they are not real commands.
function Test-RealCommand ($Name) {
    $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $c) { return $false }
    try {
        $i = Get-Item -LiteralPath $c.Source -Force
        if ($i.Length -eq 0 -and $c.Source -like '*\WindowsApps\*') { return $false }
    } catch { }
    return $true
}

function Install-Tool ($Id, $Probe, $Override, [switch]$Admin) {
    if ($Probe -and (Test-RealCommand $Probe)) { return }
    Say "Installing $Id ..."
    $wargs = @('install', '--id', $Id, '--exact', '--source', 'winget', '--silent',
               '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
    if ($Override) { $wargs += @('--override', $Override) }
    if ($Admin) {
        # Visual Studio installs for the whole machine: Windows asks for permission once.
        Start-Process winget -ArgumentList $wargs -Verb RunAs -Wait
    } else {
        & winget @wargs | Out-Host
    }
    Refresh-Path
    if ($Probe -and -not (Test-RealCommand $Probe)) { Fail "$Id did not install. Install it by hand and run install.bat again." }
}

function Find-VcVars {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath | Select-Object -First 1
    if (-not $vs) { return $null }
    $bat = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (Test-Path -LiteralPath $bat) { return $bat }
    return $null
}

Write-Host 'Tomodachi Life: Living the Dream - native build installer (Windows)' -ForegroundColor White
Write-Host 'Educational, experimental project. Use only with your own copy of the game.'
Write-Host ''

if (-not [Environment]::Is64BitOperatingSystem) { Fail 'A 64-bit Windows is required.' }
# Build folders nest deeply; a long starting path runs into the 260-character limit of Windows tools.
if ($Root.Length -gt 40) {
    Write-Host "   This folder's path is long: $Root" -ForegroundColor Yellow
    Write-Host '   Windows build tools can fail on long paths. Moving the folder somewhere short, like C:\tomodachi,' -ForegroundColor Yellow
    Write-Host '   avoids that.' -ForegroundColor Yellow
    if (-not $CI) {
        $answer = Read-Host '   Continue here anyway? [y/N]'
        if ($answer -notin 'y', 'yes', 's', 'sim') { exit 1 }
    }
}

if (-not $CI) {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Fail 'winget was not found. Install "App Installer" from the Microsoft Store (or update Windows), then run install.bat again.'
    }
    Write-Host '> Preparing the build tools (only the first time; can take a while)' -ForegroundColor Yellow
    Install-Tool 'Git.Git' 'git'
    Install-Tool 'Kitware.CMake' 'cmake'
    Install-Tool 'Ninja-build.Ninja' 'ninja'
    Install-Tool 'Python.Python.3.12' 'python' '/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1 Include_tcltk=1'
    Install-Tool 'LLVM.LLVM' $null
    if (-not (Find-VcVars)) {
        Say 'Installing Visual Studio 2022 Build Tools (several GB; Windows will ask for permission) ...'
        Install-Tool 'Microsoft.VisualStudio.2022.BuildTools' $null `
            '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended' -Admin
    }
}

$llvm = Join-Path $env:ProgramFiles 'LLVM\bin'
if (Test-Path -LiteralPath (Join-Path $llvm 'clang-cl.exe')) { $env:Path = "$llvm;$env:Path" }
$vcvars = Find-VcVars
if (-not $vcvars) { Fail 'Visual Studio 2022 with "Desktop development with C++" was not found.' }

& python -m pip install --quiet --disable-pip-version-check --user cryptography
if ($LASTEXITCODE -ne 0) { Fail 'Could not install the Python package "cryptography".' }

$env:PYTHONUTF8 = '1'
$extra = if ($CI) { ' --ci' } else { '' }
& cmd.exe /c "call `"$vcvars`" >nul && python scripts\install.py$extra"
exit $LASTEXITCODE
