---
title: 独立 demo:用 xTaskNotify 携带动作码给工作任务下指令
description: 跟着仓库 code/10_task-notifications/ 的 demo 走——一个控制源任务 prvControlTask 用 xTaskNotify 的 eSetValueWithOverwrite 给工作任务 prvWorkerTask 下发动作码(NORMAL/BOOST/SHUTDOWN),工作任务 ulTaskNotifyTake 取出来按动作码分支干活,演活「任务通知带数据」这件事,并把信号量做不到带数据的痛点对照讲清
---

# 独立 demo:用 xTaskNotify 携带动作码,给工作任务下指令

> 任务通知这一章的第二篇。上一篇讲了机制和 API,这一篇配上能跑的 demo,把任务通知「能携带一个 32 位值」这件事演给你看。链回 [本章导航](./) 看全貌,看完这篇去 [dashboard 增量](./03-dashboard-increment.md) 看它怎么嫁接到贯穿全教程的仪表盘上。

概念铺到位,我们写一个能跑的 demo,把任务通知「能携带一个 32 位值」这件事演给你看。demo 在仓库的 `code/10_task-notifications/` 里,它的设计很直接:一个「控制源」任务 `prvControlTask` 周期性地给一个「工作」任务 `prvWorkerTask` 发通知,每次发通知时用 `xTaskNotify` 的 `eSetValueWithOverwrite` 动作,把一个**动作码**(我们定义了 `ACTION_NORMAL`、`ACTION_BOOST`、`ACTION_SHUTDOWN` 三个)写进工作任务的通知值;工作任务 `ulTaskNotifyTake` 死等通知,取到值就 `switch` 按动作码分支干活——`NORMAL` 采一帧、`BOOST` 标记一帧高优先级、`SHUTDOWN` 自删退出。这套「控制源带值通知、工作任务取值分支」是任务通知最典型的用法,正好把信号量做不到的「带数据」演活了。

先看发送侧的控制源任务,这是本章的核心道具——它演示 `xTaskNotify` 怎么把一个动作码塞进通知值:

```c
/* 控制源任务:周期性下发动作码。用 eSetValueWithOverwrite 把动作码覆盖式写进
 * 工作任务的通知值——这是「任务通知带数据」的发送侧。 */
static void prvControlTask( void *pvParameters )
{
    ( void ) pvParameters;

    /* 下发序列:NORMAL → NORMAL → BOOST → NORMAL → SHUTDOWN,把三种动作码都过一遍,
     * 最后用 SHUTDOWN 让工作任务干净退出、demo 自然收尾。 */
    static const uint32_t ulSequence[] =
    {
        ACTION_NORMAL,
        ACTION_NORMAL,
        ACTION_BOOST,
        ACTION_NORMAL,
        ACTION_SHUTDOWN,
    };
    const size_t uxSeqLen = sizeof( ulSequence ) / sizeof( ulSequence[ 0 ] );

    for( size_t i = 0; i < uxSeqLen; i++ )
    {
        uint32_t ulAction = ulSequence[ i ];

        /* xTaskNotify:给 xWorkerTaskHandle 发通知,把 ulAction 覆盖式写进它的通知值。
         *   参数 1:目标任务句柄(任务通知是定向的,必须知道发给谁);
         *   参数 2:要写的 32 位值(这里是动作码);
         *   参数 3:eAction,eSetValueWithOverwrite 表示「直接覆盖通知值」。 */
        xTaskNotify( xWorkerTaskHandle, ulAction, eSetValueWithOverwrite );

        vTaskDelay( pdMS_TO_TICKS( CONTROL_PERIOD_MS ) );
    }

    vTaskDelete( NULL );
}
```

注意 `xTaskNotify` 的第一个参数是**目标任务的句柄** `xWorkerTaskHandle`——这正是任务通知「定向」的体现,你必须告诉它发给谁,不像信号量有个独立对象谁都能 give。接收侧的工作任务用 `ulTaskNotifyTake` 取这个值:

```c
/* 工作任务:在「自己的」通知值上阻塞,被通知就取值、按动作码分支干活。
 * 关键:ulTaskNotifyTake 取出来的是那个 32 位通知值本身(动作码),不是「有没有通知」
 * 这种布尔——控制源用 eSetValueWithOverwrite 把动作码写进来,这里直接读出来 switch。 */
static void prvWorkerTask( void *pvParameters )
{
    ( void ) pvParameters;

    for( ; ; )
    {
        /* ulTaskNotifyTake:在工作任务「自己的」通知上阻塞等待(不需要句柄,每个任务
         * 天生有一个通知)。pdTRUE=取到值后清零通知值,portMAX_DELAY=死等。
         * 返回值就是控制源塞进来的动作码——这就是任务通知「带数据」的现场。 */
        uint32_t ulAction = ulTaskNotifyTake( pdTRUE, portMAX_DELAY );

        switch( ulAction )
        {
            case ACTION_NORMAL:
                console_print( "[worker] notify value=%lu -> NORMAL: take a frame\n",
                               ( unsigned long ) ulAction );
                break;

            case ACTION_BOOST:
                /* 控制源无需另一条带外通道、无需额外信号量,一个动作码就把「该干嘛」
                 * 一次性说清了——这正是任务通知区别于信号量的地方。 */
                console_print( "[worker] notify value=%lu -> BOOST: high-priority frame!\n",
                               ( unsigned long ) ulAction );
                break;

            case ACTION_SHUTDOWN:
                console_print( "[worker] notify value=%lu -> SHUTDOWN: exiting\n",
                               ( unsigned long ) ulAction );
                vTaskDelete( NULL );
                break;

            default:
                console_print( "[worker] notify value=%lu -> unknown action, ignore\n",
                               ( unsigned long ) ulAction );
                break;
        }
    }
}
```

`main()` 里先建工作任务(拿到它的句柄,控制源需要)、再建控制源任务,套路和前几章一致。注意全程没有「创建信号量/队列/事件组」的步骤——任务通知不需要创建,这是它轻量的根源:

```c
/* 先建工作任务,拿到句柄——控制源需要这个句柄才能 xTaskNotify 给它。
 * 任务通知不需要任何「创建」步骤(没有 xTaskNotifyCreate),这也是它比信号量/队列
 * 省一截的根源:省掉了一个独立的同步对象。 */
xTaskCreate( prvWorkerTask, "Worker", configMINIMAL_STACK_SIZE, NULL,
             prioWORKER, &xWorkerTaskHandle );
configASSERT( xWorkerTaskHandle != NULL );

xTaskCreate( prvControlTask, "Control", configMINIMAL_STACK_SIZE, NULL,
             prioCONTROL, NULL );
```

构建和运行还是老配方(`stdbuf -oL`、`timeout` 为什么不能少,全在 [02 环境搭建](../02_environment/) 讲过):

```bash
cd code/10_task-notifications
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/10_task-notifications
```

跑起来你会看到这样的输出——控制源按序列逐条下发动作码,工作任务每次都准确按收到的动作码分支干活:

```
10_task-notifications: notify-with-value demo (NORMAL=100 BOOST=200 SHUTDOWN=999, period=600 ms)
[control] starting, will send 5 notifications
[worker] notify value=100 -> NORMAL: take a frame
[control] sent notify, action=100 (NORMAL)
[worker] notify value=100 -> NORMAL: take a frame
[control] sent notify, action=100 (NORMAL)
[worker] notify value=200 -> BOOST: high-priority frame!
[control] sent notify, action=200 (BOOST)
[worker] notify value=100 -> NORMAL: take a frame
[control] sent notify, action=100 (NORMAL)
[worker] notify value=999 -> SHUTDOWN: exiting
[control] sent notify, action=999 (SHUTDOWN)
[control] sequence done, control task exiting
```

仔细读这份输出,有两个细节值得你停下来想想。第一个是 `[worker]` 那一行总是出现在紧随其后的 `[control] sent` 之前——明明是控制源先 `xTaskNotify`、再 `console_print`「sent」,为什么 worker 的打印反而更早?因为工作任务优先级更高(`prioWORKER` 比 `prioCONTROL` 高一档),`xTaskNotify` 一执行、worker 被唤醒进就绪态,调度器立刻就把它切上去跑了(它把那行 `console_print` 打完、回到 `ulTaskNotifyTake` 阻塞后),控制源才有机会继续执行自己的 `console_print`。这条「notify 之后高优先级任务几乎立刻就跑」的现象,正是任务通知唤醒路径短、响应快的直观证据。

第二个细节是整个序列干净收尾:`SHUTDOWN` 那条让工作任务 `vTaskDelete(NULL)` 自删,控制源下完序列也自删,两个任务都消失后调度器只剩空闲任务空转,`timeout 4` 到点把进程收掉。这里没有用到「信号量 + 全局变量」这种带外通道——所有「该干嘛」的信息都靠那一个 32 位通知值传过去。如果你把这套需求换成用二值信号量实现,会立刻撞上信号量的天花板:信号量 `give` 带不了动作码,你得另开一个全局变量 `g_ulAction`、控制源先写这个变量再 give 信号量、工作任务 take 到信号量后再去读这个变量——两件事(写变量、give)之间、以及工作任务(读变量、take)之间,天然埋着「写了变量还没 give」或「give 了变量还没更新」的竞态窗口,你得加锁或精心排序才能消掉。任务通知用「通知值即数据」一把把这个竞态消掉,这是它结构性胜过信号量的地方。

独立 demo 看完了,下一篇 [dashboard 增量](./03-dashboard-increment.md) 我们把这个「带值的控制通道」嫁接到贯穿全教程的仪表盘上,sensor 任务靠它切换采样模式。
