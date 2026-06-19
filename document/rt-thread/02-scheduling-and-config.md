---
title: 调度与配置哲学差异:FreeRTOS 的 config 宏 vs RT-Thread 的 rtconfig.h + Kconfig
description: 跳出单原语看宏观——两种调度模型、main 在两边到底是谁、FreeRTOSConfig.h 的 configXXX 宏 vs RT-Thread 的 rtconfig.h(由 Kconfig 生成)、静态 vs 动态分配的默认习惯
---

# 调度与配置哲学差异:FreeRTOS 的 config 宏 vs RT-Thread 的 rtconfig.h + Kconfig

> 这是对比轨的第二篇正文。上一篇 [原语映射大表](./01-primitive-mapping.md) 我们逐个原语对齐了 API,这篇跳出来看两边的**宏观设计哲学**——调度模型怎么搭、配置怎么管、`main` 到底是谁、内存分配的默认习惯。这些是「API 都对得上、但工程结构感觉完全不一样」的根源。

## 调度模型:都是抢占式优先级调度,但「谁来启动」不同

先把内核机制摆清楚:两个系统**都是基于优先级的抢占式调度**,都有就绪队列、都用 tick 驱动切换,这部分我们在 [任务管理](../tutorial/04_tasks/) 章讲的 ready/running/blocked/suspended 状态机两边通用。差别出在**系统是怎么跑起来的、`main` 是谁**——这一点迁移到 RT-Thread 时会让人愣一下。

FreeRTOS 的启动模型是「**应用显式启动调度器**」:你在 `main` 里做完初始化、建好初始任务,然后调一句 `vTaskStartScheduler()`——从这一刻起,内核接管调度,`main` 的执行上下文就「消失」进 idle 任务了(严格说 POSIX port 上 main 线程会变成 idle 的一部分),之后再也不会回到 `vTaskStartScheduler` 那行往下走。`main` 本身**不是一个任务**,它只是启动前的引导代码。

```c
/* FreeRTOS:main 不是任务,只负责引导 */
int main(void) {
    hardware_init();
    xTaskCreate(task_a, "a", 256, NULL, 2, NULL);
    xTaskCreate(task_b, "b", 256, NULL, 3, NULL);
    vTaskStartScheduler();      /* 不会返回 */
    for (;;);                   /* 不到这 */
}
```

RT-Thread 的启动模型不一样:**`main` 本身就是一个线程**。RT-Thread 在 `rtthread_startup()` 里会自动创建一个 `main` 线程(优先级通常比应用线程低一档),把你 C 程序的 `main` 函数挂进去,然后启动调度器。所以你在 RT-Thread 的 `main` 里写的代码,是跑在一个真实线程的上下文里的,可以直接用阻塞 API(`rt_thread_mdelay`、`rt_sem_take`)、可以创建更多线程——它本身就是系统的一部分,不是引导代码。

```c
/* RT-Thread:main 是一个线程,可以直接阻塞、建线程 */
int main(void) {
    rt_kprintf("hello from main thread\n");
    rt_thread_t tid = rt_thread_create("worker", worker_entry, RT_NULL,
                                       512, 3, 10);
    rt_thread_startup(tid);
    rt_thread_mdelay(2000);     /* main 线程直接阻塞 2s,合法 */
    return 0;                   /* main 线程可以返回并被回收 */
}
```

这个差异的迁移含义是:**你在 FreeRTOS 里堆在 `main` 启动前的初始化逻辑,迁到 RT-Thread 可以直接留在 `main` 里**(因为它就是线程),甚至可以阻塞等待外设就绪,而不必像 FreeRTOS 那样把所有阻塞逻辑都搬进任务里。反过来,从 RT-Thread 迁到 FreeRTOS 时,记得 `main` 不能阻塞——`vTaskStartScheduler` 之前的 `main` 是裸的引导上下文。

## 配置哲学:configXXX 宏数组 vs rtconfig.h + Kconfig

