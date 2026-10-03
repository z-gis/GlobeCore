<#
.SYNOPSIS
    编译 GlobeCore HarmonyOS HAR（release 模式，产出带版本号的 .har）。

.DESCRIPTION
    封装 DevEco 命令行工具链（hvigorw + 内置 node + SDK）的环境变量设置，
    在 globecore-harmonyos 工程上执行 createVersionedHar 任务（内部依赖 assembleHar），
    release 构建后额外复制一份带版本号的文件名，产物落在：
        globecore-harmonyos\library\build\default\outputs\default\@zys-globecore-<version>.har

    若本机 DevEco 安装路径不同，可用环境变量覆盖：
        $env:DEVECO_HOME   DevEco Studio 安装根目录（默认 D:\Program Files\Huawei\DevEco Studio）
        $env:DEVECO_SDK_HOME / $env:NODE_HOME  显式指定时优先于从 DEVECO_HOME 推导

.PARAMETER Clean
    先执行 hvigorw clean 再构建。

.PARAMETER BuildMode
    构建模式，默认 release（发布必须用 release，debug 有源码泄露风险）。可指定 debug 用于本地调试。

.EXAMPLE
    .\build-har.ps1
    .\build-har.ps1 -Clean
    .\build-har.ps1 -BuildMode debug
#>
[CmdletBinding()]
param(
    [switch]$Clean,
    [ValidateSet('release', 'debug')]
    [string]$BuildMode = 'release'
)

$ErrorActionPreference = 'Stop'

# 仓库根 = 本脚本所在目录；鸿蒙工程在其下的 globecore-harmonyos
$RepoRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$HarmonyDir = Join-Path $RepoRoot 'globecore-harmonyos'
if (-not (Test-Path $HarmonyDir)) {
    throw "未找到鸿蒙工程目录：$HarmonyDir"
}

# DevEco 工具链路径（允许用 DEVECO_HOME 覆盖）
$DevecoHome = if ($env:DEVECO_HOME) { $env:DEVECO_HOME } else { 'D:\Program Files\Huawei\DevEco Studio' }
$Hvigorw    = Join-Path $DevecoHome 'tools\hvigor\bin\hvigorw.bat'
$NodeDir    = Join-Path $DevecoHome 'tools\node'
$SdkDir     = Join-Path $DevecoHome 'sdk'

if (-not (Test-Path $Hvigorw)) {
    throw "未找到 hvigorw：$Hvigorw`n请确认 DevEco Studio 已安装，或设置 `$env:DEVECO_HOME 指向安装目录。"
}

# 环境变量：hvigorw 依赖 NODE_HOME 与 DEVECO_SDK_HOME，缺失会报 'NODE_HOME is not set' 并退出码 1
if (-not $env:NODE_HOME -and (Test-Path $NodeDir)) { $env:NODE_HOME = $NodeDir }
if (-not $env:DEVECO_SDK_HOME -and (Test-Path $SdkDir)) { $env:DEVECO_SDK_HOME = $SdkDir }
if (-not $env:NODE_HOME) { throw "未找到 node 目录且未设置 `$env:NODE_HOME：$NodeDir" }
if (-not $env:DEVECO_SDK_HOME) { throw "未找到 SDK 目录且未设置 `$env:DEVECO_SDK_HOME：$SdkDir" }
$env:PATH = "$env:NODE_HOME;$(Split-Path $Hvigorw);$env:PATH"

Push-Location $HarmonyDir
try {
    if ($Clean) {
        Write-Host '>> hvigorw clean' -ForegroundColor Cyan
        & $Hvigorw clean --no-daemon
        if ($LASTEXITCODE -ne 0) { throw "hvigorw clean 失败（退出码 $LASTEXITCODE）" }
    }

    Write-Host ">> 编译 HAR（$BuildMode 模式，任务 createVersionedHar）" -ForegroundColor Cyan
    & $Hvigorw --mode module `
        -p module=globecore@default `
        -p product=default `
        -p "buildMode=$BuildMode" `
        createVersionedHar --no-daemon
    if ($LASTEXITCODE -ne 0) { throw "HAR 构建失败（退出码 $LASTEXITCODE）" }
}
finally {
    Pop-Location
}

# 列出产物
$outDir = Join-Path $HarmonyDir 'library\build\default\outputs\default'
Write-Host "`n>> 构建成功，产物目录：$outDir" -ForegroundColor Green
Get-ChildItem $outDir -Filter *.har -ErrorAction SilentlyContinue |
    Select-Object Name, @{N = 'KB'; E = { [math]::Round($_.Length / 1KB, 1) } }, LastWriteTime |
    Format-Table -AutoSize
