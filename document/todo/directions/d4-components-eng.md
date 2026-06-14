---
title: D4 - 交互组件与工程化
---

# D4：交互组件与工程化

> 调度甘特图等招牌交互组件（本教程用 VitePress 而非 MkDocs 的核心动机）+ 版本 / CI / 自检等质量保障。

**优先级分布**：

| P1 | P2 | P3 | 总计 |
|----|----|----| ---- |
| 1 | 6 | 2 | 9 |

---

## P1

### [P1] SchedulerGantt.vue 调度甘特图组件

- **来源**：迁移遗留
- **描述**：招牌交互组件——可拖时间轴看 tick 级任务切换 / 抢占 / 阻塞。可调优先级与 tick 周期，实时看行为变化。这是「交互式 FreeRTOS 教程」的招牌
- **涉及**：`site/.vitepress/theme/components/SchedulerGantt.vue`（新建），在 `theme/index.ts` 注册

---

## P2

### [P2] 任务状态机可视化组件

- **描述**：任务在 Running / Ready / Blocked 间跳转，鼠标悬停看是哪个 API 触发的
- **涉及**：`site/.vitepress/theme/components/`

### [P2] 可调参数时序演示组件

- **描述**：改优先级、改 tick 周期，实时看调度行为变化

### [P2] 版本钉死

- **来源**：评审建议
- **描述**：FreeRTOS 内核版本（V10.x.x）、官方书 v1.1.0、POSIX port 版本写进 README 与环境章，避免 API 漂移
- **涉及**：`README.md`、`document/tutorial/02_environment/`

### [P2] 读者画像明确

- **来源**：评审建议
- **描述**：定位「写给谁」（纯小白 vs 有嵌入式基础），决定开篇深度与前两章是否补基础
- **涉及**：`document/tutorial/01_why-rtos/`、`document/index.md`

### [P2] 一键自检脚本

- **来源**：评审建议
- **描述**：`make test` 或等价物，读者 clone 后一键验证环境 OK
- **涉及**：`codes_and_assets/`

### [P2] CI 验证 demo 编译

- **来源**：评审建议
- **描述**：目前 CI 只 build 文档站，加编译 blinky 的 job，防止代码示例腐烂
- **涉及**：`.github/workflows/`

---

## P3

### [P3] 代码 license 声明

- **来源**：评审建议
- **描述**：FreeRTOS 本身 MIT，本教程示例代码 license 明确声明
- **涉及**：`LICENSE`、`README.md`

### [P3] package.json name 改 tutorial_freertos

- **来源**：迁移遗留
- **描述**：目前 name 仍是 `imx-forge-docs`（从 imx-forge 复制），改成 `tutorial_freertos`（会触发 pnpm-lock 更新）
- **涉及**：`package.json`、`pnpm-lock.yaml`
