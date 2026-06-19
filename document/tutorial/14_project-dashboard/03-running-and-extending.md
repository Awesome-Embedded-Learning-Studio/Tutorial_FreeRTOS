---
title: 运行输出解读与可扩展方向
description: 跑一遍 dashboard、抓一段真实输出逐行拆(看三类采样来源和模式切换在屏上怎么交错、事件组 AND 门怎么把第一帧延迟显示),再讲往真硬件搬、加通信、加更多传感器时的注意点
---

> [上一页](./02-how-primitives-fit.md) 把每个原语逐章对照完了,这一页让 dashboard 真正跑起来,逐行读它的输出,然后聊聊把它往真硬件和更多功能上扩展时要注意什么。

# 运行输出解读与可扩展方向

讲了这么多架构和协作,现在该上号验证了。dashboard 的构建和运行跟其他 demo 完全一样(在 `code/dashboard/` 下 `cmake -B build -DNO_TRACING=1 && cmake --build build`),跑的时候记得套一层 `stdbuf -oL` 强制行缓冲——这是[环境搭建](../02_environment/)那个 stdio 缓冲坑的延续,常驻型 demo 不套这层、重定向抓输出会是空的。

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL ./build/dashboard
```

跑起来第一行就是各路配置的自报家门,然后输出稳定往上滚。下面这段是我实测抓的、前几秒的真实输出(逐行带解读):

```
dashboard: starting scheduler (heap=2097152 bytes, sample=800 ms, button=2500 ms, init=1200 ms, control=1600 ms, cpu_stats=4000 ms)
init: system initialization started
heap monitor: free=1177080  min_ever=1177080
sensor: sample #0 value=0 (periodic, mode=normal) sent to queue
init: initialization done, set BIT_INIT_DONE
display: showing sample #0 value=0 (sampled 401 ms ago, prev #0)
sensor: sample #1 value=1 (periodic, mode=normal) sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
control: sent notify value=1 (normal)
heap monitor: free=1308368  min_ever=1177080
stats: samples=2 (buttons=0)  displays=2
sensor: sample #2 value=2 (periodic, mode=normal) sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 (BUTTON! extra sample, mode=normal) sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 (periodic, mode=normal) sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
control: sent notify value=2 (BOOST)
sensor: mode switch via notify value=2 -> BOOST
sensor: sample #5 value=5 (periodic, mode=BOOST) sent to queue
display: showing sample #5 value=5 (sampled 0 ms ago, prev #4)
```

## 逐行拆:每条输出对应哪个原语

第一行 `starting scheduler` 是 `main` 在启动调度器前打印的配置总览:堆 2MB、采样 800ms、按钮 2500ms、初始化 1200ms、控制 1600ms、CPU 统计 4000ms。这六个数字就是前面架构页讲的那几个周期源,你对着这行就知道接下来屏幕上会按什么节拍跳动。

紧接着的 `init: system initialization started` 是 Init 任务起手打印,然后它就 `vTaskDelay(1200ms)` 去了——这 1200ms 就是事件组 AND 门里 `BIT_INIT_DONE` 还没到位的那段窗口。第三行 `heap monitor` 是 HeapMon 第一次探堆,`free=1177080` 说明七个任务+一堆内核对象已经吃掉了约 920KB(2MB 里),`min_ever` 和 `free` 相等说明水位还没降过这点。

最值得细看的是 `sample #0` 和它后面那条 `display: showing sample #0 ... (sampled 401 ms ago, ...)`。注意两个细节:**Sensor 在第 4 行就把 #0 投进了队列**(约 800ms 周期滴答到了),但 Display 直到第 6 行——`init: initialization done` 那条 `BIT_INIT_DONE` set 之后——才显示它;而 `sampled 401 ms ago` 这个"采样于 401ms 前"告诉你,#0 这一帧在队列里**压了 400 多毫秒**才被显示出来。这就是[事件组](../09_event-groups/)那个 AND 组合门的活体演示:Sensor 早就 set 了 `BIT_NEW_SAMPLE`、队列里早有货,但 `BIT_INIT_DONE` 没到位,Display 就一直阻塞;直到初始化完成的 1200ms 那一刻,AND 条件才满足、Display 才解锁。从 #1 往后 `sampled 0 ms ago` 就说明门开了、Display 实时跟着队列走了。把 `INIT_DONE_DELAY_MS` 故意设成大于采样周期,就是为了让你在这屏输出里**亲眼**看到这道门的存在。

