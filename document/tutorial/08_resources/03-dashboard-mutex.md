---
title: dashboard 增量:给共享统计计数上互斥量
description: 把互斥量嫁接到贯穿全教程的 dashboard 脊柱——新增一组统计计数器(总采样数/按钮采样数/总显示数),sensor/display 在临界区里写、低优 Stats 任务 take→拷快照→give 再在锁外打印,印证「临界区要尽量短」;末尾收全章小结,链向下一章事件组
---

# dashboard 增量:给共享统计计数上互斥量

> 这一篇是资源管理章的 dashboard 集成篇。概念与原理在 [互斥量、优先级反转与死锁:原理](./01-mutex-concepts.md),反转 demo 在 [独立 demo:复现反转,用互斥量破解](./02-priority-inversion-demo.md);这一篇把互斥量落进贯穿全教程的 dashboard 脊柱,并在末尾收全章小结。

## dashboard 增量:给共享统计计数上互斥量

学完独立 demo,我们把这个能力嫁接到 dashboard 脊柱上。dashboard 此前几章已经长成了「sensor 采集 → 队列 → display 显示 + 模拟按钮中断 + 内存监控」的形态。本章增量是**给一个真正会被多个任务并发写的共享资源上一把互斥量**:新增一组统计计数器(总采样数、按钮采样数、总显示数),sensor 任务每采一帧就在临界区里把「总采样数」(按钮帧还额外加「按钮采样数」)+1,display 任务每显示一帧就把「总显示数」+1,再起一个优先级最低的 Stats 任务周期性地 take 同一把锁、读一份快照、打印。

这里有个设计上的小讲究,正好印证上一节「临界区要尽量短」。Stats 任务是这组计数器的读者,它也要持锁——不持锁的话,它可能读到「采样数刚自增完、显示数还没自增」这种互相不一致的撕裂态。但持锁不代表要把整个 `console_print`(慢 I/O)关在锁里。正确写法是:take 锁 → 把整组计数器拷到一个局部变量 → 立刻 give → 然后在锁外慢慢格式化打印。这样临界区只覆盖「拷三个字」那么短,既保证了快照一致,又不阻塞别的任务。我们看 sensor 侧的写和 Stats 侧的读:

```c
/* sensor / display 写:整段「读改写」用互斥量包成临界区。 */
xSemaphoreTake( xStatsMutex, portMAX_DELAY );
{
    g_xStats.ulTotalSamples++;
    if( fFromButton )
    {
        g_xStats.ulButtonSamples++;
    }
}
xSemaphoreGive( xStatsMutex );

/* Stats 任务读:take → 拷走整组 → give,锁只护「拷」这一下,打印在锁外。 */
StatsCounters_t xSnapshot;
xSemaphoreTake( xStatsMutex, portMAX_DELAY );
xSnapshot = g_xStats;
xSemaphoreGive( xStatsMutex );

console_print( "stats: samples=%lu (buttons=%lu)  displays=%lu\n",
               ( unsigned long ) xSnapshot.ulTotalSamples,
               ( unsigned long ) xSnapshot.ulButtonSamples,
               ( unsigned long ) xSnapshot.ulTotalDisplays );
```

注意这里用的是 `xSemaphoreCreateMutex`(互斥量)而不是二值信号量,正是因为它带优先级继承:Stats 任务拿锁读快照时,若被高优先级的 sensor 抢、sensor 又撞在这把锁上,sensor 会把 Stats 临时抬到自己的高优先级,让它赶紧 give、自己早拿锁——本章 demo 里演过的优先级继承,在这里是真在起作用的。构建运行还是老配方:

```bash
cd code/dashboard
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 4 ./build/dashboard
```

输出里多了一行 `stats:`,正是受互斥量保护的那组计数器的周期快照:

```
dashboard: starting scheduler (heap=2097152 bytes, sample=800 ms, button=2500 ms)
heap monitor: free=1571024  min_ever=1571024
sensor: sample #0 value=0 (periodic) sent to queue
display: showing sample #0 value=0 (sampled 0 ms ago, prev #0)
sensor: sample #1 value=1 (periodic) sent to queue
display: showing sample #1 value=1 (sampled 0 ms ago, prev #0)
heap monitor: free=1571024  min_ever=1571024
stats: samples=2 (buttons=0)  displays=2
sensor: sample #2 value=2 (periodic) sent to queue
display: showing sample #2 value=2 (sampled 0 ms ago, prev #1)
sensor: sample #3 value=3 (BUTTON! extra sample) sent to queue
display: showing sample #3 value=3 (sampled 0 ms ago, prev #2)
sensor: sample #4 value=4 (periodic) sent to queue
display: showing sample #4 value=4 (sampled 0 ms ago, prev #3)
```

`stats: samples=2 (buttons=0) displays=2` 这行说明:到那一刻 sensor 一共产出了 2 帧(都是周期帧、没有按钮帧),display 也显示了 2 帧——计数器互相对得上,没有丢更新,这正是互斥量在守着。再往下 sensor 采到第 3 帧时标了 `BUTTON! extra sample`,这就是 [07 中断管理](../07_interrupts/) 那把按钮信号量触发的一次额外采样,后续的 stats 快照里 `buttons=` 那个计数就会跟着涨。顺带留意 `heap monitor: free=1571024`:上一章这个数还是 1703272,本章掉到了 1571024,差了约 131KB,正好是一个任务栈(Stats 任务)加互斥量开销的量级——又是「每多一个任务,堆就多掉一个栈」这条老规律在起作用,内存监控这个常驻水位计继续忠实地记录着脊柱的生长。

## 小结

走到这里,资源管理的全貌就拼起来了。**互斥量**用 `xSemaphoreCreateMutex` 创建、`xSemaphoreTake` 拿、`xSemaphoreGive` 还,三者必须严格配对嵌套,只有持锁任务能 give,它把一段「读改写」包成不可分割的临界区,保护被多任务并发访问的共享资源。**优先级反转**是 RTOS 里最反直觉的坑:最高优先级任务被一个和资源无关的中等优先级任务间接饿死,根因不在优先级设置、而在保护资源用的锁类型。**互斥量的优先级继承**破解反转——持锁者在有高优先级任务等它的锁时被临时抬优先级,从而不受中优先级干扰地把锁尽快还出去,这是 mutex 相对二值信号量唯一、也是决定性的区别。**死锁**来自加锁顺序不一致,优先级继承救不了它,唯一靠谱的预防是全系统固定加锁顺序、且临界区尽量短(在锁里只拷快照、出锁再处理)。选型上一条硬规矩记死:**ISR↔任务同步用信号量,任务↔任务互斥才用互斥量**。

下一章 [事件组](../09_event-groups/) 我们进入一种更灵活的同步原语——一个事件组是一组二进制位,多个任务可以各自 set 不同的位、一个任务可以等「这几个位全置 1(AND)」或「任意一个置 1(OR)」的组合条件,特别适合「等多件事都就绪再动手」这种信号量和互斥量都不擅长的场景。至于「互斥量在仿真和真 MCU 上有没有行为差异」「优先级继承在哪些移植下被裁剪过」这类系统性坑点,收在 [仿真坑点](../../pitfalls/) 里,值得对照本章读一遍。
