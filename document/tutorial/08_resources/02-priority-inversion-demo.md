---
title: 独立 demo:复现反转,用互斥量破解
description: 跟着仓库 code/08_resources/ 的独立 demo 走一遍——三个错峰任务(Low 持锁、High 撞锁、Mid 插队忙等)先用二值信号量复现优先级反转(High 被无关的中优先级任务间接饿死、足足等 2800ms),再只改一个编译开关换成互斥量,优先级继承一行业务代码都不用动就把 High 的等待时间从 2800ms 砍到 960ms
---

# 独立 demo:复现反转,用互斥量破解

> 这一篇是资源管理章的 demo 篇。概念与原理(互斥量是什么、它和二值信号量的决定性区别、优先级反转怎么发生、死锁怎么预防)在上一篇 [互斥量、优先级反转与死锁:原理](./01-mutex-concepts.md);这一篇只演反转这一件事;把这个能力落进 dashboard 脊柱的写法在 [dashboard 增量:给共享统计计数上互斥量](./03-dashboard-mutex.md)。

## 独立 demo:亲手复现反转,再用互斥量破解

概念讲完,我们写一个能跑的 demo 把反转演出来、再演一遍「换成互斥量它就消失」。demo 在仓库的 `code/08_resources/`,它安排了三个任务:一个 Low(优先级 1)先拿锁、持锁期间故意磨蹭;一个 High(优先级 5)随后撞锁、blocked;一个 Mid(优先级 3)最后插进来,纯跑一段又长又跟锁无关的忙等。三者的起跑时刻刻意错开(Low 在 300ms、High 在 600ms、Mid 在 900ms),保证因果顺序是「Low 先持锁 → High 后撞锁 → Mid 再插队」,反转才演得出来。每一步都打一行带 tick 的时间戳,这样 High 到底「等了多久才拿到锁」一目了然。

demo 用一个编译时开关 `USE_MUTEX` 选择锁的类型:不定义它就是二值信号量(复现反转),`cmake -DUSE_MUTEX=1` 定义它就是互斥量(破解反转)。两种锁在业务代码里 take/give 一字不改,差别只在创建那一行。这正是要给你看的点:**同一份逻辑,换个锁类型,调度行为天差地别**。我们先看锁的创建分流:

```c
/* 唯一的分流点:用哪种锁。
 *   - 不定义 USE_MUTEX → xSemaphoreCreateBinary():创建为「空」,必须先 give 一次
 *     让它变「满」才能 take 到(二值信号量经典初值坑);无优先级继承。
 *   - 定义 USE_MUTEX → xSemaphoreCreateMutex():创建即「满」,可直接 take;
 *     且内核会做优先级继承。 */
#ifdef USE_MUTEX
    xResourceLock = xSemaphoreCreateMutex();
#else
    xResourceLock = xSemaphoreCreateBinary();
    xSemaphoreGive( xResourceLock );   /* 二值信号量创建为空,先填满 */
#endif
```

三个任务的骨架我们挑 Low 和 Mid 看一眼,因为 High 那边只是「撞锁、等、拿到」三步,逻辑很直白。Low 拿锁后会持锁一段时间,这段持锁窗口必须够长,长到能覆盖「High 撞锁 + Mid 插队」全程,反转才演得完:

```c
static void prvLowTask( void *pvParameters )
{
    ( void ) pvParameters;
    vTaskDelay( pdMS_TO_TICKS( LOW_START_MS ) );

    xSemaphoreTake( xResourceLock, portMAX_DELAY );   /* 拿锁 */
    prvUseSharedResource( "Low" );

    /* 持锁期间故意再拖一会儿,留出窗口让 High 撞锁、Mid 插队,反转才演得出来。 */
    vTaskDelay( pdMS_TO_TICKS( LOW_HOLD_MS ) );

    xSemaphoreGive( xResourceLock );   /* 关键:Low 啥时候能跑到这行 give,决定 High 命运 */
    vTaskDelete( NULL );
}
```

注意那个 `vTaskDelay` 持锁期间的拖拽——它本身会主动让出 CPU,所以「Low 持锁却占着 CPU 不放」不是这里的故障源;真正的故障源是「Mid 抢走 CPU 后,Low 这个 vTaskDelay 醒不来、走不到下面那行 give」。Mid 那边则是个纯粹的搅局者,既不让出 CPU、也不碰锁,一上来就忙等一大段:

```c
static void prvMidTask( void *pvParameters )
{
    ( void ) pvParameters;
    vTaskDelay( pdMS_TO_TICKS( MID_START_MS ) );

    /* 纯忙等:不开锁、不碰锁、不让出。优先级高于 Low、此刻 High 又在 blocked 等锁,
     * 于是调度器眼里「ready 里最高」就是它——它独占 CPU,把 Low 按在原地。 */
    volatile unsigned long ulJunk = 0;
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < pdMS_TO_TICKS( MID_BURN_MS ) )
    {
        ulJunk += 1;
    }
    vTaskDelete( NULL );
}
```