再看采样来源的标记。`sample #0/1/2/4/5` 后面都是 `(periodic, ...)`,这是周期滴答采的"周期帧";`sample #3` 后面是 `(BUTTON! extra sample, ...)`,这是按钮事件触发的额外一帧——2500ms 的按钮定时器在这期间 give 了 `xButtonSemaphore`,Sensor 那一轮零超时探到了、就多采一帧并标 `BUTTON!`。两种来源在屏上一眼可分,这正是架构页讲"两类无值信号量怎么区分"的兑现。

然后是模式切换那两路。`control: sent notify value=2 (BOOST)` 是 Control 任务用 `xTaskNotify` 把模式码 2 发出去;紧接着 `sensor: mode switch via notify value=2 -> BOOST` 是 Sensor 那一轮零超时探通知值、取到 2、切到 BOOST 并打印。切完之后 `sample #5` 就带上了 `mode=BOOST`,和前面的 `mode=normal` 一眼可分。这就是[任务通知](../10_task-notifications/)"带 32 位值"这个信号量做不到的本事在输出里的样子——模式码直接跟着通知到了 Sensor 手里,没走任何带外通道。

## CPU 统计:谁在吃 CPU

再往下滚动,大约 4 秒处会出现一张 CpuStats 任务拍的全任务快照:

```
---- cpu stats @ tick 4001 (configGENERATE_RUN_TIME_STATS=1) ----
  IDLE         Ready    prio=0 freestack=16379  CPU=100%
  HeapMon      Ready    prio=1 freestack=16379  CPU=<1%
  Stats        Ready    prio=1 freestack=16379  CPU=<1%
  CpuStats     Running  prio=1 freestack=16379  CPU=<1%
  Sensor       Blocked  prio=3 freestack=16379  CPU=<1%
  Tmr Svc      Blocked  prio=6 freestack=32763  CPU=<1%
  Control      Blocked  prio=2 freestack=16379  CPU=<1%
  Display      Blocked  prio=2 freestack=16379  CPU=<1%
--------------------------------------------------------
```

这张表是[调试](../12_debugging/)那章运行时统计的产出。最扎眼的是 **IDLE 占了 100%**——这是完全正确的、甚至让人安心的结果:说明我们这七个应用任务都"安分守己",该阻塞时都阻塞了(状态列里 Sensor/Display/Control/Tmr Svc 都是 Blocked),没有谁在忙等烧 CPU,于是空闲任务把剩下的 CPU 时间全吃光了。如果哪天你看到某个应用任务 CPU 占用飙到几十上百,那就是它在忙等没让出,是 starvation 或逻辑 bug 的信号。各任务的 `freestack=16379`(单位是字,POSIX port 下任务栈都是 16KB 那个 `PTHREAD_STACK_MIN`,几乎没动)印证了[调试](../12_debugging/)那条"host 下栈水位探针基本不动"的边界——真硬件上这个数字才会反映真实栈深度。

一个 host 特有的小细节:第一张 CpuStats 表(4001ms 那张)有时会打印 `(no run-time stats yet)`,因为运行时统计需要先积累一段才有数。多跑一会儿第二张表就有了——这不是 bug,是运行时统计的固有特性(真硬件也一样)。

## 往真硬件搬:哪些一行不改、哪些要换

dashboard 的整套设计是冲着"host 下学机制、真硬件上一字不改地用"去的,所以搬到真 MCU 时绝大多数代码不动,要换的只是那几个**扮演外部世界的替身**。具体来说,两个周期软件定时器(`SampleTmr` 模拟采样滴答、`ButtonTmr` 模拟按钮)在真硬件上应该换成真硬件定时器 ISR 和真按钮的 GPIO ISR——但 ISR 内部那一行 `xSemaphoreGiveFromISR` **一字不改**,Sensor 任务端 take 信号量的逻辑也不改。这正是[中断管理](../07_interrupts/)那套延迟中断处理范式的价值:host 下学到的写法原样可用。

