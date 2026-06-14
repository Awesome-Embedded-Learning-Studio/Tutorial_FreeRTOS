---
title: 项目规划
---

<PageHeader icon="📋" title="项目规划" description="Tutorial_FreeRTOS 的规划、进度跟踪与任务检索" />

## 快速开始

### 新用户入口

第一次了解本教程？按以下顺序阅读：

1. **📖 [总体路线图](roadmap)** — 了解项目全貌与四个发展方向
2. **📋 [任务总览](todo)** — 查看所有待办任务的统计与当前重点
3. **🎯 [D1：主机模拟代码骨架](directions/d1-code-skeleton)** — 让示例真正在 PC 跑起来

### 贡献者入口

想参与开发？

1. **📋 [任务总览](todo)** — 按方向/优先级筛选任务
2. **📁 [方向详情](directions/d1-code-skeleton)** — 选择你擅长的方向
3. 在 GitHub Issue 声明意图 → Fork → PR

## 进度概览

| 阶段 | 状态 |
|------|------|
| VitePress 站点骨架 + 章节目录 | <StatusTag type="done" /> |
| D1：主机模拟代码骨架 | <StatusTag type="active" /> |
| D2：核心教程内容 | <StatusTag type="planned" /> |
| D3：差异化章节（坑点 + RT-Thread） | <StatusTag type="planned" /> |
| D4：交互组件与工程化 | <StatusTag type="planned" /> |

::: tip 当前重点：D1 — 让示例在 PC 跑通
教程的立身之本是「无硬件可运行」。优先完成 POSIX/MSVC 双轨 blinky + Mock HAL，让所有章节有真实可跑的代码支撑。
:::

## 发展方向

<ChapterNav variant="sub">
  <ChapterLink href="directions/d1-code-skeleton" variant="sub">D1：主机模拟代码骨架 — POSIX/MSVC demo、Mock HAL、仪表盘</ChapterLink>
  <ChapterLink href="directions/d2-tutorial-content" variant="sub">D2：核心教程内容 — 15 章正文，对齐官方书</ChapterLink>
  <ChapterLink href="directions/d3-differentiators" variant="sub">D3：差异化章节 — 仿真坑点 + RT-Thread 对比轨</ChapterLink>
  <ChapterLink href="directions/d4-components-eng" variant="sub">D4：交互组件与工程化 — 甘特图组件、版本钉死、CI</ChapterLink>
</ChapterNav>

## 任务来源说明

本规划整合自三处，每个任务都标注来源便于审计追溯：

- **PLAN task 1–5**：初版 5 任务规划（orgorg 规划，2026-06-13）
- **评审建议**：对初版规划的细化（拆分、补缺）
- **迁移遗留**：VitePress 迁移过程中识别的后续工作

## 如何贡献

1. 阅读 [路线图](roadmap)，选择一个方向
2. 在该方向按优先级（P0 > P1 > P2 > P3）挑任务
3. GitHub Issue 声明意图
4. Fork → 分支 → PR
5. 等待 Code Review

详见 GitHub 仓库贡献指南。
