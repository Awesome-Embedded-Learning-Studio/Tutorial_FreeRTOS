# POSIX 轨 `posix_demo` 行动指南

> **用途**:在 WSL2 上 fork FreeRTOS 官方 `main_blinky`,用 **CMake** 跑通 `posix_demo`。
> 这是整个教程的代码地基——[02_environment](../document/tutorial/02_environment/) 章和后续章节的可运行代码都来源于此。
> 对应 [todo](../document/todo/directions/d1-code-skeleton) 里的 **P0-D1 第一项**。
>
> **预期产物**:
> ```
> Tutorial_FreeRTOS/
> ├── .gitmodules                       ← submodule 登记(third_party/FreeRTOS)
> ├── third_party/FreeRTOS/             ← submodule,钉 tag 202411.00
> │   └── FreeRTOS/
> │       ├── Source/                   ← 嵌套 submodule(FreeRTOS-Kernel),已 init
> │       └── Demo/Posix_GCC/           ← 上游 demo 源头
> └── code/
>     └── posix_demo/                   ← fork 出来的最小 demo
>         ├── main.c  main_blinky.c  console.c  console.h  FreeRTOSConfig.h
>         ├── CMakeLists.txt            ← 改过路径的
>         └── README.md
> ```

---

## ⚠️ 三个最容易踩的坑(先记住)

1. **tag 格式是 `YYYYMM.NN`,没有 `V` 前缀!** 用 `git tag -l 'V202*'` 会匹配空。本地可选 `202411.00`。
2. **`FreeRTOS/Source` 是嵌套 submodule**(指向 `FreeRTOS/FreeRTOS-Kernel`),默认是空的!必须单独 `git submodule update --init FreeRTOS/Source` 才有内核源码。
3. **绝不能 `git submodule update --init --recursive`!** 主仓库的 `.gitmodules` 里有 30+ 个 FreeRTOS-Plus 的无关子模块(TCP/AWS/mbedtls/wolfSSL/glib/libslirp…),recursive 会下载几个 GB。**只 init `FreeRTOS/Source` 这一个**。

---

## Step 0 · 前置确认

```bash
uname -m            # 应为 x86_64。⚠️ 若是 aarch64,POSIX port 会 segfault,换 x86 WSL
gcc --version       # 需要 11+。没有就 sudo apt install build-essential
cmake --version     # 需要 3.15+。没有就 sudo apt install cmake
git --version
```

---

## Step 1 · 引入 submodule(若已存在则跳到 1.2)

### 1.1 首次添加

```bash
cd /home/charliechen/Tutorial_FreeRTOS
git submodule add https://github.com/FreeRTOS/FreeRTOS.git third_party/FreeRTOS
```

### 1.2 钉 tag `202411.00`(关键!顺序不能错)

> ⚠️ **顺序铁律:先 checkout tag,再 init Source 嵌套 submodule。**
> 若反过来(先 init 再 checkout),Source 会被 init 到旧 HEAD 对应的内核,checkout 后父仓库记录的 gitlink 变了,会出现 `M FreeRTOS/Source`,需额外 `git submodule update FreeRTOS/Source` 重新同步。先 checkout 就没这麻烦。

```bash
cd third_party/FreeRTOS
git checkout 202411.00
# 注意:此刻还停在 third_party/FreeRTOS/ 里,Step 1.3 的命令据此调整
```

⚠️ **为什么钉 `202411.00` 而不是别的**:
- 本地可选 tag:`202011.00` ~ `202411.00`(`YYYYMM.NN` 格式)。最新就是 **`202411.00`**。
- **不要用 `202212.00`** —— 它太老:Posix_GCC demo 还没有 `CMakeLists.txt`、没有 `main.c`(USER_DEMO 切换),无法走本指南的 CMake 路线。
- **不要停在 master HEAD** —— 当前 `git submodule status` 显示 `202212.00-315-gf60e36e45` 就是飘的 master,不可复现。必须 checkout 到确定 tag。
- 当前 HEAD `f60e36e45` 之后的代码没用上,checkout 202411.00 后内容会变,正常。

验证钉死:
```bash
git submodule status third_party/FreeRTOS
# 期望:类似  <hash> third_party/FreeRTOS (202411.00)   ← 括号里是纯 tag 名,无 -315-gxxx 后缀
```

### 1.3 初始化嵌套的内核 submodule(关键!)

