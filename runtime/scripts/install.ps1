# SPDX-License-Identifier: GPL-3.0-or-later
# Compila o nxrt na máquina do usuário, a partir do export do recompilador dele.
# O binário resultante contém código do jogo: não distribuir.
# Equivalente em PowerShell de scripts/install.sh (PowerShell 5.1 ou 7+).
# AVISO: o CMakeLists.txt contém caminhos condicionais para Windows, mas nenhum build
# Windows ou execução do runtime foi verificado (ver docs/INSTALL.md).
[CmdletBinding()]
param(
    [string]$GameExport  = $env:NXRT_GAME_EXPORT,
    [string]$RecompBuild = $env:NXRT_RECOMP_BUILD,
    [string]$BoostRoot   = $env:NXRT_BOOST_ROOT,
    [string]$Prefix,
    [string]$BuildDir,
    [int]$Jobs = [Math]::Min([Environment]::ProcessorCount, 2),
    [switch]$Help
)
$ErrorActionPreference = 'Stop'

$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $Prefix)   { $Prefix   = Join-Path $env:LOCALAPPDATA 'nxrt' }
if (-not $BuildDir) { $BuildDir = Join-Path $Root 'build-install' }

function Die([string]$msg) { [Console]::Error.WriteLine("erro: $msg"); exit 1 }

if ($Help) {
@'
uso: scripts\install.ps1 -GameExport DIR -RecompBuild DIR -BoostRoot DIR [opções]

  -GameExport DIR   exefs\ do export do recompilador (contém recomp_registration.c)
  -RecompBuild DIR  diretório 'recomp' do build estático (contém rtld\, main\, sdk\)
  -BoostRoot DIR    árvore de fonte do Boost (contém libs\*\include)
  -Prefix DIR       destino da instalação (padrão: %LOCALAPPDATA%\nxrt)
  -BuildDir DIR     diretório de build (padrão: build-install\ na raiz do repo)
  -Jobs N           paralelismo do build (padrão: até 2, para reduzir uso de RAM)
  -Help             mostra esta ajuda

Também aceitos por variável de ambiente: NXRT_GAME_EXPORT, NXRT_RECOMP_BUILD, NXRT_BOOST_ROOT.
'@
    exit 0
}

foreach ($tool in 'cmake', 'ninja', 'clang', 'clang++') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { Die "ferramenta ausente no PATH: $tool" }
}

if (-not $GameExport)  { Die 'informe -GameExport (ou NXRT_GAME_EXPORT)' }
if (-not $RecompBuild) { Die 'informe -RecompBuild (ou NXRT_RECOMP_BUILD)' }
if (-not $BoostRoot)   { Die 'informe -BoostRoot (ou NXRT_BOOST_ROOT)' }
if (-not (Test-Path -LiteralPath $GameExport -PathType Container))  { Die "export não é um diretório: $GameExport" }
if (-not (Test-Path -LiteralPath $RecompBuild -PathType Container)) { Die "build do recompilador não é um diretório: $RecompBuild" }
if (-not (Test-Path -LiteralPath $BoostRoot -PathType Container))   { Die "Boost não é um diretório: $BoostRoot" }
if ($Jobs -lt 1) { Die '-Jobs deve ser um inteiro positivo' }

$GameExport  = (Resolve-Path -LiteralPath $GameExport).Path
$RecompBuild = (Resolve-Path -LiteralPath $RecompBuild).Path
$BoostRoot   = (Resolve-Path -LiteralPath $BoostRoot).Path

if (-not (Test-Path -LiteralPath (Join-Path $GameExport 'recomp_registration.c') -PathType Leaf)) {
    Die "export inválido: $(Join-Path $GameExport 'recomp_registration.c') não existe"
}

# Aceita .a (toolchain GNU) ou .lib (MSVC) para cada biblioteca.
$gameLibs = @()
foreach ($rel in 'rtld/librecomp_static_rtld', 'main/librecomp_static_main',
                 'sdk/librecomp_static_sdk', 'main/librecomp_runtime_shared') {
    $found = $null
    foreach ($ext in '.a', '.lib') {
        $candidate = Join-Path $RecompBuild ($rel + $ext)
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $found = $candidate; break }
    }
    if (-not $found) { Die "biblioteca ausente: $(Join-Path $RecompBuild $rel).a (ou .lib)" }
    $gameLibs += $found
}

