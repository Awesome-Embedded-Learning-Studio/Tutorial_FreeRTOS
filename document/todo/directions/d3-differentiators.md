---
title: D3 - 差异化章节
---

# D3：差异化章节

> 仿真坑点专章 + RT-Thread 对比轨——这是本教程区别于市面其他 FreeRTOS 教程（大多绑死硬件）的核心卖点。

**优先级分布**：

| P1 | P2 | 总计 |
|----|----| ---- |
| 3 | 3 | 6 |

---

## P1

### [P1] 仿真坑点章：10 大陷阱详解

- **来源**：PLAN task 4
- **描述**：逐一展开 10 大陷阱：
  1. 非实时（无 timing 确定性）
  2. 栈双重性（`configCHECK_FOR_STACK_OVERFLOW` 在 POSIX 失效）
  3. ISR API hang（严禁外部线程直调 FromISR）
  4. 系统调用不安全
  5. ARM64 segfault
  6. CPU 占满（需 `nanosleep` 让出）
  7. GDB 需忽略 `SIGUSR1` / `SIG34`
  8. timing 需手动注入
  9. 多核新竞态
  10. `vTaskDelete` 上限
- **涉及**：`document/pitfalls/`

### [P1] 即时排障（内嵌环境搭建章）

- **来源**：评审建议（任务 4 拆两层）
- **描述**：读者搭环境第一天就会撞的问题（ISR hang、ARM64、信号），内嵌在 02_environment 提供即时解法；系统性对比放 pitfalls 章
- **涉及**：`document/tutorial/02_environment/`

### [P1] 仿真 vs 真硬件对比表

- **来源**：PLAN task 4
- **描述**：同一原语（队列阻塞、信号量、定时器、任务通知）在仿真与真 MCU 上的行为差异对比表
- **涉及**：`document/pitfalls/`

---

## P2

### [P2] RT-Thread 运行环境

- **来源**：PLAN task 5
- **描述**：用 `qemu-vexpress-a9` BSP + 官方 FreeRTOS-Wrapper 兼容层
- **涉及**：`document/rt-thread/`、`codes_and_assets/`

### [P2] 各章 API 映射 box

- **来源**：PLAN task 5
- **描述**：每章末尾以对比 box 给出 FreeRTOS ↔ RT-Thread API 映射，方便已学一方者迁移
- **涉及**：`document/tutorial/*`（每章）、`document/rt-thread/`

### [P2] 不可迁移原语标注

- **来源**：PLAN task 5
- **描述**：标注 **Queue Sets / Stream Buffers / MPU** 等无直接对应、需重新设计的原语
- **涉及**：`document/rt-thread/`