```bash
# 你现在应该在 third_party/FreeRTOS/ 里(Step 1.2 没 cd 出来)
git submodule update --init FreeRTOS/Source
# 若已在仓库根,改用: git -C third_party/FreeRTOS submodule update --init FreeRTOS/Source
```

⚠️ 只 init 这一个。**不要**加 `--recursive`。

验证内核源码落地(这两个文件必须存在):
```bash
ls third_party/FreeRTOS/FreeRTOS/Source/CMakeLists.txt    # 内核 CMake 支持
ls third_party/FreeRTOS/FreeRTOS/Source/tasks.c           # 内核源码
```
若 `CMakeLists.txt` 不存在:说明这个内核版本太老没 CMake 支持,确认你 checkout 的是 `202411.00` 而非更老的 tag。

---

## Step 2 · fork demo + 改 CMakeLists(核心)

### 2.1 复制最小文件集

```bash
SRC=third_party/FreeRTOS/FreeRTOS/Demo/Posix_GCC
cp $SRC/main.c $SRC/main_blinky.c $SRC/console.c $SRC/console.h \
   $SRC/FreeRTOSConfig.h $SRC/CMakeLists.txt code/posix_demo/
```

⚠️ **故意不复制** `main_full.c` / `run-time-stats-utils.c` / `code_coverage_additions.c` / `Trace_Recorder_Configuration/`——它们是 full demo / trace 专用,blinky 用不到。

### 2.2 改 CMakeLists 路径(最容易错的一步)

在 `code/posix_demo/CMakeLists.txt` 里改这两行(原值假设 demo 在 `Demo/Posix_GCC/`):

```cmake
# 原始
set( FREERTOS_KERNEL_PATH "../../Source" )
set( FREERTOS_PLUS_TRACE_PATH "../../../FreeRTOS-Plus/Source/FreeRTOS-Plus-Trace" )

# 改成(从 code/posix_demo/ 出发)
set( FREERTOS_KERNEL_PATH "../../third_party/FreeRTOS/FreeRTOS/Source" )
set( FREERTOS_PLUS_TRACE_PATH "../../third_party/FreeRTOS/FreeRTOS-Plus/Source/FreeRTOS-Trace" )
```

**路径算术自检**:`code/posix_demo/` →`code/`(1)→ 仓库根(2)→ `third_party/FreeRTOS/FreeRTOS/Source` ✓

> `FREERTOS_PLUS_TRACE_PATH` 在 `NO_TRACING=1` 时用不到,但留着以防误开 trace;若路径报错可整段注释掉 trace 相关行。

### 2.3 精简 `add_executable`(只留 blinky 源)

CMakeLists 里 `add_executable( posix_demo ... )` 列了一长串源。**删掉**这些 full demo 专用项,只保留 blinky 的:

```cmake
add_executable( posix_demo
                console.c
                main.c
                main_blinky.c
                # $<$<NOT:${NO_TRACING}>:${FREERTOS_PLUS_TRACE_SOURCES}>   ← 注释掉
                # ${CMAKE_CURRENT_LIST_DIR}/../Common/Minimal/*.c          ← 这 25 行全注释掉
              )
```

删掉:`main_full.c`、`run-time-stats-utils.c`、`code_coverage_additions.c`、以及整段 `${CMAKE_CURRENT_LIST_DIR}/../Common/Minimal/*.c`(约 25 行,full demo 测试用)。