$boostInc = Get-ChildItem -Path (Join-Path $BoostRoot 'libs') -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'include') -PathType Container }
if (-not $boostInc) { Die "Boost inválido: nenhum libs\*\include em $BoostRoot" }

function Invoke-Checked([string]$what, [scriptblock]$cmd) {
    & $cmd
    if ($LASTEXITCODE -ne 0) { Die "$what falhou (código $LASTEXITCODE)" }
}

Write-Host "==> configurando em $BuildDir"
Invoke-Checked 'cmake (configuração)' {
    cmake -S $Root -B $BuildDir -G Ninja `
        -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ `
        "-DNXRT_GAME_EXPORT=$GameExport" `
        "-DNXRT_GAME_LIBS=$($gameLibs -join ';')" `
        "-DNXRT_BOOST_ROOT=$BoostRoot"
}

Write-Host '==> compilando nxrt'
Invoke-Checked 'cmake (compilação)' { cmake --build $BuildDir --target nxrt -j $Jobs }

$exe = Join-Path $BuildDir 'nxrt.exe'
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { $exe = Join-Path $BuildDir 'nxrt' }
if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) { Die "binário não encontrado em $BuildDir" }

Write-Host "==> instalando em $Prefix"
$fonts = @(Get-ChildItem -Path (Join-Path $Root 'third_party/suyu/fonts') -Filter '*.ttf' -File -ErrorAction SilentlyContinue)
if ($fonts.Count -eq 0) { Die "nenhuma fonte .ttf encontrada em $(Join-Path $Root 'third_party/suyu/fonts')" }

# Espaço livre (bytes) no volume que conterá o destino; sobe até um ancestral existente.
function Get-FreeBytes([string]$dir) {
    $d = [System.IO.Path]::GetFullPath($dir)
    while (-not (Test-Path -LiteralPath $d -PathType Container)) {
        $parent = Split-Path -Parent $d
        if (-not $parent -or $parent -eq $d) { return -1 }
        $d = $parent
    }
    $rootPath = [System.IO.Path]::GetPathRoot($d)
    if (-not $rootPath -or $rootPath.StartsWith('\\')) { return -1 }  # UNC: sem DriveInfo
    return ([System.IO.DriveInfo]$rootPath).AvailableFreeSpace
}

# Cópias vão para arquivos temporários no mesmo diretório do destino e só entram no
# lugar com rename (File.Replace/Move no mesmo volume). Falha ou interrupção nunca
# deixa arquivo parcial no destino, e uma instalação anterior fica intacta.
function Install-Atomic([string]$src, [string]$dest) {
    $tmp = "$dest.tmp.$([Guid]::NewGuid().ToString('N').Substring(0, 8))"
    try {
        Copy-Item -LiteralPath $src -Destination $tmp -Force
        if (Test-Path -LiteralPath $dest -PathType Leaf) {
            [System.IO.File]::Replace($tmp, $dest, $null)
        } else {
            [System.IO.File]::Move($tmp, $dest)
        }
    } catch {
        Die "falha ao instalar ${dest}: $($_.Exception.Message) (instalação anterior intacta)"
    } finally {
        if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue }
    }
}

$needBytes = (Get-Item -LiteralPath $exe).Length
foreach ($f in $fonts) { $needBytes += $f.Length }
# margem de 16 MiB; o temporário coexiste com o binário antigo, então conta o tamanho inteiro.
$needMiB = [math]::Ceiling($needBytes / 1MB) + 16
$binDir = Join-Path $Prefix 'bin'
$free = Get-FreeBytes $binDir
if ($free -lt 0) { Die "não foi possível medir o espaço livre em $Prefix" }
if ($free -lt $needMiB * 1MB) {
    Die ("espaço insuficiente em ${Prefix}: necessário {0} MiB, livre {1} MiB (nada foi copiado)" -f $needMiB, [math]::Floor($free / 1MB))
}

$fontDir = Join-Path $binDir 'fonts'
New-Item -ItemType Directory -Force -Path $binDir, $fontDir | Out-Null
Install-Atomic $exe (Join-Path $binDir 'nxrt.exe')
# o runtime procura fonts\ ao lado do executável.
foreach ($f in $fonts) { Install-Atomic $f.FullName (Join-Path $fontDir $f.Name) }

Write-Host "pronto: $(Join-Path $binDir 'nxrt.exe')"
Write-Host "fontes: $fontDir"
Write-Host "execute: $(Join-Path $binDir 'nxrt.exe') --game <pasta-do-jogo> --user <pasta-de-saves>"
