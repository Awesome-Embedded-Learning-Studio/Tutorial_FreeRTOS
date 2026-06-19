---
title: dashboard 增量:内存余量监控
description: 内存章 dashboard 增量篇——往脊柱工程 code/dashboard 加一个轻量的常驻内存余量监控任务、运行输出解读、本章小结,以及为什么 free 会比独立 demo 少一个任务栈的量
---

# dashboard 增量:加一个内存余量监控

> 内存章第三篇(收尾)。[上一篇](./02-standalone-demo.md) 在独立 demo 里把 heap_4 的探针跑通了,这一篇把同样的观测能力嫁接到贯穿全教程的 dashboard 脊柱工程上,做一个常驻后台的水位计,并收本章小结。

## dashboard 增量:加一个内存余量监控

学完了独立 demo,我们把这个能力嫁接到贯穿全教程的 dashboard 脊柱上。dashboard 现在只有一个心跳任务(每秒打一行 `alive`),本章增量是再加一个**内存余量监控任务**:它周期性地把 free heap 和最低水位打出来,像一个常驻后台的水位计。

和独立 demo 同样的原因,dashboard 的 `CMakeLists.txt` 也把 `FREERTOS_HEAP` 改成了 4、`FreeRTOSConfig.h` 里把 `configTOTAL_HEAP_SIZE` 抬到 2MB——否则 `xPortGetFreeHeapSize` 链接不出来、任务栈也装不下。`app_hooks.c` 等样板不动,只往 `main.c` 加一个新任务。监控任务极其轻量,它不分配、不释放任何东西,只是定时读两把探针:

```c
/* 内存余量监控任务:每 2 秒探一次堆,把当前 free heap 和历史最低水位打出来。
 * 这是个典型的「轻量周期性监控」——它不分配、不释放任何东西,只是读数,
 * 所以开销极低,适合常驻在后台盯余量。后续章节任务越加越多、对象越创建越多,
 * 你会看到 free heap 缓慢下降、min_ever 一路走低;哪天 min_ever 贴近 0,
 * 就是在提醒你 configTOTAL_HEAP_SIZE 该抬了。 */
static void prvHeapMonitorTask( void *pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        console_print( "heap monitor: free=%lu  min_ever=%lu\n",
                       ( unsigned long ) xPortGetFreeHeapSize(),
                       ( unsigned long ) xPortGetMinimumEverFreeHeapSize() );
        vTaskDelay( pdMS_TO_TICKS( 2000 ) );
    }
}
```

它的优先级和心跳任务一样,挂在 idle 之上,确保只在系统有空时才跑,绝不和正经干活的任务抢 CPU。构建运行:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出长这样,心跳和内存监控两条线交替出现:

```
dashboard: starting scheduler (heap=2097152 bytes)
dashboard: alive
heap monitor: free=1834560  min_ever=1834560
dashboard: alive
heap monitor: free=1834560  min_ever=1834560
dashboard: alive
dashboard: alive
```

这里 `free=1834560`,比独立 demo 的 `1965848` 少了约 131KB——因为 dashboard 有两个应用任务(心跳 + 监控),多一个任务就多一个 128KB 栈,这和我们在 demo 里看到「一个任务吃 131KB」的账对得上。随着后续章节我们往 dashboard 里继续加任务、加队列、加定时器,你会看到这个 `free` 一路缓慢下降、`min_ever` 一路刷新最低——这个监控任务就是给我们留的一个长期水位计,哪天它的读数开始逼近 0,就是该回头审视堆大小的时候了。

## 小结

走到这里,FreeRTOS 的内存图景就清楚了几件事:**堆是任务的家,所以内存在任务之前**,这是本章排在任务章前面的原因;**五种堆实现各有定位**,只分配不释放选 heap_1、需要释放的现代项目选 heap_4、宿主机有成熟 malloc 选 heap_3、RAM 分段选 heap_5、heap_2 基本被淘汰;**`configTOTAL_HEAP_SIZE` 是堆大小的旋钮,`xPortGetFreeHeapSize` 看当前余量,`xPortGetMinimumEverFreeHeapSize` 看历史最低水位**,后者是判断堆够不够的真正依据;**`vApplicationMallocFailedHook` 是堆分不出来的硬刹车**,看到 `ASSERT FAILED` 就先怀疑堆太小。还有一个 host 模拟的体量错觉:POSIX port 下一个任务栈有 128KB,所以 `configTOTAL_HEAP_SIZE` 要抬到 MB 级,这和真 MCU 上的几百字节栈完全是两个世界。

下一章 [任务管理](../04_tasks/) 我们正式进入 `xTaskCreate`——既然已经知道任务的栈和 TCB 从堆里来,就可以动手创建任务、给它们排优先级、观察调度器怎么在它们之间切换了。至于「host 模拟和真 MCU 在内存上的行为差异」——比如栈大小的天差地别、heap_3 在宿主机上为何能无限分配——属于系统性的仿真坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照着本章读一遍。