> 背景机制:`main.c` 用 `#ifdef USER_DEMO` 切换 blinky/full,默认 `FULL_DEMO`。blinky 模式下 `main_full()` 不被调用,所以不复制 `main_full.c`、不编译 Minimal/*.c 是安全的。

---

## Step 3 · 工具链(Step 0 没装的话)

```bash
sudo apt-get install -y build-essential cmake gcc gdb
```

---

## Step 4 · CMake 构建

```bash
cd code/posix_demo
cmake -B build -DUSER_DEMO=BLINKY_DEMO -DNO_TRACING=1
cmake --build build
```

⚠️ **`-DUSER_DEMO=BLINKY_DEMO`** 必须 —— 否则 `main.c` 走 `FULL_DEMO` 分支找 `main_full()`(没复制)→ 链接失败。
⚠️ **`-DNO_TRACING=1`** 必须 —— 否则默认 `projENABLE_TRACING=1`,去拉 FreeRTOS-Plus-Trace(你没初始化那个 submodule)。

成功标志:产出 `build/posix_demo`,无报错。

---

## Step 5 · 运行验证

```bash
./build/posix_demo
```

**预期输出**:先打印 `Starting echo blinky demo`,然后两个任务交替工作(一个发字符、一个 echo 回来),console 持续滚动。**Ctrl-C 退出**(`main.c` 注册了 SIGINT handler)。

看到稳定滚动输出 = 跑通 ✅

---

## Step 6 · 收尾

1. **gitignore 构建产物**——在根 [.gitignore](../.gitignore) 加:
   ```
   code/posix_demo/build/
   ```
2. **写 `code/posix_demo/README.md`**,记录:上游 tag(`202411.00`)、构建命令(`cmake -B build -DUSER_DEMO=BLINKY_DEMO -DNO_TRACING=1 && cmake --build build`)、本机平台、预期输出、已知问题占位(ARM64 segfault、x86 前提——留给 P0-D1 第三项展开)。
3. **提醒 clone 者**——新环境 clone 后必须:
   ```bash
   git submodule update --init                              # 拉主 submodule
   git -C third_party/FreeRTOS submodule update --init FreeRTOS/Source   # 只拉内核,不 recursive
   ```
   写进根 [README.md](../README.md) 或 posix_demo/README。
4. **更新 [instractions.md](instractions.md)** 的「目录约定」,把 `posix_demo/` 从「规划中」标成「已落地」。

---

## ⚠️ 坑点速查表

| 坑 | 现象 | 对策 |
|----|------|------|
| tag 格式误用 V 前缀 | `git tag -l 'V202*'` 空 | 格式是 `YYYYMM.NN`,用 `202411.00` |
| 不钉 tag | submodule 停在 `202212.00-315-gxxx`(飘 HEAD) | `git checkout 202411.00` |
| 选了 202212.00 | Posix_GCC 没 CMakeLists / 没 main.c | 用 `202411.00` |
| Source 嵌套 submodule 空 | `Source/tasks.c: No such file` | Step 1.3 单独 init |
| 误用 `--recursive` | 下载几 GB 无关代码 | 只 `--init FreeRTOS/Source` |
| CMake 路径断 | `Cannot find source file: tasks.c` | 改 `FREERTOS_KERNEL_PATH`(2.2) |
| 忘 USER_DEMO | `undefined reference to 'main_full'` | `-DUSER_DEMO=BLINKY_DEMO` |
| 忘 NO_TRACING | 拉 FreeRTOS-Plus-Trace 失败 | `-DNO_TRACING=1` |
| Minimal/*.c 在 add_executable | 编译慢 / 符号冲突 | 删掉那 25 行(2.3) |
| ARM64 segfault | `./posix_demo` 直接崩 | POSIX port 仅 x86,换 x86 WSL |
| `-D_WINDOWS_` 名字吓人 | 以为编译成 Windows | 别慌,POSIX port 内部约定宏,保留 |
| GDB 调试总断在信号 | 频繁 SIGUSR1/SIG34 | GDB 里 `handle SIGUSR1 SIG34 nostop noprint` |

---

## 常见报错排错

- **`Source/tasks.c: No such file or directory`(cmake 阶段)** → 嵌套 submodule 没 init(Step 1.3),或 `FREERTOS_KERNEL_PATH` 没改对(2.2)。`ls third_party/FreeRTOS/FreeRTOS/Source/CMakeLists.txt` 验证。
- **`undefined reference to 'main_full'`(链接)** → 忘了 `-DUSER_DEMO=BLINKY_DEMO`,或没删 add_executable 里的 `main_full.c`。
- **`undefined reference to 'vStart...'`(链接)** → 你删了 Minimal/*.c 但 USER_DEMO 还是 FULL。两者要配套。
- **`trcRecorder.h: No such file`** → 忘了 `-DNO_TRACING=1`。
- **`fatal: unable to access github.com`** → 网络问题,确认能访问 github。clone 主仓库成功的话,init 嵌套 submodule 应该也行。
- **编译过但运行立即 segfault** → 先 `uname -m` 确认是 x86_64。

---

## 完成之后

P0-D1 第一项完成。接着可并行做(都不依赖本次代码改动):
- **P0-D1 第二项**:Windows MSVC 轨 `WIN32.sln`(需 Windows VS)
- **P0-D1 第三项**:POSIX port 已知问题标注 → 填 [02_environment](../document/tutorial/02_environment/) 与 [pitfalls](../document/pitfalls/)
- **P0-D2**:[00_roadmap](../document/tutorial/00_roadmap/) + [01_why-rtos](../document/tutorial/01_why-rtos/) 定位章
