# Ищет установку Qt6 (кит MSVC x64) и печатает путь к ней в stdout.
#
# Qt при установке не прописывает свои киты ни в реестр, ни в переменные
# окружения, поэтому порядок поиска такой:
#   1) qmake в PATH — спрашиваем у него QT_INSTALL_PREFIX;
#   2) сканирование типового места установки <диск>:\Qt\<версия>\msvc*_64
#      по всем локальным дискам; берётся самая новая версия.
#
# Код возврата: 0 — найдено, 1 — нет.

$ErrorActionPreference = 'SilentlyContinue'

function Test-QtKit([string]$path) {
    return (Test-Path (Join-Path $path 'lib\cmake\Qt6\Qt6Config.cmake'))
}

# 1) qmake в PATH
$qmake = Get-Command qmake
if ($qmake) {
    $prefix = & $qmake.Source -query QT_INSTALL_PREFIX
    if ($prefix -and (Test-QtKit $prefix)) {
        Write-Output $prefix
        exit 0
    }
}

# 2) Сканирование <диск>:\Qt\<версия>\msvc*_64
$candidates = @()
foreach ($drive in (Get-PSDrive -PSProvider FileSystem)) {
    $root = Join-Path $drive.Root 'Qt'
    if (-not (Test-Path $root)) { continue }
    foreach ($ver in (Get-ChildItem $root -Directory | Where-Object { $_.Name -match '^\d+(\.\d+)+$' })) {
        foreach ($kit in (Get-ChildItem $ver.FullName -Directory -Filter 'msvc*_64')) {
            if (Test-QtKit $kit.FullName) {
                $candidates += [pscustomobject]@{
                    Version = [version]$ver.Name
                    Path    = $kit.FullName
                }
            }
        }
    }
}

$best = $candidates | Sort-Object Version -Descending | Select-Object -First 1
if ($best) {
    Write-Output $best.Path
    exit 0
}
exit 1
