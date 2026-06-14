---
title: RT-Thread 对比轨
---

# RT-Thread 对比轨

本轨在每章末尾以对比 box 形式给出 FreeRTOS ↔ RT-Thread 的 API 映射，并标注不可直接迁移的原语，方便已学过一方的人快速迁移。

## 运行环境

- BSP：`qemu-vexpress-a9`
- 兼容层：官方 FreeRTOS-Wrapper

## 不可直接迁移的原语

::: info 待标注
以下原语在 FreeRTOS 与 RT-Thread 间无直接对应，迁移时需重新设计：

- Queue Sets（队列集）
- Stream Buffers（流缓冲）
- MPU 支持（内存保护单元）

:::

> 🚧 各章 API 映射表与对比 box 待填充。