有几件事真硬件上要重新审视。一是**堆和栈的大小**:host 下我们抬到 2MB、每个任务栈 16KB,那是 POSIX port 的体量(见[堆内存](../03_memory/));真 MCU 上 RAM 通常只有几十到几百 KB,任务栈几百到几千字节,`configTOTAL_HEAP_SIZE` 和 `configMINIMAL_STACK_SIZE` 都要按真硬件重估,`xPortGetMinimumEverFreeHeapSize` 这时就成了你估堆的金标准。二是**栈溢出检测**:host 下 `configCHECK_FOR_STACK_OVERFLOW` 压根不触发(POSIX port 够不着 pthread 的栈),真硬件上要开 `configCHECK_FOR_STACK_OVERFLOW=2`,并配合 `uxTaskGetStackHighWaterMark` 水位线长期监控——这条边界在[调试](../12_debugging/)和[排错](../13_troubleshooting/)都强调过。三是那个"队列集会 stall"的 host 脆弱点在真硬件上不存在,如果你嫌"主信号量短超时 + 旁路零超时"这套写法啰嗦,真硬件上完全可以改回队列集,一次等待多把信号量。

还有一类 host 演不出、真硬件必须管的:低功耗。真 MCU 上 dashboard 长期 idle 时,`configUSE_TICKLESS_IDLE` 能让内核空闲时停 tick、进低功耗,这在电池供电的设备上是刚需;但 host 模拟下测不出任何省电效果,所以 dashboard 完成态没引入它。低功耗的机制、唤醒延迟、低功耗定时器精度这些坑在[低功耗](../11_low-power/)那章,真硬件版本该补上。

## 加通信、加更多传感器:往哪扩

dashboard 的架构是留了扩展余地的。想**加一路传感器**(比如再加个温度、湿度),最自然的做法是再起一个采集任务、各自采各自的帧,然后要么共用同一条队列(帧结构里加个"来源"字段区分),要么各开一条队列;Display 端如果要从多条队列取帧,[队列集](../05_queues/)这时就派上用场了——真硬件上(或 host 上不碰那个 stall 组合时)用队列集一次等多条队列是干净的写法。

想**加通信出口**(比如把采样数据发到串口、网络),标准做法是起一个 Comms 任务,它在另一条队列上阻塞,谁有数据要发就往这条队列投,Comms 任务慢慢往外吐。这又是一个"生产者→队列→消费者"的模型,和 sensor→display 那条主链路同构。这样设计的好处是通信的快慢(串口波特率、网络延迟)被队列吸收了、不会拖慢采样,这也是队列"解耦生产消费速度"的工程价值。

想**加控制闭环**(采样→判断→动作),Control 任务这个壳子正好可以扩:它现在只是周期切模式,改成"根据 Display 或某个分析任务的结果决定切什么模式"就是闭环雏形。任务通知能带值这个特性在这里会很好用——下发的不只是"切模式",还可以是"设阈值为 X"、"采样率改为 Y"这种带参数的指令,一个 32 位通知值够装很多种控制码。

## 小结

走到这里,贯穿全教程的 dashboard 脊柱就闭环了。你有一个能稳定跑的、七个任务协作的多任务仪表盘,看懂了它的每一行输出对应哪个原语在工作,知道了 CPU 统计怎么读、AND 门怎么把第一帧延迟显示、三类采样来源怎么在屏上一眼可分,也清楚往真硬件和更多功能上扩展时的注意点。这套从 03 堆、04 任务一路加到 12 运行统计的渐进脊柱,到这一章拼成了完整的"采集→队列→处理→显示 + 定时器驱动 + 模拟中断 + mutex + 事件组 + 任务通知 + 可观测性"的真实应用——这就是我们把 FreeRTOS 核心原语在一台 PC 上、零硬件跑通一个像样工程的全部成果。

完结撒花。如果这套从零到集成、host 模拟全程可跑的旅程走通了,你已经具备了在真 MCU 上用 FreeRTOS 搭多任务应用所需的全部内核机制基础——剩下的就是去啃真硬件那份手册、把这套替身换回真 ISR 和真外设了。至于 host 模拟和真 MCU 之间那些系统性的行为差异,带着这张对照表去[仿真坑点](../../pitfalls/)逐条对一遍,迁移路上就不会踩空。
