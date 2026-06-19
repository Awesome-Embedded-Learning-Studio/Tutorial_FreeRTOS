---
title: 独立 demo:多个任务 set 不同的位,一个任务等组合条件
description: 跟着仓库 code/09_event-groups/ 的 demo 走——模拟一个系统启动序列,三个子系统任务异步初始化、各自往同一事件组 set 自己那一位,协调任务用 AND 等待「三个位全部到位」宣布开机完成,再演一把 OR 等待和超时,把事件组的创建、置位、AND/OR 等待、清除、超时一整套 API 在一个程序里走一遍
---

# 独立 demo:多个任务 set 不同的位,一个任务等组合条件

← 返回 [事件组](./index.md)　·　上一页 [事件组概念](./01-concepts.md)

概念铺够了,我们写一个能跑的 demo,把事件组的创建、置位、AND 等待、清除、OR 等待、超时这一整套在一个程序里全演一遍。demo 在仓库的 `code/09_event-groups/` 里。它的设定很贴近真实:模拟一个系统的启动序列——有三个独立的子系统(网络、传感器、日志),各自异步初始化、初始化完了就往一块公共的事件组里 set 自己那一位;另有一个协调任务(Coordinator)用 AND 等待「三个位全部到位」,到齐了宣布开机完成。这之后我们再演一把 OR 等待和超时,把另外两个常考点也覆盖了。

先看三个子系统任务中的一个,它们三个长得几乎一样,只是延时和 set 的位不同:

```c
/* NetTask:模拟网络子系统初始化。延时 NETWORK_INIT_MS 后,往事件组 set BIT_NETWORK_READY。 */
static void prvNetTask( void *pvParameters )
{
    ( void ) pvParameters;
    console_print( "[net]     init started\n" );
    vTaskDelay( pdMS_TO_TICKS( NETWORK_INIT_MS ) );
    console_print( "[net]     init done, set BIT_NETWORK_READY\n" );
    xEventGroupSetBits( xBootEvents, BIT_NETWORK_READY );
    /* set 完就退出:显式自删。 */
    vTaskDelete( NULL );
}
```

注意这里三个子系统任务互相不认识、谁也不等谁,它们各自 `vTaskDelay` 一段错开的时间(网络 300ms、传感器 800ms、日志 1500ms)模拟「异步初始化」,到点了就 `xEventGroupSetBits` 把自己那一位摆上板子。这个「多个独立的 setter 往同一块板上写不同的位」正是事件组「多对一」的典型姿态——产生事件的代码之间没有任何同步关系。

接着是重头戏,协调任务的 AND 等待。它死等「三个位全部到位」:

```c
EventBits_t xBits = xEventGroupWaitBits( xBootEvents,
                                         BITS_ALL_READY,
                                         pdTRUE,    /* 清除等到的位 */
                                         pdTRUE,    /* AND:全部到位才返回 */
                                         portMAX_DELAY );
console_print( "[coord]   ALL READY (bits=0x%lx) -> all systems go!\n",
               ( unsigned long ) xBits );
```

这五行就是「等多事件全部就绪」的标准写法。`BITS_ALL_READY` 是三个位的或掩码,`xWaitForAllBits=pdTRUE` 把它变成 AND(必须三个都置位),`portMAX_DELAY` 死等。在日志位(最慢的那个,1500ms)set 上去之前,这次调用会一直 blocked——这时协调任务不占 CPU,内核在每次有任务 set 位时自动检查它的条件。返回值 `bits=0x7` 说明返回那一刻三个位(bit0、bit1、bit2)都是 1,印证「等齐了」。`xClearOnExit=pdTRUE` 让它在返回时把三个位原子地清掉,我们紧接着用 `xEventGroupGetBits` 打印,你会看到 `0x0`,亲眼确认「ClearOnExit 把位清了」。

demo 的第二段专门演 OR 等待和超时,这两个是新手最容易踩的考点。我们故意只 set 一个位、然后用 OR 模式(任意一个就返回)+ 短超时:

```c
/* 只 set 一个 BIT_NETWORK_READY,然后用 OR 等「网络 OR 传感器」。 */
xEventGroupSetBits( xBootEvents, BIT_NETWORK_READY );

/* 第一次 OR 等待:位已就绪,立刻返回,返回值包含 BIT_NETWORK_READY。 */
xBits = xEventGroupWaitBits( xBootEvents,
                             BIT_NETWORK_READY | BIT_SENSOR_READY,
                             pdTRUE,    /* 返回时清掉等到的位 */
                             pdFALSE,   /* OR:任一就绪即返回 */
                             pdMS_TO_TICKS( 200 ) );
console_print( "[coord]   OR-wait returned bits=0x%lx (net ready, immediate)\n",
               ( unsigned long ) xBits );

/* 第二次 OR 等待:位已被清、无人再 set,200ms 后超时返回。 */
xBits = xEventGroupWaitBits( xBootEvents,
                             BIT_NETWORK_READY | BIT_SENSOR_READY,
                             pdTRUE, pdFALSE,
                             pdMS_TO_TICKS( 200 ) );
console_print( "[coord]   OR-wait timed out, bits=0x%lx (none of net|sensor set)\n",
               ( unsigned long ) xBits );
```

这两次 OR 等待合起来把「OR 模式」和「超时返回」一次演清。第一次:网络位已经 set,OR 等待立刻满足,返回 `bits=0x1`(只有 bit0)。第二次:位被上一次的 `xClearOnExit` 清掉了、又没人再 set,于是这次 OR 等待走满 200ms 超时,返回 `bits=0x0`——`uxBitsToWaitFor` 那些位一个都不在,这正是「我是超时返回的、不是我等的事件来了」的判据。`xEventGroupWaitBits` 超时返回**不是错误**、不停机,它只是把控制权还给你、让你自己看返回值决定怎么办。

构建运行还是老配方(`stdbuf -oL`、`timeout` 为什么不能少,全在 [02 环境搭建](../02_environment/) 讲过):

```bash
cd code/09_event-groups
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/09_event-groups
```

运行起来你会看到这样的输出,三个子系统按错开的时刻就绪,协调任务一直等到最慢的日志到位才被「解锁」:

```
09_event-groups: boot-coordination demo (net=300 ms, sensor=800 ms, log=1500 ms)
[coord]   waiting for ALL subsystems (AND) ...
[net]     init started
[sensor]  init started
[log]     init started
[net]     init done, set BIT_NETWORK_READY
[sensor]  init done, set BIT_SENSOR_READY
[log]     init done, set BIT_LOG_READY
[coord]   ALL READY (bits=0x7) -> all systems go!
[coord]   after clear-on-exit, group bits = 0x0
[coord]   set only BIT_NETWORK_READY, then OR-wait net|sensor
[coord]   OR-wait returned bits=0x1 (net ready, immediate)
[coord]   OR-wait timed out, bits=0x0 (none of net|sensor set)
[coord]   demo complete
```

读这条输出时盯住时序:网络在 300ms 就 `init done` 并 set 了位,传感器 800ms、日志 1500ms,但协调任务的 `ALL READY` 那行要等到 1500ms 日志到位之后才打出来——这说明 AND 等待真的在「等齐全部三个位」之前一直阻塞着,中间网络和传感器先后到位都没能解除它,只有最后一个位(日志)补上才解锁。`bits=0x7` 印证了返回那一刻三个位全是 1。紧接着 `group bits = 0x0` 印证 `xClearOnExit=pdTRUE` 把它们清了。后半段 OR 等待的 `bits=0x1` 和超时的 `bits=0x0` 一眼就能对上我们前面讲的两段逻辑。这份输出没有断言、没有 hang,平平整整跑完,本身就是「事件组这套 API 在 host 模拟下行为正确」的证据。

---

下一页 [dashboard 增量:display 等组合门](./03-dashboard.md) 把事件组嫁接到贯穿全教程的仪表盘脊柱上。
