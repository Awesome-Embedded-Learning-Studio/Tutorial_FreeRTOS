---
title: 待办事项
---

# 待办事项

> **最后更新**：2026-06-14
> **任务总数**：36 项待办

## 快速导航

- 🗺️ [总体路线图](roadmap)
- 📁 按方向查看：
  - [D1：主机模拟代码骨架](directions/d1-code-skeleton) — 7 项
  - [D2：核心教程内容](directions/d2-tutorial-content) — 14 项
  - [D3：差异化章节](directions/d3-differentiators) — 6 项
  - [D4：交互组件与工程化](directions/d4-components-eng) — 9 项

## 优先级说明

```
P0 ──► 主线闭环必须（示例跑通 + 首批章节）
P1 ──► 重要尽快（核心章节 + Mock HAL + 坑点）
P2 ──► 增强逐步（进阶章节 + 组件 + 对比轨）
P3 ──► 可选补充（license、命名）
```

## 任务统计

### 按方向统计

| 方向 | P0 | P1 | P2 | P3 | 总计 |
|------|----|----|----|----| ---- |
| D1：主机模拟代码骨架 | 3 | 4 | - | - | 7 |
| D2：核心教程内容 | 2 | 7 | 5 | - | 14 |
| D3：差异化章节 | - | 3 | 3 | - | 6 |
| D4：交互组件与工程化 | - | 1 | 6 | 2 | 9 |
| **总计** | **5** | **15** | **14** | **2** | **36** |

### 按类型统计

| 类型 | 数量 |
|------|------|
| 代码任务（D1 + D4 部分） | 12 |
| 文档任务（D2 + D3） | 21 |
| 交互组件（D4） | 3 |

## 当前重点

**P0 必须先做**（5 项，主线闭环）：

1. [D1] POSIX 轨 fork `main_blinky` → `posix_demo`
2. [D1] Windows MSVC 轨 `WIN32.sln`
3. [D1] 标注 POSIX port 已知问题（ARM64 segfault 等）
4. [D2] 开篇定位章 + 学习路线图
5. [D2] 环境搭建章（对接 D1）

完成这 5 项，教程就具备「可跑 + 可读入口」的闭环。

## 已完成

- [x] VitePress 迁移骨架（MkDocs → VitePress，与 imx-forge 同构）
- [x] 章节目录骨架（15 章 + pitfalls + rt-thread 占位）
- [x] 自动扫描 sidebar + 首页 + CI

## 文档结构

```
document/todo/
├── index.md                    # 入口（进度概览 + 导航）
├── roadmap.md                  # 总体路线图
├── todo.md                     # 本文件：任务总览
└── directions/
    ├── d1-code-skeleton.md     # D1：主机模拟代码骨架（7 项）
    ├── d2-tutorial-content.md  # D2：核心教程内容（14 项）
    ├── d3-differentiators.md   # D3：差异化章节（6 项）
    └── d4-components-eng.md    # D4：交互组件与工程化（9 项）
```
