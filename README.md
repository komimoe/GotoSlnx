# GotoSlnx

使用 CMake + vcpkg 构建的命令行工具，一键把 VS2022 的 `.sln` 转成 VS2026 的 `.slnx`。

## 功能

- 解析 `.sln` 项目、解决方案文件夹与 Solution Items
- 迁移解决方案配置、平台、项目配置映射
- 输出 `.slnx`（XML）
- 支持静态链接（建议使用 `x64-windows-static` triplet）

## 依赖

- CMake 3.25+
- vcpkg（已启用清单模式）
- Ninja（推荐）

## 构建

确保设置了 `VCPKG_ROOT` 环境变量。

项目使用 `cmake.toml`（cmkr）维护，`CMakeLists.txt` 由 cmkr 生成，请勿手动编辑。

使用 CMakePresets.toml：

- 配置：`default`
- 构建：`default`

如果你的 CMake 版本尚不支持 `.toml` 预设，请使用 `CMakePresets.json`（内容等价）。

## 使用

```
# 生成同名 .slnx
./out/build/goto-slnx --input path/to/solution.sln

# 指定输出
./out/build/goto-slnx --input path/to/solution.sln --output path/to/solution.slnx

# 覆盖输出
./out/build/goto-slnx --input path/to/solution.sln --force
```

## 说明

- 项目依赖会从 `.sln` 的 ProjectDependencies 映射为 `.slnx` 的 BuildDependency。
- 若 Build/Deploy 在 `.sln` 中缺失，会显式输出为 `false`。
- 解决方案文件夹名称使用 `/folder/` 形式的路径。若存在嵌套，将自动拼接。
