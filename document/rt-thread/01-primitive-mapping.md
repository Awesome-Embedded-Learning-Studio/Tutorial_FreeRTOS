---
title: 原语映射大表:FreeRTOS API → RT-Thread API → 差异注解
description: 逐原语对比 FreeRTOS 与 RT-Thread——任务(rt_thread vs xTask)、消息队列与 RT-Thread 独有的邮箱、信号量、互斥量(都有优先级继承)、事件集、软件定时器、内存管理(small/slab/memheap + mempool),以及 RT-Thread 无直接等价物的任务通知
---

# 原语映射大表:FreeRTOS API → RT-Thread API → 差异注解

> 这是对比轨的第一篇正文。我们逐个原语把 FreeRTOS 的写法摆出来、把 RT-Thread 的对应写法摆出来,然后讲清楚两者哪里能无痛平替、哪里语义不一致得小心。第二篇 [调度与配置哲学差异](./02-scheduling-and-config.md) 跳出单原语看宏观。

在展开之前,有一个贯穿全篇的心智模型要先建立:**RT-Thread 的 IPC 原语统统是「内核对象」,创建一律有「动态 `create`」和「静态 `init`」两条路**。动态那条(`rt_xxx_create`)在堆上分配对象结构体、用完 `rt_xxx_delete` 回收;静态那条(`rt_xxx_init`)要求你事先准备好一块静态 `struct rt_xxx` 结构体和(需要的)缓冲区,`init` 只是把它登记进内核对象容器、`detach` 把它摘下来不释放内存。这正好对应 FreeRTOS 里 `xSemaphoreCreateMutex()`(动态)和 `xSemaphoreCreateMutexStatic()`(静态)的区分,只不过 RT-Thread 把这套做成 IPC 对象的统一惯例,而 FreeRTOS 是靠加 `Static` 后缀的平行 API。记住「`create`/`init` 二选一」这条线,后面每个原语都不会乱。

## 任务:rt_thread 对 xTask

任务是两边最像的原语,迁移几乎无痛。FreeRTOS 的 `xTaskCreate` 想要入口函数、栈大小、参数、优先级、句柄输出;RT-Thread 的 `rt_thread_create` 想要的是**名字、入口函数、参数、栈大小、优先级、时间片**——思路完全对齐,只是多了一个显式的线程名和一个时间片参数。

```c
/* FreeRTOS */
static void task_entry(void *arg);
xTaskCreate(task_entry, "sensor", 256, NULL, 3, &handle);

/* RT-Thread */
static void thread_entry(void *parameter);
tid = rt_thread_create("sensor", thread_entry, RT_NULL,
                       512,            /* 栈大小(字节) */
                       3,              /* 优先级,数值越小越高 */
                       10);            /* 时间片 */
rt_thread_startup(tid);                /* 必须 startup 才进入就绪 */
```

有几个差异得心里有数。第一,RT-Thread 的 `rt_thread_create` 只是把线程造出来停在初始态,**必须再调一次 `rt_thread_startup` 它才进就绪队列参与调度**,而 FreeRTOS 的 `xTaskCreate` 建完就直接就绪了,这一步漏掉是新手上 RT-Thread 最经典的「线程为什么不跑」。第二,**优先级数值的方向两边是反的,这是迁移最容易翻车的点**:FreeRTOS 是数值越大优先级越高(`tskIDLE_PRIORITY`=0 是最低、idle 占着,你的任务优先级往上加);RT-Thread 是数值越小优先级越高(0 是最高,**空闲线程占最大号**),刚好反过来。所以 FreeRTOS 里优先级 3 的任务,迁到 RT-Thread 不能照抄成 3,得用「档位上限减去原值」这类方式重新映射,或者干脆按新系统的语义重新设计一遍优先级表。第三,RT-Thread 的 `rt_thread_mdelay(ms)` / `rt_thread_delay(tick)` 对应 FreeRTOS 的 `vTaskDelay(pdMS_TO_TICKS(ms))`,延时是最常用的,记这个就够覆盖大半场景。优先级档位总数由 `rtconfig.h` 里的 `RT_THREAD_PRIORITY_MAX` 决定(常见 32 或 256),对应 FreeRTOS 的 `configMAX_PRIORITIES`。

## 消息队列:rt_mq 与 RT-Thread 独有的邮箱

这里 RT-Thread 给了你**两个**原语,而 FreeRTOS 只有一个 `xQueue`——这是两边差异最大的地方之一,值得展开。

