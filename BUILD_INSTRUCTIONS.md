在本仓库中构建并生成可烧录的 bin 镜像的步骤（Windows / PowerShell）

先决条件
- Git
- Python 3.8+（推荐 3.10）
- Visual Studio Build Tools（含 C/C++ 编译工具）
- 充足的磁盘空间与网络访问

快速安装 ESP-IDF（推荐使用官方脚本）
1. 打开 PowerShell（管理员）
2. 克隆 esp-idf（以 v5.x 为例，支持 ESP32-C3）

```powershell
git clone --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
# 指定分支，例如 v5.0
git checkout v5.2
git submodule update --init --recursive
```

3. 运行 Windows 安装脚本（会安装 Python 包和必要工具）：

```powershell
.	ools\install.ps1
```

4. 运行环境变量设置脚本（每次新终端需执行，或把其加入用户环境变量）：

```powershell
.	ools\export.ps1
```

构建本项目
1. 打开已加载 ESP-IDF 环境的 PowerShell（执行过 export.ps1）
2. 进入项目根目录（含 CMakeLists.txt）

```powershell
cd E:\LLM\ai-passport-study-main\ai-passport-study-main
```

3. 设置目标为 `esp32c3`（仅需一次）

```powershell
idf.py set-target esp32c3
```

4. 构建并生成 bin

```powershell
idf.py build
```

生成物
- 构建产物位于 `build/`，其中包含 `*.bin`、`partition-table.bin`、`bootloader.bin` 等。
- 我在仓库添加了 `tools/build_idf.ps1` 用于自动化检查 idf.py、构建并把 bin 复制到 `artifacts/` 目录：

```powershell
# 在已加载 IDF 环境的终端运行
powershell -ExecutionPolicy Bypass -File .\tools\build_idf.ps1 -Port COM3
```

如果你希望我在当前环境直接构建并打包 bin，请先在本环境中提供可执行的 `idf.py`（加入 PATH 或告诉我 idf.py 的完整路径），我就能替你运行 `idf.py build` 并返回 `artifacts/` 下的 bin。