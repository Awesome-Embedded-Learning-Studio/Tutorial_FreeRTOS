---
title: 故障博物馆:五类故障的复现 demo
description: 跟着仓库 code/13_troubleshooting/ 的「故障博物馆」demo 走——一个 Driver 任务顺序跑五个互相隔离的小场景,复现 malloc failed 观测、栈溢出盲区、优先级反转、队列满丢消息、死锁,每个都带真实运行输出和「现象 → 原因 → 对策」解读
---

# 故障博物馆:五类故障的复现 demo

上一页我们把速查表逐条展开了,这一页配上能跑的 demo,让你在真实输出里看到每类故障「长什么样」、对策怎么落地。demo 在仓库的 `code/13_troubleshooting/` 里,它的组织方式和前面每章的 demo 都不一样:不是一个常驻多任务应用,而是一个**「故障博物馆」**——一个 Driver 任务顺序跑五个互相隔离的小场景,每个场景复现一类经典故障,在输出里打印「现象 → 原因 → 对策」,跑完一个把它建出来的任务/原语清掉,再进下一个。这样设计有两个好处:一是所有故障能在一次 build/run 里走完,二是彼此不串味——不会因为 A 场景挂了拖死 B 场景。这套「把可疑现象隔离成最小复现」的做法本身就是调试时的核心技巧。

先看这个 demo 的骨架——Driver 任务按阶段号推进五个场景,每个场景只在自己那个阶段号里活,阶段一切换,上一场景残留的子任务看到号不对就自删退场:

```c
/* DriverTask:顺序跑完五个场景。每个场景置阶段号 → 调该场景的 prvStageXxx
 * (它会建子任务、观察、清掉子任务)→ 阶段号变更让残留子任务自删 → 进下一场景。 */
static void prvDriverTask( void *pvParameters )
{
    ( void ) pvParameters;
    vTaskDelay( pdMS_TO_TICKS( 200 ) );
    console_print( "13_troubleshooting: fault museum, running 5 stages sequentially\n" );

    g_ulStage = STAGE_HEAP;        prvStageHeap();
    g_ulStage = STAGE_STACK;       prvStageStack();
    g_ulStage = STAGE_INVERSION;   prvStageInversion();
    g_ulStage = STAGE_QUEUE_FULL;  prvStageQueueFull();
    g_ulStage = STAGE_DEADLOCK;    prvStageDeadlock();

    console_print( "\n13_troubleshooting: all 5 stages done. end of fault museum.\n" );
    vTaskDelete( NULL );
}
```

构建和运行还是老配方(`stdbuf -oL`、`timeout` 为什么不能少,全在 [02 环境搭建](../02_environment/) 讲过):

```bash
cd code/13_troubleshooting
cmake -B build -DNO_TRACING=1
cmake --build build
stdbuf -oL timeout 9 ./build/13_troubleshooting
```

跑起来你会看到五个场景依次出场,每个场景在输出里都有清晰的 `[stage N]` 标题和「现象 → 对策」的注解。我们逐个读。

## 场景 1:内存余量观测,提前看见 malloc failed

第一个场景我们**故意不真把堆撑爆**——撑爆会进 `vApplicationMallocFailedHook` → `vAssertCalled` 死循环,demo 当场停住、再也演不了后面四个场景。这正是「真 malloc failed 的现象」之一:hook 触发、程序停在 assert。所以这一幕演的是更值钱的东西——**预防**:用 `xPortGetMinimumEverFreeHeapSize` 在 malloc failed 还没发生时就读出「堆已经吃到了哪里」。alloc 几块 8KB、量一次水位,free 掉再量一次:

```c
size_t ulFreeBefore = xPortGetFreeHeapSize();
size_t ulMinBefore = xPortGetMinimumEverFreeHeapSize();
/* ... 分配 6 块 8KB ... */
console_print( "  after allocating 6 x 8192 bytes: free=%lu, min_ever=%lu\n", ... );
/* ... 全部 vPortFree ... */
console_print( "  after freeing all: free=%lu, min_ever=%lu (min_ever did NOT rise back)\n", ... );
```

运行输出是这样的:

```
[stage 1] heap instrumentation —— how to SEE malloc-failure coming
  free=1965848 bytes, min_ever=1965848 bytes (baseline)
  after allocating 6 x 8192 bytes: free=1916600, min_ever=1916600
  after freeing all: free=1965848, min_ever=1916600 (min_ever did NOT rise back)
  -> if min_ever ever sits near 0, a malloc-failed is one allocation away.
     (a real malloc failed would call vApplicationMallocFailedHook -> vAssertCalled -> halt)
```