这是两边工程结构差异最大、迁移时最先撞上的一块。FreeRTOS 的配置是**一个手写头文件 `FreeRTOSConfig.h`**,里面是一长串 `configUSE_PREEMPTION`、`configMAX_PRIORITIES`、`configTOTAL_HEAP_SIZE`、`configUSE_TIMERS`、`configCHECK_FOR_STACK_OVERFLOW` 这样的宏,你照着官方文档逐条手改。这套宏的特点是**扁平、全手填、无依赖关系表达**——你知道你要开软件定时器就写 `#define configUSE_TIMERS 1`,没有什么「因为开了定时器所以要顺带开这个」的自动联动,全靠你读文档记得住。

```c
/* FreeRTOSConfig.h:手写、扁平 */
#define configUSE_PREEMPTION            1
#define configMAX_PRIORITIES            7
#define configTOTAL_HEAP_SIZE          (2 * 1024 * 1024)
#define configUSE_TIMERS                1
#define configTIMER_TASK_PRIORITY       3
#define configCHECK_FOR_STACK_OVERFLOW  2
```

RT-Thread 的配置是**两件事的组合**:`rtconfig.h`(最终的配置头文件,形态类似 FreeRTOSConfig.h)是由一个叫 **Kconfig** 的菜单配置系统**生成**的。你用的是 `menuconfig`(`scons --menuconfig` 或 env 工具)这个 TUI,在菜单里勾选「启用软件定时器」「启用消息队列」「启用 slab 分配器」,工具会自动算好依赖关系——比如你勾了某个组件,它依赖的东西会自动勾上,不兼容的会自动关掉——然后生成/更新 `rtconfig.h` 里的那些 `RT_USING_XXX` 宏。

```c
/* rtconfig.h:由 menuconfig 生成,带依赖联动 */
#define RT_NAME_MAX 8
#define RT_THREAD_PRIORITY_32          /* 优先级档位 */
#define RT_THREAD_PRIORITY_MAX         32
#define RT_USING_SEMAPHORE             /* ← 这些是勾选出来的 */
#define RT_USING_MUTEX
#define RT_USING_EVENT
#define RT_USING_MAILBOX
#define RT_USING_MESSAGEQUEUE
#define RT_USING_HEAP                  /* 开了堆,底下 small/slab 二选一 */
#define RT_USING_SMALL_MEM
```

这套差异带来的是**两种完全不同的工程心智**。FreeRTOS 那边你手写 `FreeRTOSConfig.h`、对着 FreeRTOS 参考手册逐条配,适合内核单一、配置项就那么几十个的场景;RT-Thread 那边你有 `menuconfig` 的菜单树和组件生态(设备驱动框架、文件系统、网络协议栈、各种 package),配置项成百上千、靠 Kconfig 的依赖表达管住,**不靠人脑记**。迁移时要适应的就是:在 RT-Thread 项目里改配置,**别手改 `rtconfig.h`**(会被下次 `menuconfig` 覆盖),要进 `menuconfig` 菜单改。两边的对应关系大致是:FreeRTOS 的 `configUSE_TIMERS` ↔ RT-Thread 勾选软定时器组件、`configMAX_PRIORITIES` ↔ `RT_THREAD_PRIORITY_MAX`、`configTOTAL_HEAP_SIZE` ↔ RT-Thread 堆区间(由 `rt_system_heap_init` 指定,不是个固定数字)。

## 静态 vs 动态分配的默认习惯

FreeRTOS 和 RT-Thread 在「静态 vs 动态」这件事上的**默认姿态和 API 形态不一样**,迁移时要注意别套错。

FreeRTOS 的默认和主流用法是**动态分配**:开 `configSUPPORT_DYNAMIC_ALLOCATION=1`(默认就是 1),用 `xTaskCreate`、`xQueueCreate` 这些不带 `Static` 后缀的 API,内核对象和任务栈都从 `ucHeap` 里 `pvPortMalloc` 出来。静态分配是可选的第二条路:开 `configSUPPORT_STATIC_ALLOCATION=1`,改用 `xTaskCreateStatic`、`xQueueCreateStatic`,对象结构体和缓冲区由你提前给好,完全不碰堆。**两条路是平行的两套 API,靠后缀区分**——这套机制我们在 [堆内存管理](../tutorial/03_memory/) 章的静态 vs 动态小节讲过。

