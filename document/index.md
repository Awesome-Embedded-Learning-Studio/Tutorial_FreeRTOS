---
layout: home

hero:
  name: "Tutorial_FreeRTOS"
  text: "无硬件 FreeRTOS 主机模拟教程"
  tagline: 在 PC 上跑通《Mastering the FreeRTOS Kernel》全部内核示例 —— 零 MCU 门槛，专注 RTOS 内核机制
  image:
    src: /Awesome-Embedded.png
    alt: Tutorial_FreeRTOS Logo
  actions:
    - theme: brand
      text: 快速开始
      link: /QUICK_START
    - theme: alt
      text: 教程目录
      link: /tutorial/
    - theme: alt
      text: GitHub
      link: https://github.com/Awesome-Embedded-Learning-Studio/Tutorial_FreeRTOS

features:
  - icon: 🖥️
    title: 双轨主机模拟
    details: POSIX Linux/macOS/WSL2 与 Windows MSVC 两条路径，无需任何开发板即可运行全部 FreeRTOS 内核示例。
    link: /tutorial/02_environment/
  - icon: 🔌
    title: Mock HAL + bridge-task
    details: 每个模拟外设跑独立 pthread 并 block signals，经 IPC 投递到 bridge task 走标准 FromISR 路径，绕开 POSIX port 的 hang 陷阱。
    link: /tutorial/02_environment/
  - icon: 📚
    title: 对齐官方书 13 章
    details: 章节顺序严格对齐《Mastering the FreeRTOS Kernel》：内存在任务之前；信号量归中断章、互斥量归资源管理章。
    link: /tutorial/
  - icon: 📈
    title: 渐进式仪表盘项目
    details: 多任务传感器仪表盘作为贯穿脊柱，采集 → 队列 → 处理 → 显示，每章引入一个原语，最终拼成完整应用。
    link: /tutorial/14_project-dashboard/
  - icon: ⚠️
    title: 仿真坑点专章
    details: 系统梳理主机模拟的 10 大陷阱（ISR hang、ARM64 segfault、栈检测失效……）与仿真 vs 真硬件行为对比表。
    link: /pitfalls/
  - icon: 🔁
    title: RT-Thread 对比轨
    details: 每章末尾给出 FreeRTOS ↔ RT-Thread API 映射，并标注 Queue Sets / Stream Buffers / MPU 等不可直接迁移的原语。
    link: /rt-thread/
---
