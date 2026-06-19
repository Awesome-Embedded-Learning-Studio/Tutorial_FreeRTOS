---
title: 任务的概念与机制
description: 04 章概念篇——xTaskCreate 的六个参数逐个拆、抢占式优先级调度怎么挑任务、ready/running/blocked/suspended 状态机、vTaskDelete 收尾与 POSIX port 的回收延迟、configMINIMAL_STACK_SIZE 旋钮
---

# 任务的概念与机制

> 本章第一篇,讲概念。这一篇结束时你会理解任务是怎么造出来的、调度器凭什么挑谁先跑、任务状态怎么流转、怎么收尾、栈旋钮怎么定。下一篇 [独立 demo](./02-demo.md) 把这些概念跑成可见输出。

## 这章解决什么问题

上一章我们把堆内存摊开了,得到一个关键结论:**任务的栈和 TCB 都是从堆里 `pvPortMalloc` 出来的**。那一章像是把「舞台后台」搭好——有了堆这块地,任务才有地方住。这一章我们正式登台:动手 `xTaskCreate` 把任务造出来,给它们排好优先级,然后站在一旁看调度器在它们之间挑挑拣拣。

很多人第一次接触 RTOS 会卡在一个直觉上:我一个裸机程序,`main` 里 `while(1)` 跑得好好的,凭什么要拆成好几个任务?这个问题问得好,答案就藏在本章要做的事情里。裸机的 `while(1)` 是一条单线:你得自己用状态机把「采样、处理、显示」这三件事切成时间片轮着做,代码越写越像一团状态机面条。RTOS 的做法是把这三件事各自封装成一个**独立的、自己带主循环的任务**,然后让调度器在每个时刻自动挑出「此刻最该跑的那个」交给 CPU。你不用再手写时间片分配,只需要告诉调度器「谁更重要」(优先级),剩下的它来。这一章我们要做的,就是把这套机制亲手跑起来、看清楚调度器到底在怎么挑。

## xTaskCreate:一个任务是怎么被造出来的

创建任务就一个 API,签名长这样:

```c
BaseType_t xTaskCreate( TaskFunction_t pxTaskCode,
                        const char * const pcName,
                        const configSTACK_DEPTH_TYPE usStackDepth,
                        void * const pvParameters,
                        UBaseType_t uxPriority,
                        TaskHandle_t * const pxCreatedTask );
```

我们逐个参数拆。第一个 `pxTaskCode` 是任务函数指针——它必须是一个「永远不返回」的函数,典型长相是一个 `for( ;; )` 死循环。这和普通 C 函数是反过来的:普通函数跑完 `return` 就结束,任务函数如果真返回了,FreeRTOS 会认为这是错误,有的 port 会直接断言。所以任务函数的正确写法是无限循环,需要它停下的时候用 `vTaskDelete(NULL)` 主动销毁自己。

第二个 `pcName` 是给任务起个名字,主要给调试用——`vTaskList`、`uxTaskGetSystemState`、GDB 里都会显示它。它不是字符串指针常驻,内核会把这名字拷一份进 TCB(长度上限是 `configMAX_TASK_NAME_LEN`,模板里是 12),所以你给它起个一眼能认出来的短名字,比如 `"Sensor"`、`"Display"`,比 `t1`/`t2` 这种命名有用得多。

第三个 `usStackDepth` 是这个任务的栈深度,这是和上一章直接挂钩的参数。注意单位是**字(StackType_t)**,不是字节——`StackType_t` 在 64 位上是 8 字节,所以你传 `16384`,实际栈是 `16384 × 8 = 128KB`。任务运行时所有局部变量、函数调用栈帧、可能被中断打断时保存的上下文,全压在这块栈里。给小了会栈溢出,给大了浪费堆(栈是从堆出的,见上一章)。本章 demo 直接用模板的 `configMINIMAL_STACK_SIZE`,它的含义我们后面单独讲。