FreeRTOS 的队列是一个「定长元素、按值拷贝」的通道:你 `xQueueCreate(length, itemSize)` 时定死每个元素多大,之后 `xQueueSend` 把你的数据**整块拷贝**进队列内部缓冲区、`xQueueReceive` 再拷贝出去。这个语义统一且唯一,不管你传的是个 int 还是个 64 字节的结构体。

RT-Thread 把这件事拆成了两半,你得按数据量选:

```c
/* FreeRTOS:一个队列搞定,定长按值拷贝 */
QueueHandle_t q = xQueueCreate(16, sizeof(sensor_sample_t));
xQueueSend(q, &sample, portMAX_DELAY);
xQueueReceive(q, &sample, portMAX_DELAY);

/* RT-Thread:数据大、要变长,用消息队列 rt_mq */
rt_mq_t mq = rt_mq_create("mq", sizeof(sensor_sample_t), 16, RT_IPC_FLAG_FIFO);
rt_mq_send(mq, &sample, sizeof(sensor_sample_t));
rt_mq_recv(mq, &sample, sizeof(sensor_sample_t), RT_WAITING_FOREVER);
```

`rt_mq`(`rt_msgqueue`)对应 FreeRTOS 的队列:定长缓冲、支持紧急发送(`rt_mq_urgent`,相当于插队)、能带超时阻塞,语义最接近。但要注意 RT-Thread 的消息队列底层实现和 FreeRTOS 不太一样——它维护的是一条消息链表,所以**单条消息最大尺寸有上限**(取决于 `rtconfig.h` 的 `RT_MQ_BUF_SIZE` 或动态创建时给的缓冲),而 FreeRTOS 队列的容量是你 `xQueueCreate` 时算好的 `length × itemSize` 那一整块。迁移时如果消息体很大,优先核对 RT-Thread 工程的队列缓冲配置。

然后是 FreeRTOS 完全没有的**邮箱 `rt_mb`**。邮箱是定长 4 字节(64 位系统上是 8 字节)的消息通道,典型的用法是传一个指针或一个 32 位整数,**零拷贝**——你把数据放别处,只把它的地址 `rt_mb_send(mb, (rt_uint32_t)ptr)` 丢进邮箱,接收方拿到地址再用。这东西在 FreeRTOS 里没有独立原语,你要么用队列传整个结构体(按值拷贝,有开销),要么干脆传指针(队列也支持传指针,但元素大小仍占一个槽)。所以迁移方向上:

- **FreeRTOS 队列 → RT-Thread**:默认映射到 `rt_mq`;如果队列元素其实只是一个指针/整数,可以改用 `rt_mb` 拿到零拷贝的好处。
- **RT-Thread 邮箱 → FreeRTOS**:用 `xQueueCreate(n, sizeof(void*))` 传指针来近似,但失去了邮箱「4 字节超轻」的确定性行为。

还有一个真坑:RT-Thread 邮箱的 `rt_mb_send` 在邮箱满时**默认非阻塞直接丢**(返回 `-RT_EFULL`),不像 FreeRTOS 队列的 `xQueueSend` 默认可以无限阻塞。如果你的代码依赖「满了就阻塞等消费者」的反压语义,迁移到邮箱时必须改成 `rt_mb_send_wait`(带超时的版本)或改用消息队列。

## 信号量:rt_sem 对 xSemaphore

信号量是两边最干净的一一对应,几乎可以逐符号替换。FreeRTOS 的 `xSemaphoreCreateBinary` / `xSemaphoreCreateCounting(max, initial)` 对应 RT-Thread 的 `rt_sem_create("name", initial_value, flag)`,二值就是初始值为 1 的信号量、计数就是初始值任意的信号量——RT-Thread 不像 FreeRTOS 那样把「二值」和「计数」做成两个独立的 create API,而是统一一个 `rt_sem`,用初始值和上限来表达。

```c
/* FreeRTOS */
SemaphoreHandle_t sem = xSemaphoreCreateCounting(5, 0);
xSemaphoreTake(sem, portMAX_DELAY);   /* P 操作,可阻塞 */
xSemaphoreGive(sem);                   /* V 操作 */

/* RT-Thread */
rt_sem_t sem = rt_sem_create("sem", 0, RT_IPC_FLAG_FIFO);
rt_sem_take(sem, RT_WAITING_FOREVER);  /* 对应 take */
rt_sem_release(sem);                    /* 对应 give */
```

