---
title: 独立 demo:分配、释放、观测
description: 内存章独立 demo 篇——用 code/03_memory 这个能跑的 demo 把 heap_4 的概念串起来:切堆到 heap_4、把 configTOTAL_HEAP_SIZE 抬到 2MB、探针包成一行打印、分配/释放循环、逐行读懂 free 与 min_ever 的输出
---

# 独立 demo:动手分配、释放、观测

> 内存章第二篇(独立 demo)。[上一篇](./01-heap-implementations.md) 讲清了五种堆和两把探针的概念,这一篇把它们写进 `code/03_memory` 这个能跑的 demo 里。下一篇 [dashboard 增量](./03-dashboard-monitor.md) 把这套观测能力嫁接到脊柱工程上。

## 独立 demo:动手分配、释放、观测

光讲概念不过瘾,我们写一个能跑的 demo 把上面这些串起来。demo 在仓库的 `code/03_memory/` 目录,它做的事很简单:创建一个内存任务,在里面 `pvPortMalloc` 一批小对象,每个对象分配前后都用两把探针探一下 free heap,然后再逐个 `vPortFree`,看 free heap 怎么涨回来。结构和 [02 环境搭建](../02_environment/) 里的 dashboard demo 一样——`CMakeLists.txt`、`FreeRTOSConfig.h`、`console.c/.h`、`app_hooks.c` 都是直接复用模板,我们只写 `main.c` 的逻辑。

先说一个配置上的细节,这也是本章最「折腾」的一个点。模板默认编的是 heap_3,但我们这个 demo 要用 `xPortGetFreeHeapSize`,而 heap_3 不实现它——链接阶段会直接报 `undefined reference to 'xPortGetFreeHeapSize'`。所以我们在 demo 的 `CMakeLists.txt` 里把 `FREERTOS_HEAP` 从 `3` 改成 `4`,编译 heap_4 进来。这是有意识的选择:heap_4 能观测、能合并碎片,正适合拿来教学。

然后是一个会让你血压升一截的坑。heap_3 默认下,任务的栈和 TCB 是从**宿主机 glibc 的堆**里出的,那个堆基本无限大,所以 02 章里 `configTOTAL_HEAP_SIZE` 写多少都不影响。可一旦切到 heap_4,堆就变成了**固定大小的静态数组 `ucHeap[]`,大小严格等于 `configTOTAL_HEAP_SIZE`**。而 POSIX port 下 `configMINIMAL_STACK_SIZE` 被设成了 `PTHREAD_STACK_MIN`(16384),注意栈深度的单位是「字」不是字节,`StackType_t` 在 64 位上是 `unsigned long` 也就是 8 字节——所以一个任务的栈是 `16384 × 8 = 128KB`。一个任务光栈就要从堆里吃掉 128KB,你 `configTOTAL_HEAP_SIZE` 留个 65KB 的话,`xTaskCreate` 内部一 `pvPortMalloc` 就分不出来,直接触发 `malloc failed hook`,你会看到一行 `ASSERT FAILED` 然后 demo 死在原地。

所以 demo 里 `configTOTAL_HEAP_SIZE` 被抬到了 2MB,给这些「巨型」任务栈留足空间。这件事在真 MCU 上完全不存在——真 MCU 一个任务栈通常就几百到几千字节,`configTOTAL_HEAP_SIZE` 几 KB 就够,这里是 host 模拟独有的体量错觉,我们顺手认识一下。

配置就这两处改动(heap 改 4、堆抬到 2MB),其余模板原样。下面是 demo 的内存任务逻辑,我们先看分配这一段。它维护一个指针数组,逐个 `pvPortMalloc` 一个 `SensorReading_t`(12 字节的小结构体),每分配一个就探一次针:

```c
/* 探针:把当前 free heap 和历史最低 free heap 一起打出来。
 * 两个数能讲清楚两件事——前者是「现在还剩多少」,后者是「峰值时被吃掉多少」。 */
static void prvReportHeap( const char *pcTag )
{
    size_t xFreeNow = xPortGetFreeHeapSize();
    size_t xFreeMin = xPortGetMinimumEverFreeHeapSize();

    console_print( "[heap] %-12s free=%6lu  min_ever=%6lu\n",
                   pcTag, ( unsigned long ) xFreeNow, ( unsigned long ) xFreeMin );
}
```

这段就是把两把探针包成一行打印。两个数并列着看特别有信息量:`free` 是当前余量,`min_ever` 是历史最低水位。接下来是分配循环,逐个 `pvPortMalloc`,每分配一个就探一次:

```c
for( size_t i = 0; i < uxObjectCount; i++ )
{
    pxReadings[ i ] = ( SensorReading_t * ) pvPortMalloc( uxPerObject );

    if( pxReadings[ i ] != NULL )
    {
        pxReadings[ i ]->lTimestampMs = ( int32_t ) ( i * 100 );
        pxReadings[ i ]->lValue       = ( int32_t ) ( i * i );
        pxReadings[ i ]->ucSlot        = ( uint8_t ) i;
    }

    console_print( "memory task: alloc[%lu] => %s\n",
                   ( unsigned long ) i,
                   pxReadings[ i ] != NULL ? "ok" : "FAILED" );
    prvReportHeap( "after alloc" );
    vTaskDelay( pdMS_TO_TICKS( 150 ) );
}
```

