# 代码与硬件资产说明

本目录放置教程所有的可运行代码、硬件电路图或 PCB 文件。

## 目录约定（规划中）

```
codes_and_assets/
  posix_demo/        # POSIX 轨示例（基于 portable/ThirdParty/GCC/Posix）
  win32_demo/        # Windows MSVC 轨示例（基于 Demo/WIN32-MSVC）
  harness/           # Mock HAL 与 bridge-task 模拟外设层
  boards/            # （可选）真实硬件的电路图 / PCB，供对照
```

## 如何编译运行

详细的编译步骤、编译开关、环境要求见文档站的 [环境搭建章](../document/tutorial/02_environment/)。

::: tip 最小验证
先跑通 `main_blinky`：POSIX 轨 `make && ./posix_demo`，Windows 轨用 `WIN32.sln`。
:::

## 与文档站的关系

文档源在仓库根的 `document/`，由 VitePress 构建（构建方式见 `README.md`）。本目录的代码通过文档中的相对链接引用，代码本身不进入文档源。
