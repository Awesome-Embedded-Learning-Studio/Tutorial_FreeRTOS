---
title: 仿真 vs 真实 MCU 坑点
---

# 仿真 vs 真实 MCU 坑点

主机模拟是把双刃剑：零硬件门槛，但行为与真 MCU 存在系统性差异。本章把教学目标**明确限定在 RTOS 内核机制**（调度 / 同步 / 通信 / 内存 / 调试），主动把实时性、中断延迟、功耗声明为边界外，让你在移到真硬件前建立正确预期。

## 10 大陷阱速览

1. **非实时** —— PC 调度无确定性，timing jitter 不可信
2. **栈双重性** —— `configCHECK_FOR_STACK_OVERFLOW` 在 POSIX port 失效
3. **ISR API hang** —— 严禁从外部线程直调 FromISR，必须经 bridge task
4. **系统调用不安全** —— 阻塞式 syscall 会拖垮整个模拟器
5. **ARM64 segfault** —— POSIX port 在 ARM64 Ubuntu 24.04 已知崩溃
6. **CPU 占满** —— 忙等任务需 `nanosleep` 主动让出
7. **GDB 信号干扰** —— 调试时需忽略 `SIGUSR1` 与 `SIG34`
8. **timing 需手动注入** —— 传感器/按钮事件靠 Mock HAL 周期注入
9. **多核新竞态** —— 多核 PC 暴露单核 MCU 上隐藏的竞态
10. **`vTaskDelete` 上限** —— 删除任务的资源回收差异

## 仿真 vs 真硬件行为对比

::: warning 待填充
每条原语（队列阻塞、信号量、定时器、任务通知等）在仿真与真 MCU 上的行为差异对比表待补。
:::
