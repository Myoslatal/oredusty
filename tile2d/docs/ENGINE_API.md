# 引擎公开面：模组可以依赖哪些符号

模组是编好的机器码，它引用的宿主符号在**加载时**才解析。所以"哪些符号算公开的"不是文档礼貌，而是
一条要执行的规则：清单外的引用，加载时**拒绝并指名**（`engine.api` 是机器读的那份）。

## 一、这份清单是怎么来的（实测，不是估的）

`codetab dump mine.codetab` 给出游戏自己的表向宿主索取的**全部**符号：622 个全局未定义，其中

| 来源 | 个数 | 是什么 |
|---|---|---|
| `t2d::` | 42 | 框架服务：日志、.ecfg、随机数、相机、字体与文字、精灵批、动态库、路径 |
| `ore::` | 15 | 渲染与平台：Renderer、rhi、Window、Image、Application、InputServer |
| libstdc++ / libc / 异常机制 | 248 | 平台层，**不属于**我们的 API，模组随意用 |
| `mine::` | 92 | 同一张表内被别的翻译单元定义，合并时内部解析，不是对外需求 |

**游戏自己需要的就是 57 个**（去重后），这就是公开面的**下限**：游戏要用的，模组至少要能用。
一个模组通常只用其中一小部分——多数模组只碰日志、.ecfg、注册表与世界查询。

## 二、分层

| 层 | 模块 | 个数 | 政策 |
|---|---|---|---|
| **A 常用 + 稳定** | `t2d/core`（log、ecfg、rng、camera2d、executable、module…）、`t2d/text`（font、locale、line_box、language） | 25 | **冻结**：改动 = 显式决定 + 引擎版本号 +1 |
| **B 常用但会变** | `t2d/render`（SpriteBatch、TextRenderer）、`t2d/net`、`ore/{renderer,rhi,platform,app,debug}` | 32 | **提供，但会随渲染器/网络/加载机制改**；加载时照常校验，报告里标出依赖了 B 层 |
| **C 不提供** | `mine/app.h`（应用外壳）、`mine/sandbox.h`（开发工具）、各 `.cpp` 内部、模组宿主内部 | — | **拒绝**并指名符号 |
| **平台层** | libc、libstdc++、Vulkan、GLFW | — | 不是我们的 API；校验时**不要求**在清单里 |

分层依据是两条实测的交叉：**被本体重引用的次数**（registry 17、types 10、content_loader 10、log 8、
ecfg 7…）与**改动频率**（`ore/app/application.h` 5、`t2d/sim/tilemap.h` 4、render/net/rhi/platform 各 3）。
引用多且 churn ≤2 的进 A；引用不少但 churn 3–5 的进 B。

## 三、怎么执行（下一步的落地）

1. **编译期先报**：`codetab build|pack --api engine.api`（默认开）——模组引用到的表外符号逐个对照清单，
   越界即编译失败。早报比加载时报省事。
2. **加载期兜底**：启动器读 `engine.api`（随可执行文件走），对每个模块的表外未定义符号查表，
   不在清单 → **拒绝该模块**，消息："`ore::Renderer::begin_pass` 不属于 engine 0.1 的公开面"。
   已有的 `requires engine@0.1` 版本校验保留：版本是粗筛，符号是细筛。
3. **回归护栏**：`test_mine_table` 断言"游戏表只用到清单内的符号"——引擎改到公开面之外，测试立刻红。
4. **生成器**：`codetab api --headers <清单> -o engine.api`（按头文件声明公开面、按符号校验）。
   在生成器到位之前，`engine.api` 由上面的实测直接产出并进仓库，改动看得见。

## 四、还没定的（等实现时再收）

* C 层是"拒绝"还是"警告"：默认拒绝，另给 `--allow-internal` 供开发期自查。
* 模组**覆盖本体函数**（`mine::`）要不要也收进清单：现在任何强符号都能覆盖，靠合并报告兜底；
  建议先不收——覆盖是要的能力，报告已经看得见。
* 版本号怎么发：A 层不变 = 版本不变；A 层任何改动、或 B 层改动 → 小版本 +1。