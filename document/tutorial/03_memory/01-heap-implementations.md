---
title: 五种堆实现与观测探针
description: 内存章概念篇——任务和内核对象的内存从哪来、为什么内存在任务之前、heap_1~heap_5 五种实现的区别与选型、configTOTAL_HEAP_SIZE 旋钮与 xPortGetFreeHeapSize/xPortGetMinimumEverFreeHeapSize 两把探针、malloc failed hook
---

# 五种堆实现与观测探针

> 内存章第一篇(概念/原理)。这一篇结束时,你会清楚 FreeRTOS 的堆从哪来、五种实现怎么选、怎么观测它。下一篇 [独立 demo](./02-standalone-demo.md) 我们把这些概念在一个能跑的 demo 里串起来。[返回本章导航](./index.md)。

## 这章解决什么问题

上一章我们把环境搭起来、跑通了第一个 blinky,但有一件事一直被刻意绕开——blink 起来的那些任务、队列、定时器,它们到底住在内存的哪儿?一个任务光栈就要占地方,你 `xTaskCreate` 的时候并没有自己 `malloc`,那这块栈是谁分配的?这一章就是把这件事讲透:FreeRTOS 的动态内存从哪来、谁在管、怎么观测它。

我们先把结论摆在前面:在 FreeRTOS 里,任务(以及队列、信号量、定时器、事件组这些内核对象)的内存,**默认全都是从 FreeRTOS 自己管的一块堆里 `pvPortMalloc` 出来的**。`xTaskCreate` 内部会去 `pvPortMalloc` 一块栈、再 `pvPortMalloc` 一个 TCB(任务控制块)。换句话说,**堆是任务的家,得先把堆准备好,才能创建任务**——这就是为什么这一章排在任务(下一章)前面。理解了这一点,你以后看到 `xTaskCreate` 返回失败、或者运行中莫名其妙 `malloc failed`,第一反应才会是「是不是堆太小了」,而不是漫无目的地翻任务代码。

FreeRTOS 一个很特别的设计是:它**不假定你的目标平台有 `malloc`**。真 MCU 上、尤其小 MCU,标准库 `malloc` 又慢又容易把内存搅碎,很多时候根本不可用。所以 FreeRTOS 自己提供了 **五种**堆管理实现,叫 heap_1 到 heap_5,你根据自己系统的特性挑一个编进去。它们都实现了同一组 API(`pvPortMalloc` / `vPortFree`),但内部策略天差地别——有的根本不能 free、有的会合并碎片、有的还能把几块不连续的内存拼成一个堆。这一章我们先把这五种实现的区别和选型讲清楚,然后亲手在一个 demo 里分配、释放、观测 free heap 的变化。

## 五种堆实现:heap_1 到 heap_5

你可以在内核源码的 `portable/MemMang/` 目录下看到这五个文件,`heap_1.c` 到 `heap_5.c`。我们编哪一个进工程,就决定了 `pvPortMalloc` 走哪一套逻辑。本教程为了能在 PC 上观察内存行为,把 demo 和 dashboard 都切到了 **heap_4**——具体原因后面讲 demo 的时候会展开,这里先逐个认识它们。

heap_1 是**最简单的那一个,简单到只有 `pvPortMalloc`、没有真正能用的 `vPortFree`**。它内部维护一块静态数组当堆,分配指针 `xNextFreeByte` 单调递增,要多少就往前推多少。它压根不支持释放(官方说法是:绝大多数 FreeRTOS 系统创建完所有任务和内核对象后,就再也不会释放它们,这种「只分配不释放」的场景用 heap_1 最省)。它的好处是确定性极强、代码极短、没有碎片问题(因为根本不回收),适合那种「上电初始化一把分配完,之后只读不释放」的简单嵌入式系统。

heap_2 在 heap_1 的基础上**加上了 `vPortFree`**,内部用一个空闲块链表来管理,可以回收内存。但它有一个明显短板:**释放时不会把相邻的空闲块合并**。也就是说,你连续分配三块再释放掉中间那块、又释放掉边上那块,它们在链表里仍然是两个独立的小块,哪怕它们物理上挨在一起。这会导致**内存碎片**——free heap 数字上还有不少,但因为都碎成小块,一个稍大的请求反而分不出来。heap_2 现在已经不推荐新项目用了,基本被 heap_4 取代。