读这一段要盯住 `min_ever` 那个数。分配 48KB 之后 `free` 从 1965848 掉到 1916600,`min_ever` 跟着掉——它记的是「自启动以来的历史最低点」,只要 free 创了新低它就跟着降。但全部 `vPortFree` 之后,`free` 涨回 1965848,`min_ever` 却**纹丝不动**,还是 1916600。这就是「高水位线」的精髓:它只升不降,刻下的是「曾经最紧张的一刻」——哪怕你现在全还了,那个最低点依然在那儿供你判断「堆是不是曾经差一点就爆了」。排 malloc failed 时盯 `min_ever` 远比盯 `free` 有用:free 高不代表安全(可能下一秒一个大分配就触底),`min_ever` 贴近 0 才是真危险信号。这也是前面坚持用 heap_4 的根本原因——heap_3 压根不提供这俩函数,你连「曾经掉到多低」都看不见。

## 场景 2:栈溢出在 host 下是盲区,老实承认

第二个场景是最需要「诚实交代」的一个。我们让一个任务故意深递归吃栈(每层 512B 局部数组),递归到深度 5/20/80,每次前后量 `uxTaskGetStackHighWaterMark`,预期看到水位随深度下降——结果在 host 下**纹丝不动**:

```
[stage 2] stack overflow —— on POSIX neither the hook NOR the probe fires
  configCHECK_FOR_STACK_OVERFLOW=0 (hook won't fire on POSIX port regardless)
  recursion depth=  5: high_water 16379 -> 16379 words (UNCHANGED — probe unreliable on POSIX)
  recursion depth= 20: high_water 16379 -> 16379 words (UNCHANGED — probe unreliable on POSIX)
  recursion depth= 80: high_water 16379 -> 16379 words (UNCHANGED — probe unreliable on POSIX)
  -> host is blind to stack overflow: the hook never fires (POSIX port)
     AND the high-water probe barely moves (real frames live on the pthread stack).
     on real HW: set configCHECK_FOR_STACK_OVERFLOW=2 — both the hook AND the
     probe work there, because the kernel owns the task stack directly.
```

为什么不灵?根源还是 POSIX port 的实现:内核给任务的 `pxStack` 缓冲区涂了 0xa5 魔数,`uxTaskGetStackHighWaterMark` 靠扫这块缓冲区里「还没被踩的魔数」来算剩余栈;但任务**真正的调用帧压在 pthread 自己的栈上**,根本不碰内核那块 `pxStack`。所以你递归再深,魔数区域都不动,水位线永远报同一个数。这和 `configCHECK_FOR_STACK_OVERFLOW` 那个 hook 在 host 下永远不触发是同一根因——内核够不着 pthread 的栈。我们完全可以伪造一个「漂亮」的递减数字糊弄过去,但那对读者有害无益,所以这里老实演出「探针不可信」、把可靠诊断留给真硬件(`configCHECK_FOR_STACK_OVERFLOW=2`)。这个「不演假结果」的态度,是排错章该有的。

## 场景 3:优先级反转,高优任务被中优拖了 300ms

第三个场景复现经典三任务反转:低优 `Low` 持二值信号量(故意不带优先级继承)、干长活;高优 `High` 随后要这把信号量被阻塞;中优 `Mid` 上线忙等,抢占 `Low`,把 `High` 活活拖住:

```
[stage 3] priority inversion + starvation (binary semaphore, no inheritance)
  [LOW] acquiring binary semaphore, will hold it while working...
  [HIGH] need the resource, blocking on semaphore...
  [MID] preempting and busy-spinning (starves LOW, blocks HIGH indirectly)
  [LOW] work done, releasing semaphore
  [HIGH] got it after 300 ms (if >>0, priority inversion happened)
  -> fix: use xSemaphoreCreateMutex() instead — it does priority inheritance,
     so LOW is boosted to HIGH's priority and Mid can't preempt it.
```

关键就一行:`[HIGH] got it after 300 ms`。`High` 是系统里优先级最高的任务,它要一把信号量,按理说 `Low` 一还它就该立刻拿到,延迟应该接近 0;可这里它硬等了 300ms——这 300ms 就是「反转」的代价,完全是 `Mid`(优先级比 `High` 低)造成的。读输出要理解这条时间线:`Low` 拿锁开始忙等干活(本该很快还锁),`High` 来撞锁被阻塞,这时 `Mid` 上线——`Mid` 优先级高于 `Low`,一把把 `Low` 抢走,`Low` 没法继续干、还不掉锁,`High` 就只能干等 `Mid` 忙完。`Mid` 的存在本不该影响 `High`,却因为 `Low` 夹在中间持锁,把 `High` 的延迟传染给了 `Mid`,这就是「反转」。对策就一句:把二值信号量换成 `xSemaphoreCreateMutex()`——mutex 带优先级继承,`Low` 持锁时被 `High` 撞上会被临时抬到 `High` 的优先级,`Mid` 就抢不动它,反转消失。详细的 mutex vs 二值信号量对比和优先级继承机制,在 [08 资源管理章](../08_resources/)的独立 demo 里完整演过,那里有「反转前 vs 继承后」的对照输出。

## 场景 4:队列满,非阻塞 send 狂丢消息