`rt_sem_take` = `xSemaphoreTake`,`rt_sem_release` = `xSemaphoreGive`。`flag` 参数(`RT_IPC_FLAG_FIFO` 或 `RT_IPC_FLAG_PRIO`)控制当多个线程在等同一个信号量时谁来,前者按先来后到、后者按优先级——FreeRTOS 默认就是按优先级唤醒,所以想完全对齐 FreeRTOS 行为就用 `RT_IPC_FLAG_PRIO`。ISR 里的版本:FreeRTOS 是 `xSemaphoreGiveFromISR`,RT-Thread 是 `rt_sem_release` 不分 ISR 版——**RT-Thread 的 IPC API 大多线程与中断通用**(中断里调 `rt_sem_release` 会走 `rt_interrupt_enter/leave` 保护),这是和 FreeRTOS 「FromISR 后缀」哲学的一个显著不同,后面的互斥量、事件集同理。

## 互斥量:rt_mutex 对 xSemaphoreCreateMutex

互斥量也是近乎一一对应,而且**两边都内置优先级继承**——这是迁移时最让人放心的一点,你不用为优先级反转重新设计。FreeRTOS 的 `xSemaphoreCreateMutex()`(内部带优先级继承)对应 RT-Thread 的 `rt_mutex_create("name", flag)`,RT-Thread 的 mutex 同样在持有时会把持锁低优先级线程临时抬到等待者里最高优先级,机制一致。

```c
/* FreeRTOS */
SemaphoreHandle_t mtx = xSemaphoreCreateMutex();
xSemaphoreTake(mtx, portMAX_DELAY);
/* 临界区 */
xSemaphoreGive(mtx);

/* RT-Thread */
rt_mutex_t mtx = rt_mutex_create("mtx", RT_IPC_FLAG_PRIO);
rt_mutex_take(mtx, RT_WAITING_FOREVER);
/* 临界区 */
rt_mutex_release(mtx);
```

有一个细节差异值得点一下:**RT-Thread 的 mutex 支持「同线程重复 take」(递归语义由实现决定,需核对版本),而且会记录持锁线程**用于做优先级继承;FreeRTOS 的 mutex 不支持递归,要递归得用单独的 `xSemaphoreCreateRecursiveMutex`。另外 RT-Thread 的 mutex 有个 `RT_IPC_FLAG_PRIO` 的讲究更明显——因为 mutex 的存在就是为了优先级继承,挂起队列按优先级排几乎是默认正确选择。关于优先级反转和优先级继承的机制本身,我们已经在 [资源管理](../tutorial/08_resources/) 章里完整拆过,这里就不重复,迁移时只要确认 RT-Thread 这边默认开了优先级继承(默认就开)即可。

## 事件集:rt_event 对 xEventGroup

事件组在两边都是「一组二进制位,可等 AND/OR 条件」,但 API 形态和位宽上限有差异。FreeRTOS 的 `xEventGroupCreate` 给你一个 24 位(或 8 位,取决于 `configUSE_16_BIT_TICKS`)的事件组,用 `xEventGroupSetBits` 置位、`xEventGroupWaitBits(eg, bits, clearOnExit, waitAll, timeout)` 等待。RT-Thread 的事件集 `rt_event` 是 32 位的,用 `rt_event_send(ev, set)` 置位、`rt_event_recv(ev, set, option, timeout, &recvd)` 接收/等待。

```c
/* FreeRTOS */
EventGroupHandle_t eg = xEventGroupCreate();
xEventGroupSetBits(eg, (1<<0));
EventBits_t b = xEventGroupWaitBits(eg, (1<<0)|(1<<1), pdFALSE, pdTRUE, portMAX_DELAY);

/* RT-Thread */
rt_event_t ev = rt_event_create("ev", RT_IPC_FLAG_FIFO);
rt_event_send(ev, 0x01);
rt_uint32_t recvd;
rt_event_recv(ev, 0x01 | 0x02, RT_EVENT_FLAG_AND | RT_EVENT_FLAG_CLEAR,
              RT_WAITING_FOREVER, &recvd);
```

差异主要在「选项怎么表达」。FreeRTOS 用 `waitAll`(pdTRUE=AND,pdFALSE=OR)和 `clearOnExit`(pdTRUE=收到后清、pdFALSE=不清)两个布尔参数表达;RT-Thread 用一组 `option` 标志位:`RT_EVENT_FLAG_AND` / `RT_EVENT_FLAG_OR` 选逻辑,`RT_EVENT_FLAG_CLEAR` 选是否清除。**位宽**上 RT-Thread 是固定 32 位、FreeRTOS 默认 24 位(足够大多数场景),迁移时一般无碍。还有一个语义小差别:RT-Thread 的 `rt_event_recv` 可以**既等又返回当前哪些位是 1**(通过 `recvd` 出参),FreeRTOS 的 `xEventGroupWaitBits` 返回值也是退出时的事件位值,这点反而挺一致。事件组的机制和 AND/OR 用法在 [事件组](../tutorial/09_event-groups/) 章有完整演示。