第四个 `pvParameters` 是传给任务函数的那个 `void *` 参数——任务函数签名是 `void taskFn( void *pvParameters )`,这个 `pvParameters` 就是你在这里塞进去的东西。一个任务函数常常要服务多个实例(比如三个 Worker 共用一份代码),靠的就是这个参数把各自不同的配置传进去。本章 demo 里三个 Worker 共用 `prvWorkerTask`,就是靠各自的 `WorkerConfig_t` 区分。

第五个 `uxPriority` 是优先级,数字越大越重要,这是本章后半段的主角。最后第六个 `pxCreatedTask` 是个输出参数,如果你以后想对这个任务做点什么(改优先级、挂起、删除),就把它填上,内核会把任务句柄写进去;不需要的话传 `NULL`。返回值是 `pdPASS`(成功)或 `pdFAIL`(失败,通常是堆不够,这又绕回上一章的 `malloc failed`)。

## 优先级:谁更重要,谁先跑

FreeRTOS 是**抢占式(preemptive)优先级调度**(模板里 `configUSE_PREEMPTION=1`)。它的工作方式可以用一句话概括:**在任何时刻,调度器都让「当前处于 ready 状态、且优先级最高的」那个任务跑**。这句话有三个关键词,我们一个个拆。

「处于 ready 状态」排除了两类任务:正在跑的那个(running,系统里永远只有一个 running)和那些暂时干不了活的(blocked、suspended)。一个 `vTaskDelay(100)` 的任务这 100 个 tick 内是 blocked,调度器根本不考虑它;它睡醒自动变回 ready,才有资格被选中。

「优先级最高」是说,只要有一个高优先级任务 ready,所有比它低的任务就**一个都跑不了**——不管低的排了多久、积压了多少活。这叫优先级抢占:高优先级任务一来,正在跑的低优先级任务会被立即打断(preempted),CPU 让给高优先级那个。这有个直接后果,也是新手最容易栽的坑:**如果你写了一个高优先级任务,它里面既不让出 CPU(`vTaskDelay`/阻塞 API 都不调)、也不被任何事件阻塞,那它会把所有低优先级任务活活饿死(starvation)**。本章 demo 里我们用 `vTaskDelay` 主动让出,正是为了避开这个坑;真正的死循环忙等会怎样,排错章会专门复现。

「让……跑」的另一半是**优先级相同时怎么办**。FreeRTOS 默认开时间片轮转(time-slicing):两个相同优先级的 ready 任务,每个 tick 中断都会给当前 running 的那个记一笔,轮到一定时候就切到另一个,大家均分 CPU。所以同优先级的任务是「轮流」跑的,不是「先到先得独占」。这个细节在本章 demo 里看不到(我们的三个 Worker 优先级都不同),但你要知道默认行为是轮转。

优先级取值范围是 `0` 到 `configMAX_PRIORITIES - 1`(模板里 `configMAX_PRIORITIES=7`,所以 0..6)。**优先级 0 是 idle 任务专属**,你自己创建的任务至少给 `tskIDLE_PRIORITY + 1`(也就是 1),否则会和 idle 任务抢同一档,行为不可预期。优先级数字越大越重要,但别一上来就全给最高——你把所有任务都设成 6,那就退化成同优先级轮转,优先级机制形同虚设。好的实践是:**按任务的「时序敏感度」分层**,真正时序敏感的(传感器采样、控制环路)给高优先级,对时序不敏感的(显示、日志、统计)给低优先级。

## 任务的状态机:ready / running / blocked / suspended

任务不是「生下来就在跑」,它在生命周期里会在几个状态之间来回切。理解这个状态机,是看懂调度器行为的关键。

`running` 是当前正占用 CPU 的那个任务,全系统永远只有一个。调度器的工作,本质上就是「决定下一个 running 是谁」。`ready` 是「万事俱备、就等 CPU」的任务——它没在等任何事件、没在睡觉,只要轮到它就能立刻跑。running 和 ready 之间是双向的:running 的任务被高优先级抢占,就掉回 ready;ready 的任务被选中,就升 running。

