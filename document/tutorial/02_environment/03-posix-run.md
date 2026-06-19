---
title: 运行、读输出与排坑
description: POSIX 轨第三页——跑起 blinky、读懂 TX/RX/定时器的输出、stdio 缓冲坑、gdb 信号,以及整条 POSIX 轨的踩坑速查表
---

# 运行、读输出与排坑

> POSIX 轨第三页(本轨收尾)。[上一页](./02-posix-build.md) 产出了 `build/posix_demo`,这一页让它跑起来、看懂它在输出什么,并收拾几个运行和调试阶段才会暴露的坑。

## 点火:跑起来

直接运行构建产物:

```bash
./build/posix_demo
```

正常的话你会立刻看到输出稳定地往上滚:

```
Starting echo blinky demo
Message received from task
Message received from task
Message received from task
...
Message received from software timer
Message received from task
...
```

看到稳定滚动就说明跑通了,`Ctrl-c` 退出。

## 读懂这屏输出在干什么

光看到滚动还不够,得知道这些字符对应内核里发生了什么,这才是本教程的意义。这屏输出对应的是 `main_blinky()` 起的**两个任务加一个软件定时器**:发送任务(代码里记名 `"TX"`)周期性地往一个队列里塞消息;接收任务在队列另一头阻塞等待,一旦收到消息就打印 `Message received from task`;另外还有一个软件定时器,它的回调偶尔触发,打印 `Message received from software timer`。

所以你看到的「一堆 task 夹杂零星 timer」,本质上是两个任务通过队列一收一发——这正是 FreeRTOS 最经典的任务间通信模型,只不过这次它跑在你 PC 的 pthread 上而不是 MCU 上。后面 [队列](../05_queues/) 那章会把这个模型彻底讲透,现在你只要建立一个直观印象:任务、队列、定时器这些抽象,在 host 模拟下和真 MCU 上行为一致,这就是我们能用它学内核机制的前提。

至于 `Ctrl-c` 能干净退出,是因为 `main.c` 开头注册了 SIGINT 处理函数(`signal( SIGINT, handle_sigint )`),POSIX port 特意不屏蔽 SIGINT,就是让你能体面地结束这个常驻进程。

## 第一个坑:重定向抓输出竟然是空的

⚠️ 这是运行阶段最容易让人误判的一个坑,一定要讲。如果你像我一样,习惯把输出重定向到文件、外面套个 `timeout` 来抓记录:

```bash
timeout 5 ./build/posix_demo > out.txt
```

五秒后打开 `out.txt`,大概率是**空的零字节**。第一反应往往是「完了,demo 崩了」,但一看 `timeout` 的退出码是 124——那是 `timeout` 主动杀掉进程的正常退出码,不是崩溃。交互终端里跑得好好的,怎么一重定向就哑了?

原因不在 FreeRTOS,而在 C 标准库的 stdio 缓冲机制。`printf` 在输出到一个真正的终端(tty)时是**行缓冲**——每碰到 `\n` 就 flush 一次;可一旦它发现 stdout 不是终端而是管道或文件,就会切到**全缓冲**,要攒满几 KB 的 buffer 才肯吐。我们的 `console_print` 用的就是 `vprintf`,而那五秒的输出总共也就二十来行,根本攒不满全缓冲的 buffer;`timeout` 把进程一杀,buffer 里没来得及写出去的东西跟着进程一起进了垃圾桶,文件自然就是空的。

解决办法是用 `stdbuf` 强制行缓冲,或者干脆在交互终端里直接看:

```bash
stdbuf -oL ./build/posix_demo > out.txt    # 这样重定向才抓得到完整输出
```

记住这条结论就好:验证这种常驻型 demo 的输出,要么交互终端,要么 `stdbuf -oL`,别裸重定向。

## 第二个坑:gdb 调试被信号刷屏

如果你想用 gdb 调试这个 demo,会发现根本没法正常单步——每走一步都可能断在一个 `SIGUSR1` 或 `SIG34` 上。这不是你的代码触发了什么异常,而是 POSIX port 内部用这类信号在 pthread 之间做调度同步(可以理解成用它来模拟 MCU 上「触发一次上下文切换」)。gdb 默认会把这些信号当成「值得停下来」的事件,于是你就被刷屏了。

对策是在 gdb 里告诉它别理这些信号:

```
handle SIGUSR1 SIG34 nostop noprint
```

`nostop` 是收到信号也不停、`noprint` 是不打印通知。把它写进你的 `.gdbinit` 省得每次手敲。这件事的更完整背景见 [仿真坑点](../../pitfalls/)。

## POSIX 轨踩坑速查表

把这条轨从头到尾的坑收一张表,遇到现象直接对照:

| 阶段 | 坑 | 现象 | 对策 |
|------|----|------|------|
| 架构 | 在 ARM64 上跑 | 立即 segfault | POSIX port 仅 x86_64,换 x86 环境 |
| 拉源码 | 内核 submodule 没 init | `Source/tasks.c: No such file` | `git -C third_party/FreeRTOS submodule update --init FreeRTOS/Source` |
| 拉源码 | 手滑加了 `--recursive` | 下载几个 GB 无关代码 | 只 init Source 这一个,recursive 是禁词 |
| 构建 | 忘 `-DUSER_DEMO=BLINKY_DEMO` | 链接报 `undefined reference to 'main_full'` | 构建时带上 |
| 构建 | 忘 `-DNO_TRACING=1` | 找不到 `trcRecorder.h` | 带上,关掉 trace |
| 运行 | 裸重定向抓输出 | 文件零字节 | `stdbuf -oL` 强制行缓冲,或交互终端直接看 |
| 调试 | gdb 频繁断信号 | 一直停在 SIGUSR1 / SIG34 | `handle SIGUSR1 SIG34 nostop noprint` |

## 小结

走到这里,POSIX 轨就闭环了:你有一个能稳定滚动的 `posix_demo`,看懂了它的输出对应任务、队列、定时器三者怎么协作,也知道了 stdio 缓冲和 gdb 信号这两个运行/调试阶段的坑。这个 demo 就是后面所有章节示例的运行底座——[任务](../04_tasks/)、[队列](../05_queues/)、[定时器](../06_timers/) 里出现的每一段代码,你都能在这个底座上动手验证。

至于「这套 host 模拟和真 MCU 到底有哪些行为差异、哪些东西不能照搬」,那是 [仿真坑点](../../pitfalls/) 的事,建议在进入具体内核机制之前扫一眼,建立正确预期。