先跑**二值信号量**这一版,也就是默认构建(不加 `USE_MUTEX`),命令还是老配方:

```bash
cd code/08_resources
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/08_resources
```

(命令里那几个开关的来历、`stdbuf -oL` 为什么不能少、`timeout 4` 是怎么回事,都在 [02 环境搭建](../02_environment/) 里讲过,这里不重复。)运行起来,反转清清楚楚地发生了:

```
08_resources: lock = BINARY SEMAPHORE (NO priority inheritance)
08_resources: starting scheduler (prio High=5 Mid=3 Low=1)
[   301 ms] Low: trying to TAKE lock
[   301 ms] Low: lock TAKEN
[   301 ms] Low: INSIDE critical section, holding lock
[   361 ms] Low: leaving critical section
[   601 ms] High: trying to TAKE lock (blocked expected)
[   901 ms] Mid: START long unrelated work (burning 2500 ms, NOT touching lock)
[  3401 ms] Mid: long work DONE
[  3401 ms] Low: GIVING lock
[  3401 ms] High: lock TAKEN after 2800 ms wait
[  3401 ms] High: INSIDE critical section, holding lock
[  3401 ms] Low: done, deleting self
```

逐行读这个时间线。Low 在 301ms 拿到锁,361ms 就离开了临界区(它那段 `prvUseSharedResource` 干完了),按理说它该在持锁拖拽期(1200ms)结束后、大约 1561ms 把锁 give 掉。但 High 在 601ms 来撞锁、blocked 等锁;紧接着 Mid 在 901ms 插进来,一跑就是整整 2500ms 的纯忙等。Mid 优先级(3)高于 Low(1),Low 那个 `vTaskDelay` 的拖拽根本醒不来——它被 Mid 牢牢按住、拿不到 CPU、走不到 give 那一行。结果就是 High 这个全系统最高优先级(5)的任务,硬生生从 601ms 等到 3401ms,**足足等了 2800ms**,而且这 2800ms 里 Mid 干的活和 High 想要的资源一点关系都没有。最高优先级被无关的中优先级间接饿死——这就是优先级反转的完整样貌,教科书级别。

现在换互斥量,只改一个构建开关,业务代码一行不动:

```bash
cmake -B build -DNO_TRACING=1 -DUSE_MUTEX=1
cmake --build build
stdbuf -oL timeout 4 ./build/08_resources
```

输出立刻不一样了:

```
08_resources: lock = MUTEX (priority inheritance ON)
08_resources: starting scheduler (prio High=5 Mid=3 Low=1)
[   301 ms] Low: trying to TAKE lock
[   301 ms] Low: lock TAKEN
[   301 ms] Low: INSIDE critical section, holding lock
[   361 ms] Low: leaving critical section
[   601 ms] High: trying to TAKE lock (blocked expected)
[   901 ms] Mid: START long unrelated work (burning 2500 ms, NOT touching lock)
[  1561 ms] Low: GIVING lock
[  1561 ms] High: lock TAKEN after 960 ms wait
[  1561 ms] High: INSIDE critical section, holding lock
[  1621 ms] High: leaving critical section
[  1621 ms] High: done, deleting self
[  3401 ms] Mid: long work DONE
[  3401 ms] Low: done, deleting self
```

对比触目惊心。High 的等待时间从 2800ms 暴跌到 **960ms**(601→1561),而且关键是看 Low 何时 give:这次 Low 在 **1561ms** 就 give 了锁——正是它持锁拖拽期(1200ms)自然结束的时刻,没有被任何人按住。原因就是互斥量的优先级继承在起作用:High 在 601ms 撞锁 blocked 的那一刻,内核发现持锁的 Low 优先级(1)低于等锁的 High(5),于是把 Low **临时抬升**到 High 的优先级(5)。这下 Mid(优先级 3)就再也抢不动 Low 了——Low 抬到了 5,Mid 只有 3,调度器不再让 Mid 插队。于是 Low 顺顺当当跑完它的拖拽期、按时 give,give 的瞬间 Low 优先级降回 1,High 立刻接过锁飞起来跑。Mid 那段长活被推到 3401ms 才干完,但那已经是 High 早就离场之后的事了,没人再受它拖累。

这就是优先级继承的全部魔法:**持锁者在有高优先级任务等它这把锁时,临时获得高优先级,从而「不受中优先级干扰地把锁尽快还出去」**。它没有破坏优先级调度的规则,反而是在维护规则——让真正重要的任务尽快拿到它要的东西。把两段输出摆一起看,你会对「互斥量和二值信号量长得一样、内核天差地别」这句话有切肤的理解。

下一篇 [dashboard 增量:给共享统计计数上互斥量](./03-dashboard-mutex.md) 把这套互斥量能力嫁接到贯穿全教程的 dashboard 脊柱上,你会看到「在锁里只拷快照、出锁再处理」这条纪律在真实应用里怎么落地。
