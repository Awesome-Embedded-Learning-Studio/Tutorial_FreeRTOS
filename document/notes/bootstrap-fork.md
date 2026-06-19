---
title: 开发日志 #1:从上游 fork 出最小工程
description: code/00-bootstrap 是怎么从官方 Demo/Posix_GCC 扒出来的、CMake 路径怎么爆改、砍了哪些 full demo 的尾巴,以及为什么要 fork 一份自己的
---

# 开发日志 #1:从上游 fork 出最小工程

> 这是 `code/00-bootstrap` 这个最小工程的来历记录。读者侧的「怎么把它跑起来」在 [环境搭建章](../tutorial/02_environment/) 已经讲清楚了,这里只记它当初是怎么从上游 fork 出来的、为什么这么改——给想贡献或重 fork 的人看的。

## 先说为什么非要 fork 一份自己的

最容易想到的做法其实是偷懒:让读者直接 clone 官方 FreeRTOS 仓库,`cd Demo/Posix_GCC` 然后照官方 README 跑。说实话一开始我也是这么想的,但稍微一推敲就发现这条路对本教程是堵死的,原因有三个。

第一,官方仓库太重。它的 `.gitmodules` 里挂了三十多个 FreeRTOS-Plus 的子模块——TCP、AWS、mbedtls、wolfSSL、glib、libslirp——一个 `--recursive` 就是几个 GB,而我们整个教程用不上其中任何一个。让读者为了一个 blinky demo 去拉这些东西,体验极差。

第二,官方 demo 没钉 tag。你 clone 下来拿到的是飘在 master HEAD 的版本,今天能跑、下个月上游一改可能就跑不了。一个教程必须保证「半年后 clone 下来还能复现」,所以版本必须钉死。

第三,官方那个目录噪音太多。`Demo/Posix_GCC` 里既有我们要的 `main_blinky`(简单 demo),也有 `main_full`(一整套压力测试),还有 trace 配置、run-time-stats 工具、code coverage 附件、二十多个 Minimal 测试源——一股脑塞给一个初学者,他根本分不清哪个是 blinky 要的、哪个是 full demo 的。

所以正确的做法是:fork 出一个最小、钉死版本、clone 即跑的工程,放进我们自己的仓库。这就是 `code/00-bootstrap` 的来历。

## 扒文件:只挑 blinky 要的那几个

fork 的第一步是决定「搬哪些文件过来」。原则就一条:blinky 跑起来需要什么,就搬什么,多一个都不要。最后挑了这六个:

```bash
SRC=third_party/FreeRTOS/FreeRTOS/Demo/Posix_GCC
cp $SRC/main.c $SRC/main_blinky.c $SRC/console.c $SRC/console.h \
   $SRC/FreeRTOSConfig.h $SRC/CMakeLists.txt code/00-bootstrap/
```

`main.c` 是入口(含 SIGINT 处理和 demo 选择),`main_blinky.c` 是 blinky 的两个任务一个定时器,`console.c`/`console.h` 是带互斥的 printf 封装,`FreeRTOSConfig.h` 是内核配置,`CMakeLists.txt` 是构建脚本——刚好够 blinky 跑起来。

故意不搬的有一串,每一个都是 full demo 或 trace 专用的:`main_full.c`(压力测试主体)、`run-time-stats-utils.c`(运行时统计)、`code_coverage_additions.c`(覆盖率)、整个 `Trace_Recorder_Configuration/` 目录(trace 配置)。blinky 模式下这些东西根本不会被调用,带进来只会让编译变慢、还可能冒符号冲突。这一步的关键是克制——「最小可跑」不是口号,是搬文件时就得守的纪律。

## CMake 路径爆改:整个 fork 最容易翻车的一步

搬过来的 `CMakeLists.txt` 有个隐含假设:它觉得自己就住在 `Demo/Posix_GCC/` 里,所以内核路径是相对自己算的。现在它搬到了 `code/00-bootstrap/`,视角变了,这一行不改构建直接挂:

```cmake
# 原始(假设自己在 Demo/Posix_GCC/)
set( FREERTOS_KERNEL_PATH "../../Source" )

# 改成(从 code/00-bootstrap/ 出发)
set( FREERTOS_KERNEL_PATH "../../third_party/FreeRTOS/FreeRTOS/Source" )
```

这个路径算术我每次都要掰着手指头数:从 `code/00-bootstrap/` 出发,`..` 到 `code/`(一级),再 `..` 到仓库根(两级),然后接上 `third_party/FreeRTOS/FreeRTOS/Source`。它之所以是最容易翻车的一步,是因为 `add_subdirectory` 用的就是这个路径,一旦算错,cmake 阶段抛的是 `Cannot find source file: tasks.c`——报错信息根本不告诉你「路径错了」,只冷冰冰说找不到文件,你得自己反应过来是路径算术的问题。我建议后来人改完先 `ls` 一下解析出来的路径在不在,比看报错猜快。

## 砍 add_executable:把 full demo 的尾巴切干净

改完路径还没完,`CMakeLists.txt` 里 `add_executable( posix_demo ... )` 默认还挂着一长串 full demo 的源,得接着砍。要砍的有两坨:一坨是 trace 相关的源(我们关了 trace,用不上),另一坨是从 `${CMAKE_CURRENT_LIST_DIR}/../Common/Minimal/` glob 出来的约 25 个 Minimal 测试文件。砍完只留三个核心源:

```cmake
add_executable( posix_demo
                console.c
                main.c
                main_blinky.c
              )
```

这里有个配套关系必须想明白,不然砍了会炸。`main.c` 的逻辑是:编译时如果定义了 `USER_DEMO` 宏就用它选 demo,没定义就默认 `FULL_DEMO` 去调 `main_full()`。我们既然把 `main_full.c` 连同 Minimal 测试都砍了,就必须在构建命令里把 `USER_DEMO` 钉成 `BLINKY_DEMO`,让 `main.c` 走 blinky 分支、不去碰那个已经不存在的 `main_full`。砍源和加开关是一对操作,少一个链接器就拿 `undefined reference to 'main_full'` 糊你脸上。这也是为什么教程里反复强调 `-DUSER_DEMO=BLINKY_DEMO` 是命门开关——它不是可选优化,而是和「我们编了哪些源」绑死的硬约束。

## 小结:fork 的三条原则

回头看,fork 这一下其实就守了三条原则,后来人想重 fork 或 fork 别的 demo 都能套用:**最小**——只搬目标 demo 跑起来必需的文件,full/trace/测试一律不带;**钉版本**——上游 submodule checkout 到确定 tag(`202411.00`),不留飘的 HEAD;**可复现**——路径算清楚、源和构建开关配套,clone 下来照着命令就能跑。这三条做到位,一个小而干净的 host 工程就立住了,后面所有章节的示例代码都有了落脚的地方。
