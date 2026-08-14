# IOCP Learning Workspace

这是一个由 VS Code CMake Tools 管理的可复用 CMake/C++17 练习工作区。

根目录统一管理：

- C++17 标准。
- MSVC 与 Ninja。
- Debug/Release CMake presets。
- 编译警告、UTF-8 和 Windows Unicode 宏。
- Winsock 库 ws2_32 与 mswsock。
- 输出目录和 compile_commands.json。
- practice 下面的练习项目自动发现。

日常配置、编译、运行和调试全部通过 CMake Tools 选择，不需要 PowerShell 构建脚本。

## VS Code 扩展

需要：

- CMake Tools
- C/C++
- clangd（用于代码索引时使用）

工作区配置位于 .vscode/settings.json。CMake Tools 会在 Ninja preset 下自动加载
Visual Studio Developer Environment，因此可以找到 cl.exe、link.exe 和 ninja.exe。

## 第一次打开

请用 VS Code 直接打开本目录：

    D:\CodeRepository\claude\IOCP

然后使用状态栏或命令面板：

1. 执行 **CMake: 选择配置预设**。
2. 选择 **msvc-debug** 或 **msvc-release**。
3. 执行 **CMake: 配置**。
4. 执行 **CMake: 设置生成和启动/调试目标**。
5. 选择需要的 target，例如 **iocp_00_1_atomic**。

工作区已启用“启动目标同步为生成目标”，因此通常只需要选择一次 target。

## 编译、运行和调试

选择好目标后：

- 编译：执行 **CMake: 生成**，或者点击状态栏 Build 按钮。
- 运行：执行 **CMake: 运行但不调试**。
- 调试：执行 **CMake: 调试**。

当前 target：

    iocp_00_1_atomic

生成文件位于：

    build/msvc-debug
    build/msvc-release

程序统一位于相应生成目录的 bin 子目录。

## 新建练习项目

以后只需要在 practice 下创建目录、源码和一个 CMakeLists.txt。

示例：

    practice/
      01_sync_winsock/
        CMakeLists.txt
        Main.cpp

CMakeLists.txt 只需要：

    iocp_add_current_practice_executable()

保存文件后，CMake Tools 会重新配置。随后执行：

1. **CMake: 设置生成和启动/调试目标**。
2. 选择 **iocp_01_sync_winsock**。
3. 使用 CMake Tools 编译、运行或调试。

target 名称自动从 practice 下的相对目录生成：

- 00_1_atomic → iocp_00_1_atomic
- 01_sync_winsock → iocp_01_sync_winsock
- 02_overlapped_event → iocp_02_overlapped_event

源码可以放在练习目录，也可以放入 src、include 等子目录；公共 helper 会自动收集常见
C++ 源文件和头文件，并排除 build、out 和 .git 目录。

## 自定义单个练习

公共函数会把实际 target 名称写入 IOCP_CURRENT_TARGET。需要额外配置时可以写：

    iocp_add_current_practice_executable()
    target_compile_definitions(${IOCP_CURRENT_TARGET} PRIVATE MY_DEFINITION)

Windows 下默认已经链接 IOCP 练习常用的 ws2_32 和 mswsock。

## 关键文件

- CMakeLists.txt：工作区入口。
- CMakePresets.json：CMake Tools 使用的 Debug/Release preset。
- cmake/IOCPPracticeHelpers.cmake：自动发现、target 命名和公共配置。
- .vscode/settings.json：CMake Tools 的工作区行为。
- practice/：每个独立练习项目。
