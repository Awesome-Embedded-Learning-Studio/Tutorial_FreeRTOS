---
title: 调试工具箱:概念与原理
description: 调试这章讲什么、configASSERT 怎么当场拦下写错的 API 用法、uxTaskGetSystemState/vTaskList 怎么把任务状态拍成快照、uxTaskGetStackHighWaterMark 怎么探栈水位、configGENERATE_RUN_TIME_STATS 怎么算出各任务的 CPU 占用、GDB 下要 ignore 掉哪些信号——以及栈溢出检测为什么在 POSIX port 下失效这一最重要的边界
---

# 调试工具箱:概念与原理

> 调试这一章第一篇。这一篇讲工具的「为什么」和「机制」,不跑代码;看完你就知道手上有哪些观测手段、它们的边界在哪。[下一篇](./02-debugging-demo.md) 把这些工具串成一个能跑的独立 demo,[再下一篇](./03-debugging-dashboard.md) 把 CPU 占用统计嫁接到 dashboard 脊柱上。回[章节导航](./index.md)。

## 这章解决什么问题

前面十一章我们一直在「写」:写任务、写队列、写信号量、写定时器、写中断处理。但代码写完只是一半,另一半是「它跑起来不对的时候怎么办」。RTOS 比裸机难调试的地方在于:你有多个任务在并发地跑,出问题的现场往往转瞬即逝——一个任务踩坏了另一个任务的栈、一次错误的 API 调用让某个任务再也没被调度、一个优先级设错把别人活活饿死。等你挂上调试器想看的时候,现场早就没了。这一章要解决的就是「怎么在 RTOS 里把这种转瞬即逝的现场,变成你能看见的东西」。

FreeRTOS 给我们准备了一套「观测工具箱」:**`configASSERT`** 让你在写错 API 用法时当场断言停下、把出错文件名+行号甩给你;**`uxTaskGetSystemState`/`vTaskList`** 让你把所有任务的状态拍成一张快照(谁在跑、谁在 blocked、谁的栈快满了);**`uxTaskGetStackHighWaterMark`** 让你单独探一个任务的栈水位;**`configGENERATE_RUN_TIME_STATS`** 让你算出每个任务各占多少 CPU。这一章我们把这些工具一个个跑通,让你在自己的输出里看到它们的真实长相。

但这也是整个教程里**最需要诚实交代边界**的章节之一,因为真相是:有一件「调试神器」——栈溢出检测——在 POSIX port 下压根不生效。真 MCU 上它是 FreeRTOS 兜底排查栈问题的第一道防线,host 模拟下它失效,我们只能用 `uxTaskGetStackHighWaterMark` 「主动探」来代替它「被动报警」。这个差异我们会专门展开讲,务必和 [仿真坑点](../../pitfalls/) 对照着读。除此之外,本章的断言、状态快照、运行时统计,这几样在 host 和真硬件上行为完全一致——它们是你在两个平台上都用得上的常备工具。

## configASSERT:写错 API 用法时当场炸给你看

调试的第一道防线,是「快速失败」(fail fast)。裸机 C 里你有个 `assert()` 宏,条件不满足就 abort;FreeRTOS 提供了一个更强的、内核级的版本,叫 `configASSERT`,它定义在 `FreeRTOSConfig.h` 里,模板的写法是:

```c
#define configASSERT( x )    if( ( x ) == 0 ) vAssertCalled( __FILE__, __LINE__ )
```

它和普通 `assert` 形态一样——条件为假就触发——但它做的不是 abort,而是调一个**应用提供的回调** `vAssertCalled`,把出错的 `__FILE__` 和 `__LINE__` 传过去。这个回调的实现我们在 `app_hooks.c` 里已经替你写好了(每个 demo 都有这一份样板),它的核心就这几行:

```c
void vAssertCalled( const char * const pcFileName, unsigned long ulLine )
{
    volatile uint32_t ulSetToNonZeroInDebuggerToContinue = 0;

    ( void ) pcFileName;
    ( void ) ulLine;

    printf( "ASSERT FAILED: %s:%lu\n", pcFileName, ulLine );
    fflush( stdout );

    taskENTER_CRITICAL();
    {
        while( ulSetToNonZeroInDebuggerToContinue == 0 )
        {
        }
    }
    taskEXIT_CRITICAL();
}
```

