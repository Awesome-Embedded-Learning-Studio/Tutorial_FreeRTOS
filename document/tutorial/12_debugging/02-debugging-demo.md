---
title: 独立 demo:断言、状态快照、运行时统计三连
description: 跟着仓库 code/12_debugging/ 的独立 demo 走——故意触发一次 configASSERT 看 vAssertCalled 打的 ASSERT FAILED、用 uxTaskGetSystemState 拍全任务状态快照、折算出各任务的 CPU 占用,带真实运行输出和逐行解读
---

# 独立 demo:断言、状态快照、运行时统计三连

> 调试这一章第二篇。[上一篇](./01-debugging-concepts.md) 把工具的概念和机制讲透了,这一篇把它们串成一个能跑的独立 demo,让你亲眼看见 `ASSERT FAILED` 长什么样、状态快照表怎么读、CPU 占用百分比怎么折算出来。回[章节导航](./index.md)。

概念铺到位,我们写一个能跑的 demo 把前面讲的工具串起来。demo 在仓库的 `code/12_debugging/` 里,它做三件事,正好对应三件工具:(a) 故意触发一次 `configASSERT`,看 `vAssertCalled` 打的那行 `ASSERT FAILED`;(b) 用 `uxTaskGetSystemState` 拍全任务状态快照(并探测 `vTaskList` 那条路的开关状态);(c) 在快照里折算出各任务的 CPU 占用。为了 (b)(c) 有意义,demo 起了几个负载任务:两个「忙等一段、睡一段」的周期 Worker(忙等/睡眠比不同,占用不同),加一个半忙等的 `IdleBurner`(几乎一直在 ready/running,抢走大量本该归 idle 的 CPU),这样运行时统计里能看到鲜明的占用差异。一个优先级最低的 `Debug` 任务周期性拍快照。

先看负载任务,这是「制造可统计的 CPU 占用」的部分:

```c
/* 周期 Worker:每轮忙等 ulWorkTicks 个 tick(占用 CPU),再 vTaskDelay 200ms(让出)。
 * 两个 Worker 传不同的 ulWorkTicks,运行时统计里占用就不同。 */
static void prvPeriodicWorker( void *pvParameters )
{
    uint32_t ulWorkTicks = ( uint32_t ) ( uintptr_t ) pvParameters;

    for( ; ; )
    {
        /* 忙等:纯空转占用 CPU,模拟「在干活」。这段会被运行时统计记成它的占用。 */
        TickType_t xStart = xTaskGetTickCount();
        while( ( xTaskGetTickCount() - xStart ) < ulWorkTicks )
        {
        }

        /* 睡一段(进 blocked,把 CPU 让给别人)。 */
        vTaskDelay( pdMS_TO_TICKS( 200 ) );
    }
}
```

然后是核心的 `Debug` 任务,它把 (b)(c) 一次做完——拍快照、排序、按运行时占用算百分比打印:

```c
configRUN_TIME_COUNTER_TYPE ulTotalRunTime = 0;
UBaseType_t uxCount = uxTaskGetSystemState( xStatus, MAX_TASKS_IN_SNAPSHOT, &ulTotalRunTime );

/* 按运行时占用降序排,最吃 CPU 的排最上面。任务数少,O(n^2) 选择排序无所谓。 */
for( UBaseType_t i = 0; i + 1 < uxCount; i++ )
{
    for( UBaseType_t j = i + 1; j < uxCount; j++ )
    {
        if( xStatus[ j ].ulRunTimeCounter > xStatus[ i ].ulRunTimeCounter )
        {
            TaskStatus_t xTmp = xStatus[ i ];
            xStatus[ i ] = xStatus[ j ];
            xStatus[ j ] = xTmp;
        }
    }
}

/* 占用 % = ulRunTimeCounter × 100 / 总运行时。不足 1% 的标 <1%。 */
console_print( "  %-12s %-8s prio  freestack   CPU%%\n", "name", "state" );
for( UBaseType_t i = 0; i < uxCount; i++ )
{
    uint32_t ulPct = ( uint32_t ) ( ( xStatus[ i ].ulRunTimeCounter * 100UL ) / ulTotalRunTime );
    /* ...格式化成 "NN%" 或 "<1%" 塞进同一行... */
}
```

构建运行还是老配方(两个开关的来历、`stdbuf -oL` 为什么不能少,全在 [02 环境搭建](../02_environment/) 里讲过):

```bash
cd code/12_debugging
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/12_debugging
```

默认编译(`ASSERT_DEMO_ENABLED=0`)你会看到周期性的状态快照 + CPU 占用表:

```
12_debugging: starting scheduler (heap=2097152 bytes, max_prio=7, ASSERT_DEMO=0)
============ debug snapshot @ tick 1031 ============
  name         state    prio  freestack   CPU%
  IdleBurn     Ready      1      16379   86%
  WorkerA      Blocked    3      16379   9%
  WorkerB      Blocked    2      16379   4%
  Debug        Running    1      16379   <1%
  IDLE         Ready      0      16379   <1%
  Tmr Svc      Blocked    6      32763   <1%
  (vTaskList is disabled; set configUSE_STATS_FORMATTING_FUNCTIONS=1 to enable)
============ debug snapshot @ tick 2031 ============
  name         state    prio  freestack   CPU%
  IdleBurn     Ready      1      16379   84%
  WorkerA      Blocked    3      16379   10%
  WorkerB      Blocked    2      16379   4%
  Debug        Running    1      16379   <1%
  IDLE         Ready      0      16379   <1%
  Tmr Svc      Blocked    6      32763   <1%
  (vTaskList is disabled; set configUSE_STATS_FORMATTING_FUNCTIONS=1 to enable)
```

这张表信息量很足,值得逐行读。先看 `CPU%` 那一列——这就是 `configGENERATE_RUN_TIME_STATS` 的成果:`IdleBurn` 占了 86%,因为它是个半忙等任务、几乎一直在 ready/running,把本该归 idle 的 CPU 几乎全抢走了;`WorkerA` 占 9%、`WorkerB` 占 4%,差距来自它们各自的忙等/睡眠比不同(WorkerA 忙等 20 tick、WorkerB 忙等 10 tick);`Debug` 自己 `<1%`,因为它一秒才醒一次、拍完快照立刻又睡;`IDLE` 也 `<1%`,因为 `IdleBurn` 把它的份额吃光了——这正是「有个任务在死循环式地抢 CPU」的典型信号,真实项目里你看到 idle 占比反常地低,就该去查是不是哪个任务没好好让出 CPU。注意 `IdleBurn` + `WorkerA` + `WorkerB` 加起来约 99%,剩下的零头分给了 `Debug`/`IDLE`/`Tmr Svc`,近似 100%,这说明统计是自洽的。

再看 `state` 那一列,它就是任务状态机的实时切片:`Debug` 是 `Running`(拍快照这一刻正是它在跑);被它观测的那些负载任务此刻大多 `Blocked`(各自的 `vTaskDelay`/`taskYIELD` 让出后没轮到);`IDLE` 是 `Ready`(随时能兜底)。`freestack` 全是 16379(单位是字),说明 `configMINIMAL_STACK_SIZE` 给的栈绰绰有余、离溢出还远——host 模拟下栈虚胖到 128KB,这个数字会一直很大;真 MCU 上它才是你收紧 `usStackDepth` 的依据。最后那行 `(vTaskList is disabled; ...)` 是我们对 `configUSE_STATS_FORMATTING_FUNCTIONS` 开关的探测结果,提醒你 `vTaskList` 这条路是关着的、想要就在 `FreeRTOSConfig.h` 置 1。

实验 (a) 那个断言演示,要把 `main.c` 顶部的 `ASSERT_DEMO_ENABLED` 从 `0` 改成 `1` 重新编译再跑。它起一个一次性任务,延时 800ms 后故意把优先级设成非法值 `configMAX_PRIORITIES`(正好越界 1),触发内核的 `configASSERT`:

```bash
# 改成 #define ASSERT_DEMO_ENABLED    ( 1 ) 后:
cmake --build build
stdbuf -oL timeout 4 ./build/12_debugging
```

```
12_debugging: starting scheduler (heap=2097152 bytes, max_prio=7, ASSERT_DEMO=1)
assert-demo: waiting 800 ms before triggering configASSERT...
assert-demo: deliberately setting priority to illegal value 7 (max=7)
ASSERT FAILED: /home/charliechen/Tutorial_FreeRTOS/third_party/FreeRTOS/FreeRTOS/Source/tasks.c:2778
```

看到了——`ASSERT FAILED: tasks.c:2778`,这就是 `vAssertCalled` 打的那行,它把内核源文件 `tasks.c` 的第 2778 行甩给你。那一行正是 `vTaskPrioritySet` 开头的 `configASSERT( uxNewPriority < configMAX_PRIORITIES )`,你一眼就能定位「是优先级越界触发的」。打完这行后程序就卡在 `vAssertCalled` 的 while 循环里不再往下(这正是「断言炸了就别往下走」的现场),所以后面的快照不再打印——`timeout 4` 到点杀掉它退出。这就是一次完整的「写错用法 → configASSERT 拦截 → vAssertCalled 报点」链路,真硬件上行为完全一致。演示完记得把 `ASSERT_DEMO_ENABLED` 改回 `0` 再编译,才能看到连续的 (b)(c) 输出。

---

独立 demo 跑通了,接下来把这套 CPU 占用统计的能力嫁接到贯穿全教程的 dashboard 脊柱上。[下一篇](./03-debugging-dashboard.md) 加一个周期 CPU 占用统计任务,并收束本章小结。