## 软件定时器:rt_timer 对 xTimer

定时器两边都有,但**回调执行的上下文不一样**,这是迁移时必须重新理解的地方。FreeRTOS 的 `xTimer` 回调跑在一个专门的 Timer Service Task 上下文里(优先级由 `configTIMER_TASK_PRIORITY` 定),所以回调里**严禁阻塞**,阻塞会把整个定时器服务卡住,这点我们在 [软件定时器](../tutorial/06_timers/) 章强调过。RT-Thread 的 `rt_timer` 回调跑在**系统 tick 中断的下半部**(timer 软定时器线程或中断上下文,取决于配置),同样是**严禁阻塞、严禁做重活**——这一点两边精神一致,都是「定时器回调要快进快出」。

```c
/* FreeRTOS:回调在 Timer Service Task */
TimerHandle_t tmr = xTimerCreate("tmr", pdMS_TO_TICKS(1000), pdTRUE, NULL, cb);
xTimerStart(tmr, 0);

/* RT-Thread:周期定时器 */
rt_timer_t tmr = rt_timer_create("tmr", cb, RT_NULL,
                                 rt_tick_from_millisecond(1000),
                                 RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
rt_timer_start(tmr);
```

几个差异。第一,RT-Thread 的 `rt_timer_create` 把**周期/单次**做成了标志位(`RT_TIMER_FLAG_PERIODIC` vs 默认单次),而 FreeRTOS 是 `xTimerCreate` 的一个布尔参数 `uxAutoReload`。第二,RT-Thread 区分**硬定时器**(`RT_TIMER_FLAG_HARD_TIMER`,回调在 tick 中断里跑)和**软定时器**(`RT_TIMER_FLAG_SOFT_TIMER`,回调在软定时器线程跑)——这是个 FreeRTOS 没有的维度,迁移时一般选软定时器更安全(中断上下文跑回调限制更多)。第三,FreeRTOS 的定时器操作(`xTimerStart` 等)其实是往 Timer Service Task 发消息、异步执行,所以参数里有个「阻塞多久等命令入队」的 timeout;RT-Thread 的 `rt_timer_start` 是直接操作内核对象链表、同步完成,没有这个 timeout 概念,语义更直接。

## 内存管理:三种堆算法 + mempool 对 heap_1~5

这是两边设计哲学差异最大的一块。FreeRTOS 给你 `heap_1` 到 `heap_5` 五个互斥的堆实现,你在编译期选一个(本教程选 `heap_4`),它们共用 `pvPortMalloc`/`vPortFree` 这套接口、共用一个固定大小的 `ucHeap[configTOTAL_HEAP_SIZE]` 数组,差异只在内部怎么管这块数组。

RT-Thread 的内存管理是**分层**的,不是「五选一」,而是「动态堆算法三选一 + 可选的固定大小内存池」两套机制并存:

- **动态堆(`rt_malloc`/`rt_free`)**:底下挂三种算法之一,在 `rtconfig.h` 里二选一(互斥)——`RT_USING_SMALL_MEM`(小内存管理,适合 <1MB,实现简单)、`RT_USING_SLAB`(slab 分配器,适合大内存、多大小类)、`RT_USING_MEMHEAP_AS_HEAP`(`memheap`,把多块不连续内存拼成一个大堆,适合多 RAM 区域的芯片)。这大致对应 FreeRTOS 的 heap 选型思路,但 RT-Thread 的堆是**挂在一个由 `rt_system_heap_init(begin, end)` 指定的 RAM 区间上**,不像 FreeRTOS 是一个编译期数组。
- **内存池(`rt_mp_create`/`rt_mp_alloc`)**:这是 RT-Thread 的独立机制,启用 `RT_USING_MEMPOOL` 后可用——预先划一块连续内存成 N 个等长块,分配/释放都是 O(1) 且确定性强、无碎片。FreeRTOS **没有独立的 mempool 原语**,要做等长对象池你只能自己拿 `heap_4` 包一层或手写静态数组+链表。

所以映射关系是:

| FreeRTOS | RT-Thread | 说明 |
|----------|-----------|------|
| `heap_1`(只分配不释放) | (无直接对应,RT-Thread 堆都能 free) | RT-Thread 没有「只 malloc 不 free」的极简实现 |
| `heap_2`(无合并,best-fit) | `RT_USING_SMALL_MEM`(近似,best-fit 但会合并) | 语义最接近的是 small mem |
| `heap_3`(包标准 malloc) | `RT_USING_MEMHEAP_AS_HEAP` 或直接用 C 库 malloc | 都是把堆交给底层更大 allocator |
| `heap_4`(合并空闲块,首选) | `RT_USING_SMALL_MEM` / `RT_USING_SLAB` | small mem 适合小、slab 适合大,按 RAM 规模选 |
| `heap_5`(多不连续区) | `RT_USING_MEMHEAP_AS_HEAP` | 两者都是为多 RAM 区域设计 |
| (无) | `RT_USING_MEMPOOL` 内存池 | RT-Thread 独有,确定性强 |

迁移心智上的关键转变:FreeRTOS 的堆是「一个编译期数组 + 一种算法」,RT-Thread 的堆是「一个 RAM 区间 + 一种算法」,外加一个并行的 mempool 机制。具体到内存监控,FreeRTOS 有 `xPortGetFreeHeapSize` / `xPortGetMinimumEverFreeHeapSize`(本教程 [堆内存管理](../tutorial/03_memory/) 章用它们监控水位),RT-Thread 对应的是 `rt_memory_info()`(slab)或 small mem 的统计接口,且 mempool 有自己的 `rt_mp_info`——这块 API 名随算法变,迁移时核对 `rtconfig.h` 开了哪个再查对应接口。

## 任务通知:RT-Thread 无直接等价物

终于到这条最关键的差异。**FreeRTOS 的任务通知(Task Notification)在 RT-Thread 里没有直接对应的原语**,这是迁移时最容易卡住、也最需要重新设计的点。

任务通知的本质我们在 [任务通知](../tutorial/10_task-notifications/) 章里讲过:它是直接挂在每个任务 TCB 上的一个 32 位通知值 + 一个通知状态,**一对一、不走 IPC 对象容器**,所以比信号量/队列/事件组都更快更省(省掉整个对象的内存、省掉对象链表操作)。它的典型用法是「任务 A 直接通知任务 B」,API 是 `xTaskNotifyGive(target)` / `xTaskNotifyWait(...)` / `xTaskNotify(target, value, action)`。

RT-Thread 没有这种「直接挂在线程上」的轻量通知。你要实现等价语义,只能在它已有的 IPC 原语里挑一个绕一下:

- **最接近的是信号量**——给目标线程配一个专属信号量,`rt_sem_release` 对应 `xTaskNotifyGive`、`rt_sem_take` 对应 `xTaskNotifyWait`。语义上几乎一致,只是要多建一个信号量对象。
- **如果要用到那个 32 位值**(通知附带数据),改用**事件集**或**消息队列**,但开销都明显大于 FreeRTOS 的任务通知。

代价是实打实的:FreeRTOS 任务通知省掉的那块内存和那次对象链表操作,在 RT-Thread 里你必须用一个真正的 IPC 对象换回来,所以「一对一高频轻量通知」这个场景在 RT-Thread 上天然比 FreeRTOS 重。迁移时这是要承认的设计差异,不是调参能抹平的——**在 RT-Thread 项目里没有「任务通知」这个概念,统一用信号量/事件/队列表达同类需求**。

## 小结

把上面散在各原语里的差异收一下,有三条主线最值得带走。第一,**RT-Thread 的 IPC 原语一律 `create`/`init` 两条路、线程与中断通用一套 API**(没有 FromISR 后缀),这是和 FreeRTOS 最大的 API 形态差异。第二,**RT-Thread 在队列上多了个定长邮箱 `rt_mb`、在内存上多了个固定大小内存池**,这两个是 FreeRTOS 没有的概念,迁移方向反过来时要补。第三,**任务通知无对应物**,这是唯一一个不能平替、必须重新设计的原语。

记住这三条,大部分迁移决策都能当场拍板。但还有一个更宏观的层面我们没碰:两边的**调度模型**和**配置哲学**——FreeRTOS 的 `FreeRTOSConfig.h` 那堆 `config*` 宏,对应到 RT-Thread 是什么样的体系?`main` 在两个系统里到底是谁、什么时候才跑?这些是下一篇 [调度与配置哲学差异](./02-scheduling-and-config.md) 要拆的。