这里有两层设计值得讲。第一,它先打印一行 `ASSERT FAILED: <文件>:<行号>`——这就是「把现场甩给你」的核心动作:你看到这行,立刻就知道是哪个内核源文件的第几行拦下了你,定位问题几乎一步到位。第二,打印完它不直接 abort,而是进一个「空循环」卡住——`while( ulSetToNonZeroInDebuggerToContinue == 0 )` 里那个变量是 `volatile` 的,你挂上 GDB 之后,可以手动把它改成非 0,程序就从断言点继续往下走,方便你在断言现场附近再多看两眼。这套「打印+卡死+可手动放行」就是嵌入式断言的标准写法:既快速失败、又不丢现场。

那 `configASSERT` 到底拦什么?它在内核各处埋点,拦的都是「你用法错了」这类硬错误:传了 NULL 句柄给 `vTaskPrioritySet`、给任务设了越界的优先级、在中断里调了任务上下文才能用的 API、创建对象时堆不够……这些错误如果不禁声地放过去,后面往往是一个更难追的玄学 bug(任务莫名不再被调度、内核链表错乱);`configASSERT` 把它拦在发生的那一刻,正是 FreeRTOS 比裸机更容易排错的关键之一。我们这章 demo 会故意制造一个越界优先级,让你亲眼看到那行 `ASSERT FAILED` 长什么样。

## 任务状态快照:uxTaskGetSystemState 和 vTaskList

`configASSERT` 是「出错时」的工具,但很多时候程序没出错、只是行为奇怪,你需要的是「现在这一刻系统里到底发生了什么」——这时候就要拍**任务状态快照**。FreeRTOS 提供两条路拍快照,我们这章都走一遍。

更底层、也更可控的那条是 `uxTaskGetSystemState`。它一次性把系统里**所有任务**(你自己建的 + 内核的 idle 任务 + 定时器服务任务)的当前状态填进一个 `TaskStatus_t` 结构体数组,返回填了多少条。这个结构体里装着我们关心的全部信息:任务名(`pcTaskName`)、当前状态(`eCurrentState`,取值 `eRunning`/`eReady`/`eBlocked`/`eSuspended`/`eDeleted`)、优先级(`uxCurrentPriority`)、栈高水位(`usStackHighWaterMark`),以及开运行时统计后的累计运行时(`ulRunTimeCounter`)。我们拿到数组后自己格式化打印,列怎么排、显示哪些字段都由我们说了算,这正是 [04 任务管理](../04_tasks/) 那张状态快照表的做法,本章 demo 沿用它。

另一条路是 `vTaskList`,它本质上内部也是调 `uxTaskGetSystemState`,只不过把「格式化打印」这步替你封装好了——你传一个写缓冲进去,它给你吐出一张固定格式的表。这条路更省事,但有个前提:它受 `configUSE_STATS_FORMATTING_FUNCTIONS` 这个开关控制,模板里这个开关是 0(关着的)。我们这章**故意不去动这个开关**(样板文件能不动就不动,改了反而要让读者翻 `FreeRTOSConfig.h` 找),于是 `vTaskList` 在链接阶段就被剔掉了。这不是问题——`uxTaskGetSystemState` 自己格式化是更推荐的教学路线,你想要 `vTaskList` 那种现成格式,只要在 `FreeRTOSConfig.h` 把 `configUSE_STATS_FORMATTING_FUNCTIONS` 置 1 重新编译即可。我们 demo 里会探测一下这个开关、把它的状态打出来,让你心里有数「这条路是开是关、开了什么样」。

## uxTaskGetStackHighWaterMark:探一个任务的栈水位

