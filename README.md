# Tutorial_FreeRTOS

> 无硬件主机模拟的 FreeRTOS 教程 —— 在 PC 上跑通《Mastering the FreeRTOS Kernel》全部内核示例

本教程创建于: 2025-12-16
作者: Charliechen114514
联系方式: 725610365@qq.com

本项目隶属于组织 [Awesome-Embedded-Learning-Studio](https://github.com/Awesome-Embedded-Learning-Studio) 的文档教程系列。

## 这是什么？

一个**不依赖真 MCU、在 PC 上即可运行全部 FreeRTOS 内核示例**的教程：

- **双轨主机模拟**：POSIX Linux/macOS/WSL2 + Windows MSVC
- **Mock HAL + bridge-task** 架构安全模拟外设，绕开 POSIX port 的 FromISR-hang 陷阱
- 章节对齐《Mastering the FreeRTOS Kernel》13 章顺序
- 以多任务传感器仪表盘为渐进式贯穿项目
- 独立「仿真 vs 真实 MCU」坑点章 + RT-Thread 对比轨

## 仓库结构

```
Tutorial_FreeRTOS/
├── document/          # 文档源（VitePress，Markdown）
│   ├── tutorial/      # 核心教程（15 章，对齐官方书）
│   ├── pitfalls/      # 仿真 vs 真实 MCU 坑点
│   └── rt-thread/     # RT-Thread 对比轨
├── codes_and_assets/  # 代码、硬件电路图、PCB 等资产
├── site/              # VitePress 工程目录（配置、主题、组件、插件）
├── project.config.ts  # 单一配置真相源（站点元信息、nav、sidebar volumes）
└── package.json       # VitePress 依赖与脚本
```

## 本地预览

需要 Node.js 22+ 与 [pnpm](https://pnpm.io)。

```bash
pnpm install        # 安装依赖
pnpm dev            # 启动开发服务器 → http://localhost:5173
pnpm build          # 构建静态站点到 site/.vitepress/dist
pnpm preview        # 本地预览构建产物
```

## 代码资产

`codes_and_assets/` 放置教程所有可运行代码、硬件电路图或 PCB 文件。详见 [代码资产说明](./codes_and_assets/instractions.md)。

不知道从何开始？请看 [学习路线图](./document/tutorial/00_roadmap/index.md)。

## 与同系列项目的关系

本项目与 [imx-forge](https://github.com/Awesome-Embedded-Learning-Studio/imx-forge) 共用同一套 VitePress 文档脚手架（`site/` 工程 + `project.config.ts` 单源配置 + 自动扫描 sidebar），结构同构。