RT-Thread 的姿态是**「动态是默认,静态是每个原语自带的第二条路,靠函数名前缀区分」**:每个 IPC 对象都有 `rt_xxx_create`(动态,堆上分配)和 `rt_xxx_init`(静态,你给结构体)两个版本,我们在上一篇映射表里反复提到这条线。线程同理,`rt_thread_create` 是动态、`rt_thread_init` 是静态。所以两边的「静态/动态」二选一能力都齐全,差别在于:

- **FreeRTOS**:靠 `Static` 后缀的平行 API + `configSUPPORT_STATIC_ALLOCATION` 总开关,**不开开关就没有静态 API**。
- **RT-Thread**:`init`/`create` 二选一是每个对象的内置惯例,**不需要总开关**,你随时可以在同一个工程里混用——某个热点对象用静态 `init`(省 malloc、确定性高),其他用动态 `create`(方便)。

迁移时这个区别很实用:如果你在 FreeRTOS 工程里全程静态分配,迁到 RT-Thread 时每个对象改用对应的 `init` 版本即可;反过来,RT-Thread 的混用习惯搬到 FreeRTOS 时,要先确认开了 `configSUPPORT_STATIC_ALLOCATION`,而且静态对象要乖乖走 `Static` 后缀那套。顺带一提,RT-Thread 的静态 `init` 版本要求你提供对象结构体(比如 `struct rt_semaphore`)的存储,这和 FreeRTOS 的 `StaticSemaphore_t` 是同一思路——把对象的内存从堆里拿出来、交给你管。

## 还有几个工程层面的细节

几个值得知道但不至于卡住你的点,放在一起说。**FreeRTOS 没有内置的控制台/打印框架**(本教程我们手写了 `console.c` 做线程安全 printf),RT-Thread 自带 `rt_kprintf`,这是内核级的线程安全打印,迁移时 FreeRTOS 那边的手写封装可以直接换成 `rt_kprintf`。**FreeRTOS 没有设备驱动模型**,你跟硬件打交道就是直接调 HAL/寄存器;RT-Thread 有一套 `rt_device` 设备框架(统一 open/read/write/close 接口),这是它「组件化」生态的根基,也是和 FreeRTOS 在工程结构上差距最大的地方,但这个属于 RT-Thread 生态而非内核原语,本轨不展开。最后,**RT-Thread 有官方的 FreeRTOS 兼容层**(一个把 FreeRTOS API 翻译成 RT-Thread API 的 wrapper),如果你只是想尽快把一个 FreeRTOS 工程跑在 RT-Thread 上、不打算重写,可以先上兼容层过渡,但长期建议按这篇和上篇的映射表重写成原生 RT-Thread API——兼容层覆盖不全(任务通知这类对不上的就得绕),而且失去了 RT-Thread 生态的好处。

## 小结

把两篇对比轨收一下。原语层面,绝大多数 FreeRTOS 概念在 RT-Thread 都有一一对应,迁移成本集中在**队列多出的邮箱、内存多出的 mempool、定时器的硬/软区分、任务通知的缺失**这几条上;工程层面,最大的认知切换是**配置从手写 `FreeRTOSConfig.h` 换成 `menuconfig` 生成 `rtconfig.h`、`main` 从引导代码变成线程、静态/动态从总开关后缀变成每对象的 init/create 惯例**。这些差异都不是「谁对谁错」,而是两个系统不同的设计哲学:FreeRTOS 追求内核极简、配置透明手控;RT-Thread 追求组件化、配置工具化、生态丰富。吃透这张迁移地图,在两边来回切就不会再对着 API 表发愣了。

至于本轨和主轨的关系:对比轨是给你「已经会 FreeRTOS」时快速迁移用的横向参考,不替代任何一章的主线学习。如果你想反过来——先用 RT-Thread 再碰 FreeRTOS,这张表倒着读同样成立,因为内核机制两边是相通的,真正的主线还是前面十四章里那套「调度 / 同步 / 通信 / 内存 / 调试」的机制理解,RT-Thread 不过是同一套思想的另一种 API 表达。