分配完检查返回值是不是 `NULL`,是的话说明 `pvPortMalloc` 没分出来(在正确配置下这里应该一直是 `ok`,如果看到 `FAILED` 就是堆不够了)。每个动作之间 `vTaskDelay` 一下,既让输出可读,也让别的任务有机会跑。释放循环结构对称,逐个 `vPortFree`,释放前先 `memset` 清掉内容——这是个好习惯,养成「释放即不再访问」的肌肉记忆,以后能少踩很多野指针的坑。

demo 在进 scheduler 之前、`xTaskCreate` 之前会先打印一行 `total heap`,这是「内存在任务之前」的直接证据——这时候一个任务还没创建,我们已经能说出堆一共多大。不过要注意,heap_4 是**惰性初始化**的:它的堆区虽然是编译期就存在的静态数组,但内部的空闲块链表要等到第一次 `pvPortMalloc` 才在 `prvHeapInit()` 里搭起来,所以进 scheduler 之前调 `xPortGetFreeHeapSize()` 会返回 0(还没初始化,不是「满」)。真正的 baseline 我们在任务里、scheduler 把那几个系统任务弄好之后再探。

构建和运行跟 02 章一样的配方,只是 demo 目录换成 `code/03_memory`:

```bash
cd code/03_memory
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/03_memory
```

(那个 `timeout 4` 是为了抓一段输出就停,真要长跑去掉它就行;`stdbuf -oL` 的来历见 [02 环境搭建的排坑页](../02_environment/03-posix-run.md)。)运行起来你会看到这样的输出:

```
03_memory: total heap = 2097152 bytes
03_memory: starting scheduler
memory task: object size = 12 bytes, will allocate 8 of them
[heap] baseline     free=1965848  min_ever=1965848
memory task: alloc[0] => ok
[heap] after alloc  free=1965816  min_ever=1965816
memory task: alloc[1] => ok
[heap] after alloc  free=1965784  min_ever=1965784
memory task: alloc[2] => ok
[heap] after alloc  free=1965752  min_ever=1965752
memory task: alloc[3] => ok
[heap] after alloc  free=1965720  min_ever=1965720
memory task: alloc[4] => ok
[heap] after alloc  free=1965688  min_ever=1965688
memory task: alloc[5] => ok
[heap] after alloc  free=1965656  min_ever=1965656
memory task: alloc[6] => ok
[heap] after alloc  free=1965624  min_ever=1965624
memory task: alloc[7] => ok
[heap] after alloc  free=1965592  min_ever=1965592
memory task: free[0]
[heap] after free   free=1965624  min_ever=1965592
memory task: free[1]
[heap] after free   free=1965656  min_ever=1965592
...
memory task: free[7]
[heap] after free   free=1965848  min_ever=1965592
memory task: cycle done, looping
[heap] steady cycle free=1965848  min_ever=1965592
```

这份输出值得逐行读。先看 `baseline`:`free=1965848`,而 `total heap = 2097152`,两者一减是 `131304`——这就是「一个 Memory 任务 + 系统内部开销」从堆里吃掉的量,大头正是前面算的那个 128KB 任务栈。这串数直观地告诉你「任务一启动,堆就先掉一大块」,比任何文字描述都清楚。

再看分配阶段:每分配一个 12 字节的对象,`free` 掉的不是 12 而是 **32**。多出来的那 20 字节是 heap_4 的开销——`pvPortMalloc` 会给每个块前面塞一个 `BlockLink_t` 头(64 位上 16 字节,记录块大小和链表指针),再把请求大小连同这个头一起按 8 字节对齐(`12 + 16 = 28`,对齐到 32)。这就是「你 malloc 12 字节,实际从堆里拿走 32 字节」的真相,在估算 `configTOTAL_HEAP_SIZE` 时这笔账得算进去。

接着看释放阶段:`free` 一格一格精确涨回来,8 次释放后回到 `1965848`,和 baseline 一模一样。这就是 heap_4 合并(coalesce)机制的功劳——相邻的空闲块被合成大块,没有碎片残留,所以分配-释放是「守恒」的。最后那个 `steady cycle` 行是 demo 进入无限循环后,每轮分配 8 个再全部释放一次的稳态读数,`free` 稳定停在 1965848,证明没有内存泄漏。

最有意思的是 `min_ever` 这一列。它在分配到第 7 个时跌到 `1965592`(最低水位),之后开始释放,`free` 虽然一路涨回来,`min_ever` 却**纹丝不动停在 1965592**。这正是 `xPortGetMinimumEverFreeHeapSize` 的设计:它只记最低、不往回涨,像一个「最高水位线」记号。你跑完整个应用最吃内存的路径之后看一眼它,就知道 `configTOTAL_HEAP_SIZE` 顶到头被吃了多少——如果这个数离 0 太近,就该抬堆了。这是本章最该带走的一个习惯。

---

下一篇:[dashboard 增量——加一个内存余量监控](./03-dashboard-monitor.md)。