heap_3 是一个**特殊存在**,它根本不自己管堆,而是**对编译器/标准库自带的 `malloc` / `free` 做一层薄包装**——加上 `vTaskSuspendAll`/`xTaskResumeAll` 保证线程安全(因为标准库的 `malloc` 通常不是可重入的)。它没有自己的 `ucHeap[]` 数组,堆就是宿主环境(linker 配的那块)的堆。我们在 [02 环境搭建](../02_environment/) 里跑的 blinky、以及 dashboard 的默认配置用的就是 heap_3,因为宿主机上 glibc 的 `malloc` 成熟稳定,没必要自己另起炉灶。但 heap_3 有一个对我们本章很关键的副作用:**它不实现 `xPortGetFreeHeapSize` / `xPortGetMinimumEverFreeHeapSize` 这两个观测函数**——你根本问不出「堆还剩多少」,因为那要靠宿主机的 allocator 内部状态,FreeRTOS 不好封装。所以本章的 demo 想观察 free heap 变化时,不能再用 heap_3。

heap_4 是**heap_2 的改良版,也是现代 FreeRTOS 最常用的选择**。它和 heap_2 一样用空闲块链表、支持 `vPortFree`,关键区别是:**释放时会合并(coalesce)相邻的空闲块**。两块挨着的空闲内存会被合成一个更大的块,从而把碎片问题压到最小。它还维护一个 `xPortGetMinimumEverFreeHeapSize`,记录历史上 free heap 跌到过的**最低水位**——这数特别有用:它告诉你「最忙的时候,堆被吃掉了多少」,从而你能判断 `configTOTAL_HEAP_SIZE` 留得够不够、还有多少余量。本章 demo 和 dashboard 都用 heap_4,就是冲着这两个观测函数和它的合并特性来的。

heap_5 是**在 heap_4 之上,再支持「堆由多块不连续的内存拼成」**。有些 MCU 的 RAM 是分段的(比如一块紧挨着一段外挂 SRAM),heap_4 只能用一个连续数组,heap_5 允许你传一个 `HeapRegion_t` 数组,把好几块地址不连续的内存登记进去,当成一个大堆用。代价是你必须在第一次 `pvPortMalloc` 之前、也就是创建任何任务之前,先调 `vPortDefineHeapRegions()` 把这几块内存告诉它——这个顺序铁律很容易忘,忘了就直接崩。host 模拟下我们内存是一整块连续的,用不上 heap_5,但你要知道有它,以后碰到分段 RAM 的板子不会两眼一抹黑。

把这五种放到一起对比,选型其实挺清晰的:只创建不释放的极简系统用 heap_1;绝大多数需要动态分配/释放的现代项目用 heap_4;宿主机/有成熟 malloc 的环境用 heap_3;RAM 分段用 heap_5;heap_2 基本是历史遗留,新项目别选。

## 两个关键配置和两把探针

讲完了五种实现,我们聚焦到本章 demo 真正会用到的几个东西上,它们是堆管理的「旋钮」和「仪表」。

第一个旋钮是 `configTOTAL_HEAP_SIZE`。对于 heap_1/2/4/5,它决定那块静态数组 `ucHeap[]` 一共多大;对于 heap_3,它没用(堆是宿主机管的)。这个值你要根据「我打算创建多少任务、多少队列、多少定时器,每个多大」来估——所有这些内核对象的栈和 TCB 都从这儿出。估小了就会 `malloc failed`,估大了就浪费 RAM。怎么知道估得够不够?这就需要探针。

第一把探针是 `xPortGetFreeHeapSize()`,它返回**此刻**堆里还剩多少字节。你分配一拨东西之后调一下,就能看到 free heap 掉了多少;释放之后再调一下,就能看到涨回来没有(用 heap_4 涨得回来,用 heap_2 因为碎片可能涨不回满)。

第二把探针是 `xPortGetMinimumEverFreeHeapSize()`,它返回**从上电到现在,free heap 跌到过的最低值**。这把探针才是判断 `configTOTAL_HEAP_SIZE` 够不够的真正依据:你跑遍所有最吃内存的路径之后,看一眼最低水位离 0 还有多远。如果最低水位已经贴近 0,说明堆已经被压到极限,稍微再多分配一点就要 `malloc failed`,这时候就该把 `configTOTAL_HEAP_SIZE` 抬大了。注意这把探针**只有 heap_4 和 heap_5 实现**,heap_1/2/3 都没有——heap_3 下它压根不存在,heap_1/2 下会返回 0。

还有最后一块拼图:`vApplicationMallocFailedHook`。当 `pvPortMalloc` 分配失败、且 `FreeRTOSConfig.h` 里 `configUSE_MALLOC_FAILED_HOOK` 设成 1 时,内核会调这个 hook。它在 `app_hooks.c` 里(各 demo 的样板代码),默认实现是打一行 `ASSERT FAILED` 然后停下——这就是你以后在排错章会反复看到的那行输出的来源。它不是普通错误,而是「堆已经分配不出来了,继续跑下去只会更糟」的硬刹车。

---

下一篇:[独立 demo——动手分配、释放、观测](./02-standalone-demo.md)。
