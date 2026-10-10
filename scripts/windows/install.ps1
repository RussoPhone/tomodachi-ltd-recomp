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
#   install.ps1 -PatchExe "C:\Program Files\Git\usr\bin\patch.exe"   use this patch.exe (fixes "Could not find patch executable")

[CmdletBinding()]
param([switch]$CI, [string]$PatchExe)

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

# winget is an "App Execution Alias" of the App Installer package: it can be installed and still be missing from
# PATH (an elevated window, another user, an old PATH). Look in every place it can be.
function Find-Winget {
    $c = Get-Command winget -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($c) { return $c.Source }
    $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
    if (Test-Path -LiteralPath $alias) { return $alias }
    foreach ($all in $false, $true) {
        try {
            $pkg = if ($all) { Get-AppxPackage -AllUsers Microsoft.DesktopAppInstaller -ErrorAction Stop }
                   else { Get-AppxPackage Microsoft.DesktopAppInstaller -ErrorAction Stop }
            foreach ($p in @($pkg)) {
                $exe = Join-Path $p.InstallLocation 'winget.exe'
                if (Test-Path -LiteralPath $exe) { return $exe }
            }
        } catch { }
    }
    $found = Get-ChildItem -Path (Join-Path $env:ProgramFiles 'WindowsApps\Microsoft.DesktopAppInstaller_*_x64__8wekyb3d8bbwe\winget.exe') `
        -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) { return $found.FullName }
    return $null
}

$script:Winget = $null
function Get-Winget {
    if (-not $script:Winget) { $script:Winget = Find-Winget }
    if (-not $script:Winget) {
        Fail (@(
            'winget was not found, and some build tools are missing.'
            '  - If winget works in a normal PowerShell window, run install.bat by double-clicking it'
            '    (not "Run as administrator").'
            '  - Otherwise update "App Installer" in the Microsoft Store, or install by hand:'
            '    Git, CMake, Ninja, Python 3.12 (tick "Add to PATH"), LLVM, and Visual Studio 2022'
            '    Build Tools with "Desktop development with C++". Then run install.bat again.'
        ) -join "`n")
    }
    return $script:Winget
}

function Install-Tool ($Id, $Probe, $Override, [switch]$Admin, [string]$File) {
    if ($Probe -and (Test-RealCommand $Probe)) { return }
    if ($File -and (Test-Path -LiteralPath $File)) { return }
    $winget = Get-Winget
    Say "Installing $Id ..."
    $wargs = @('install', '--id', $Id, '--exact', '--source', 'winget', '--silent',
               '--accept-package-agreements', '--accept-source-agreements', '--disable-interactivity')
    if ($Override) { $wargs += @('--override', $Override) }
    if ($Admin) {
        # Visual Studio installs for the whole machine: Windows asks for permission once.
        Start-Process $winget -ArgumentList $wargs -Verb RunAs -Wait
    } else {
        & $winget @wargs | Out-Host
    }
    Refresh-Path
    $ok = (-not $Probe -and -not $File) -or ($Probe -and (Test-RealCommand $Probe)) -or ($File -and (Test-Path -LiteralPath $File))
    if (-not $ok) { Fail "$Id did not install. Install it by hand and run install.bat again." }
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
    Write-Host '> Preparing the build tools (only the first time; can take a while)' -ForegroundColor Yellow
    Install-Tool 'Git.Git' 'git'
    Install-Tool 'Kitware.CMake' 'cmake'
    Install-Tool 'Ninja-build.Ninja' 'ninja'
    Install-Tool 'Python.Python.3.12' 'python' '/quiet InstallAllUsers=0 PrependPath=1 Include_launcher=1 Include_tcltk=1'
    Install-Tool 'LLVM.LLVM' 'clang-cl' -File (Join-Path $env:ProgramFiles 'LLVM\bin\clang-cl.exe')
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
if ($PatchExe) {
    if (-not (Test-Path -LiteralPath $PatchExe -PathType Leaf)) { Fail "PatchExe points to a file that does not exist: $PatchExe" }
    $env:SUYU_PATCH_EXE = (Resolve-Path -LiteralPath $PatchExe).Path
    Say "Using patch.exe: $env:SUYU_PATCH_EXE"
}
$extra = if ($CI) { ' --ci' } else { '' }
& cmd.exe /c "call `"$vcvars`" >nul && python scripts\install.py$extra"
exit $LASTEXITCODE