第四个场景复现最常见的「队列丢消息」:快生产者往容量只有 2 格的队列狂 `xQueueSend`(零超时),慢消费者 80ms 才取一帧,几毫秒队列就满,之后生产者的发送全是 `errQUEUE_FULL`:

```
[stage 4] queue full —— non-blocking send drops messages
  producer attempted 441461 sends, 441444 dropped (errQUEUE_FULL)
  -> fixes: (a) xQueueOverwrite for latest-value semantics (capacity 1),
     (b) xQueueSend with a block timeout to back-pressure the producer,
     (c) counting-semaphore token budget to gate the producer.
```

这一幕的现象是触目惊心的:`producer attempted 441461 sends, 441444 dropped`——生产者尝试了 44 万次发送,其中 44 万次全丢了,只有 17 次成功挤进队列被消费掉。丢消息率接近 99.996%。而且关键是:**整个过程没有报错、没有 assert、程序照常跑**——这正是这类 bug 最坑的地方,它静默地发生,你要是只看「程序在跑」根本发现不了数据在少。生产者的循环就两行,撞满队列就默默计数:

```c
if( xQueueSend( xSmallQueue, &ulValue, 0 ) != pdPASS )
{
    g_ulDropped++;    /* 队列满,这一帧丢了 */
}
```

对策三条,选哪条取决于「丢帧能不能容忍」:最新值语义 `xQueueOverwrite`(丢最旧、容量为 1,适合采样这种「只在乎最新」的场景,dashboard 脊柱就用这招)、带阻塞超时反压(生产者满了就等消费者,一帧不丢但生产者被阻塞)、计数信号量配额(生产者发一帧 take 一个 token、消费者消费一帧 give 回,硬性钳速)。详细对比在 [05 队列章](../05_queues/)。

## 场景 5:死锁,用超时探测而不是真挂

第五个场景复现死锁,但**用超时探测而不是真挂**——真挂会让 demo 卡死、Ctrl-c 才能退,对读者不友好。两个任务各先拿一把 mutex、再去抢对方的另一把,而且顺序相反(A 先 A 后 B、B 先 B 后 A),构成环形等待。第二个 take 带超时,到点拿不到就证明卡在环形等待里:

```
[stage 5] deadlock —— reverse lock ordering (detected via timeout)
  [B] locked B, now wants A (reverse order -> circular wait)
  [A] locked A, now wants B
  [B] timed out waiting for A —— DEADLOCK detected!
  [A] locked B too (no deadlock)
  deadlock DETECTED (timed-out second take proved the circular wait)
  -> fix: enforce a single global lock-acquisition order (always A before B)
     so a circular wait can never form. Timeout-based takes catch it in debug.
```

读这段输出要注意它的时序:`[A] locked A` 和 `[B] locked B` 两行说明两个任务各先拿到了自己的第一把锁——A 持 A、B 持 B;然后它们都要对方的第二把锁,A 等 B 手里的 B、B 等 A 手里的 A,环形等待成形。`[B] timed out waiting for A` 这行是死锁的铁证——B 带 300ms 超时去拿 A,到点还拿不到,说明它被 A 卡死了。输出里 `[A] locked B too (no deadlock)` 这行不是「没死锁」,而是 B 超时放弃后把 B 释放了,A 这才在它的超时窗口内拿到 B——这是「带超时探测」带来的附带效果:超时让其中一方退让,打破环。但诊断已经成立(那个 timed-out 就是死锁证据)。对策是治本的一招:**全局统一的锁获取顺序**(整个代码库永远先 A 后 B),环形等待就构不成。带超时的 take 不能预防死锁,但能在 debug 时探测到它——这正是本章 demo 选「探测」而不是「真挂」的意义。

## 小结

跑完这个故障博物馆,你应该带走两样东西。第一样是**那张速查表的肌肉记忆**:看到 `ASSERT FAILED` 先想 malloc failed,看到高优任务延迟先想优先级反转,看到数据少了先想队列满,看到任务集体卡住先想死锁,看到新任务不跑先想 starvation 或优先级设错——这套「现象 → 原因」的对应关系,是排错时最快的那一步。第二样是**这套 demo 的组织方法论**:把可疑现象隔离成最小复现、用超时/计数/水位线去「看见」它、对症给对策——这个「隔离 + 探测 + 对策」的套路,搬到任何真实工程里都通用。

还有两个 host 模拟特有的诚实边界,这个 demo 演得最透:一是**真 malloc failed 和真死锁会挂掉 demo**,所以我们用超时探测代替真挂——真硬件上你照样会看到真挂,但那时你已对照过速查表;二是**host 下栈溢出是盲区**(hook 不触发、水位线不动),可靠诊断靠真硬件 `configCHECK_FOR_STACK_OVERFLOW=2`。这两条都收在[仿真坑点](../../pitfalls/),移到真板子前务必建立预期。下一章[项目实战](../14_project-dashboard/)我们离开排错、把贯穿全教程的 dashboard 脊柱推到完成态——你会看到所有这些原语(以及它们的坑)在一个真实多任务应用里如何协作拼装。
