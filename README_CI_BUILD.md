远程 CI 构建（GitHub Actions）说明

如果本地没有 ESP-IDF 环境，可以使用仓库内的 GitHub Actions 自动构建固件并下载 bin。

步骤：
1. 把当前仓库 push 到 GitHub（在 `main` 分支）
2. 打开仓库的 `Actions` → 找到 `ESP-IDF Build` workflow，点击 `Run workflow` 或在 push 后自动触发。
3. 等待 Workflow 运行完毕（会在 Actions 页显示日志）
4. 构建成功后，下载名为 `firmware-bins` 的 artifact（包含 `build/*.bin`）

注意：
- CI 使用的是 esp-idf v5.2（如需其他版本，请修改 `.github/workflows/ci-build.yml`）。
- GitHub runners 有构建时间与带宽限制，首次运行会下载并安装 esp-idf 依赖，可能耗时较久。