`blocked` 是本章最重要也最容易理解错的状态。一个任务调了 `vTaskDelay`、或者在一个空队列上 `xQueueReceive`、或者等一个没被释放的信号量——它就进入 blocked,而且**带一个超时**。blocked 的任务不参与调度器的挑选(它「干不了活」),直到超时到了、或者它等的事件发生了,才被唤醒回 ready。这里有个新手常有的误解:blocked 不是「浪费 CPU」,恰恰相反,**blocked 是好事**——一个 blocked 的任务根本不占 CPU,把 CPU 让给了别人。我们上一节说「高优先级任务不让出会饿死别人」,反过来,**善用 blocked(vTaskDelay、阻塞式 API)是多任务能并存的前提**。本章 demo 的每个 Worker 干完活就 `vTaskDelay`,就是在主动进 blocked、主动让出 CPU。

`suspended` 是一个「人工挂起」的状态:你调 `vTaskSuspend()` 把它挂起来,它就一直停着,直到有人 `vTaskResume()` 唤醒。它和 blocked 的区别是:**suspended 不带超时、也不等任何事件,只能被显式 resume**。这是个偏少用的状态,本章 demo 用不到,你先有个印象就行。

还有一个 `deleted`:任务被 `vTaskDelete` 销毁后,它就从这个状态机里彻底消失了。本章 demo 的任务都是无限循环、不删除的;但 `vTaskDelete` 我们必须会,因为它在「临时任务」场景里很常用——比如某个一次性初始化任务干完活就 `vTaskDelete(NULL)` 自我销毁,腾出资源。

## vTaskDelete:任务的收尾

`vTaskDelete( TaskHandle_t )` 把一个任务销毁掉。传一个具体句柄删别人,传 `NULL` 删自己(在任务函数里)。它干两件事:把这个任务的 TCB 从各种链表里摘掉、标记为已删除;然后归还它占的栈和 TCB 内存。

这里有个 POSIX port 独有的细节,值得提前知道。在真 MCU 上,`vTaskDelete` 通常立刻把内存还给堆;但在 POSIX port 这种宿主模拟环境下,内存归还要拖到 idle 任务里「懒回收」——因为要回收的内存可能正被当前 running 的任务用着,不能当场 free。所以你在 host 模拟下删除一个任务后,可能要等 idle 任务跑一轮,`xPortGetFreeHeapSize` 才反映出堆涨回来。这不算 bug,是模拟环境的实现选择;真硬件上行为更直接。这一点收在 [仿真坑点](../../pitfalls/) 的「vTaskDelete 资源回收差异」条目里,移到真板子时要留意。

还有个 `INCLUDE_vTaskDelete` 开关,模板里是 1(开了),所以这个 API 可用。如果你在极精简的系统里把它关成 0,链接器就把 `vTaskDelete` 整个剔掉,省点代码体积——但本章我们开着。

## configMINIMAL_STACK_SIZE:那个栈深度旋钮到底是什么

模板里 `configMINIMAL_STACK_SIZE` 被设成了 `PTHREAD_STACK_MIN`(16384),注释说这是「pthread_create 需要的最小栈」。这名字有点误导,容易让人以为「所有任务栈都得这么大」。其实不是。`configMINIMAL_STACK_SIZE` 在内核里有两个用途:一是给 idle 任务和 timer 任务当默认栈深(见 `app_hooks.c` 里 `vApplicationGetIdleTaskMemory` 就直接用了它),二是当你在应用里图省事、给任务传栈深时一个常用的「基准值」。

真正决定你某个任务栈多大的是你 `xTaskCreate` 时传的 `usStackDepth`——它可以是 `configMINIMAL_STACK_SIZE`、也可以是它的两倍、也可以是几百。在真 MCU 上,任务栈通常很小(几百到几千字),你会在调试阶段用 `uxTaskGetStackHighWaterMark` 探出每个任务实际用多少,再把 `usStackDepth` 收紧到略大于峰值,省 RAM。host 模拟下 `PTHREAD_STACK_MIN=16384` 是被 POSIX port 强制的最小值(再小 pthread 会拒),所以这里所有任务栈都「虚胖」到 128KB,这是上一章讲过的体量错觉,真板子上完全不是这个数量级。

---

概念讲透了,下一篇 [独立 demo:多优先级任务 + 状态快照](./02-demo.md) 我们把这套机制跑成可见输出。
