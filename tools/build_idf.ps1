# Windows PowerShell 自动化：检查 ESP-IDF 环境并构建项目
# 使用前请在管理员权限的 PowerShell 中运行或在已安装并配置好 ESP-IDF 的终端运行

param(
    [string]$IdfPyPath = "idf.py",       # 若 idf.py 未在 PATH，可传入完整路径
    [string]$Port = "COM3"               # Flash 串口（如需要 flash）
)

function Check-Command($cmd) {
    $proc = Get-Command $cmd -ErrorAction SilentlyContinue
    return $null -ne $proc
}

Write-Host "== 项目自动构建脚本 =="
if (-not (Check-Command $IdfPyPath)) {
    Write-Host "未检测到 idf.py 在 PATH。请先安装并配置 ESP-IDF。参考 BUILD.md。" -ForegroundColor Yellow
    exit 2
}

# 确保工作目录为项目根
$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $repoRoot
Write-Host "工作目录: $repoRoot"

# 可选：设置目标为 esp32c3
Write-Host "设置 target 为 esp32c3..."
& $IdfPyPath set-target esp32c3
if ($LASTEXITCODE -ne 0) { Write-Host "set-target 失败" -ForegroundColor Red; exit $LASTEXITCODE }

# 构建
Write-Host "开始构建... (idf.py build)"
& $IdfPyPath build
if ($LASTEXITCODE -ne 0) { Write-Host "构建失败" -ForegroundColor Red; exit $LASTEXITCODE }

# 归档生成物
$buildDir = Join-Path $repoRoot "build"
$artDir = Join-Path $repoRoot "artifacts"
if (-not (Test-Path $artDir)) { New-Item -ItemType Directory -Path $artDir | Out-Null }
Get-ChildItem -Path $buildDir -Filter "*.bin" -Recurse | ForEach-Object {
    Copy-Item $_.FullName -Destination $artDir -Force
}
Write-Host "构建成功，bin 文件已复制到: $artDir" -ForegroundColor Green

# 可选：立即刷写
if ($PSBoundParameters.ContainsKey('Port')) {
    Write-Host "开始刷写到设备 ($Port)..."
    & $IdfPyPath -p $Port flash
}

Write-Host "完成。" -ForegroundColor Cyan