栈是 RTOS 里最容易出问题的地方之一——任务栈给小了会溢出、踩坏相邻内存,给大了又浪费 RAM。但「栈到底用到了多深」这件事,光看代码是看不出来的(局部变量、函数调用链、中断打断时保存的上下文都在压栈),得**运行起来量**。`uxTaskGetStackHighWaterMark( TaskHandle_t )` 就是这个量尺:它返回「自这个任务启动以来,它的栈最多用到离栈底还剩多少」。这个值越接近 0,说明栈越危险、随时可能溢出;它要是还很大,说明你给大了、可以收紧省 RAM。

调用姿势有两种。一是给某个具体任务传句柄,探那一个任务的栈水位;二是给 `uxTaskGetSystemState` 返回的 `TaskStatus_t` 数组里读 `usStackHighWaterMark` 字段,一次性看到所有任务的水位(本章 demo 和 [04 任务管理](../04_tasks/) 都是这么用的)。有一个单位坑要记住:`usStackHighWaterMark` 的单位是**字(StackType_t)**,不是字节——64 位上一个字是 8 字节,所以你看到 `16379` 不是 16379 字节,换算成字节要乘 8。这个单位坑和 `xTaskCreate` 的 `usStackDepth` 同源,在那里讲过。

这里必须立刻插一个**最重要的边界**:你可能在别处读到 `configCHECK_FOR_STACK_OVERFLOW` 这个开关——把它设成 1 或 2,内核就会在任务切换时检查栈有没有溢出、溢出了就调 `vApplicationStackOverflowHook` 报警。听起来很美好,但我们要彻底说清楚——

## ⚠️ 栈溢出检测在 POSIX port 下不生效

这是本章最大的一颗雷,也是必须诚实交代清楚的边界:**`configCHECK_FOR_STACK_OVERFLOW`(栈溢出被动检测)在 POSIX port 下实际不生效,你设了 1 或 2 也不会触发那个 hook**。模板的 `FreeRTOSConfig.h` 里它就是 0,我们也没开它,不是疏忽,是开了也没用。

要理解为什么,得看这个机制在真 MCU 上是怎么实现的。`configCHECK_FOR_STACK_OVERFLOW` 设成 2 时,内核会在每个任务栈的栈底(增长方向的尽头)涂一段已知的「魔数」(sentinel pattern),然后在每次任务切换时检查这段魔数有没有被踩——如果被踩了,说明栈溢出过、踩进了这段哨兵区,就调 `vApplicationStackOverflowHook` 报警。这套机制的核心前提是:**内核要能访问、能涂写、能检查任务的栈内存**。在真 MCU port 上,任务栈是内核从堆里分出来的、内核拥有完整控制权的一块普通内存,涂魔数、检查魔数都没问题。

但 POSIX port 不一样。它把每个 FreeRTOS 任务映射成一个宿主 pthread,任务栈是**宿主 glibc 在 pthread 创建时分配和管理的 pthread 栈**,不是内核能直接控制的那块堆内存。内核根本没有一个「能涂魔数的栈底地址」可写——它够不着 pthread 的栈。所以 `vTaskSwitchContext` 里那段栈溢出检查代码在 POSIX port 上要么被条件编译跳过、要么检查的是一个内核并不真正拥有、也无法反映 pthread 栈真实使用的区域。结果是:你在 host 模拟下就算真把某个任务的栈用爆了(让 pthread 栈溢出),那个 `vApplicationStackOverflowHook` 也不会被调——pthread 栈溢出会被宿主直接 SIGSEGV 掉,程序崩在 glibc 层面,而不是优雅地走进你的 hook。这条在 [仿真坑点](../../pitfalls/) 的「栈双重性」条里有专门记录。

那 host 模拟下我们怎么排查栈问题?只能用上面讲的 `uxTaskGetStackHighWaterMark`「主动探」——周期性地读每个任务的水位,看谁逼近 0。这是「主动量」代替「被动报警」,虽然不如真硬件那个 hook 省事,但够用。demo 里我们仍保留 `vApplicationStackOverflowHook` 的实现(在 `app_hooks.c`),它在真硬件上一字不改就能用;host 下它是个「备而不用」的占位。要强调的是,**真 MCU 上 `configCHECK_FOR_STACK_OVERFLOW=2` + `uxTaskGetStackHighWaterMark` 是标配,两个都要**——被动 hook 兜底捕获已发生的溢出、主动水位计平时监控趋势;只有 host 模拟下被动那条失效,我们靠主动那条撑住。

## 运行时统计:每个任务各占多少 CPU

最后一件工具是 `configGENERATE_RUN_TIME_STATS`,它回答「每个任务各吃了多少 CPU」这个问题,是排查「任务 starvation(被饿死)」「某个任务死循环占满 CPU」这类问题的利器。它的原理比栈检测直观得多:内核给每个任务维护一个累计运行时计数 `ulRunTimeCounter`,只要这个任务处于 running 状态,这个计数就按某个时钟往上累加。这个时钟要比系统 tick 快得多(典型是 tick 的 10~20 倍),才能区分出短任务的占用。

要让这套机制工作,你得提供两样东西:一个「配置运行时统计时钟」的函数、一个「读这个时钟当前值」的函数。在 `FreeRTOSConfig.h` 里它们通过两个宏挂钩——`portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()` 在调度器启动时调一次(初始化时钟)、`portGET_RUN_TIME_COUNTER_VALUE()` 每次采样时调(读时钟值)。**好消息是:POSIX port 已经替我们把这两样都接好了**——`portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()` 是个 no-op(POSIX 环境的时钟天然可用,不用初始化),`portGET_RUN_TIME_COUNTER_VALUE()` 映射到 port 自带的 `ulPortGetRunTime()`。所以我们在 host 模拟下只要确保 `configGENERATE_RUN_TIME_STATS=1`(模板已开),就能直接用,一行额外代码都不用写。

用的时候,`uxTaskGetSystemState` 的第三个参数是个出参 `pulTotalRunTime`——开了运行时统计后,它会把「自启动以来的总运行时」填进去;同时每个任务的 `TaskStatus_t.ulRunTimeCounter` 就是它各自累计的运行时。占比的算法很朴素:`某任务 ulRunTimeCounter × 100 / 总运行时`,就是这个任务的 CPU 占用百分比。本章 demo 和 dashboard 增量都是这么算的。注意这个统计是「自启动以来的累计」平均,不是「最近一秒」的瞬时——所以一个偶尔死循环的任务,在长期统计里占比可能不高;排查这类问题,你看的是「相对趋势」和「谁占比异常高」,而不是把它当瞬时仪表盘用。

## GDB 技巧:ignore 掉那两个调度信号

讲完内核提供的观测工具,最后补一个调试器层面的技巧,它和 [运行、读输出与排坑](../02_environment/03-posix-run.md) 里讲过的那个坑是同一件事,这里呼应一下。如果你想在 host 模拟下用 GDB 调试 FreeRTOS demo,会发现自己根本没法正常单步——每走一步都可能断在一个 `SIGUSR1` 或 `SIG34` 上。这不是你的代码炸了,而是 POSIX port 内部用这一类信号在 pthread 之间做调度同步(相当于用信号来模拟 MCU 上「触发一次上下文切换」)。GDB 默认会把这些信号当成「值得停下来看看」的事件,于是你就被刷屏、断个不停。

对策是进 GDB 后立刻执行这一行:

```
handle SIGUSR1 SIG34 nostop noprint
```

`nostop` 是收到信号也不停下、`noprint` 是不打印通知,这样这些调度信号就被静默放行,你才能正常打断点、单步。强烈建议把它写进你的 `~/.gdbinit`,省得每次手敲。这件事的更完整背景(为什么是这两个信号、它和 host 模拟的调度实现什么关系)在 [运行、读输出与排坑](../02_environment/03-posix-run.md) 和 [仿真坑点](../../pitfalls/) 的「GDB 信号干扰」条里都有,这里不重复展开,只记住「调试 host demo 前先 ignore 掉这两个信号」这条操作结论即可。

---

概念铺到这里就齐了,但要真正「看见」这些工具的输出,得让它们跑起来。[下一篇](./02-debugging-demo.md) 我们写一个独立 demo,把 configASSERT、状态快照、运行时统计三件串起来跑出真实输出。
