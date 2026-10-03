# 外部内容：纯 ecfg 内容包与模组包

本体只做**核心逻辑**（tilemap、渲染、文字、传输、注册表、沙盒）。内容——定义与逻辑——从外部加载，
有两种形式，按需要选：

| 形式 | 是什么 | 能带代码吗 |
|---|---|---|
| **内容包**（纯 ecfg） | **一个 `.ecfg` 文件就是一个包**，可选一个 `pack::` 头说明自己是谁 | 不能 |
| **模组包** | 一个目录 + `mod.ecfg` 清单 + 若干内容文件 + 可选的共享库 | 能（C ABI，见 §3） |

大多数内容只需要第一种；需要"跑点什么"（按参数生成内容、将来挂 tick 钩子）时才用第二种。

加载顺序固定为：**本体内容 → 内容包 → 模组包**，三者进同一个注册表，id 按注册顺序分配。因此本体的 id
永远稳定，内容包只能在它后面追加，模组包再往后。

## 0. 纯 ecfg 内容包

    packs/
      01_base.ecfg            # pack:: 头可选
      02_extra.ecfg           # 没有头：文件名就是它的身份

    ./build/debug/games/mine/mine_game --world story --start 1 --packs packs
    ./build/debug/games/mine/mine_game --world sandbox --start 1 --pack packs/01_base.ecfg

![会话界面里的内容包](images/session_packs_en.png)

```
pack::                        # 可选。没有它，包的身份就是文件名（去掉扩展名）
    id:"base_pack"            # 唯一；其它包用这个名字 require
    name:"Base pack"          # 可选，给人看的
    version:"2.0"             # 可选
    requires:["other_pack"]   # 可选：先加载这个包（字符串或数组）
item::                        # 其余的表就是内容，和本体内容文件的写法完全一样
    <name>::
        <设计者的字段>
structure::
    <name>::
```

* `--packs <dir>` 把目录下**每个 `*.ecfg`** 当作一个包（按文件名排序）；`--pack <file>` 直接指定一个文件。
  两者都可重复。不写 `--packs` 时，如果工作目录下存在 `packs/`，它会被自动加载（缺省不存在不报错；
  显式给出的路径不存在则报错）。
* 顺序：先按文件名，再按 `requires` 调整——**只移动必须移动的**，所以按 01/02/03 命名的文件会保持
  你写的顺序。
* 同名内容、重复的包 id、依赖缺失或成环、语法错（带行列号）、`pack::` 里的未知键——全部**报告**，
  该包不加载或只加载不冲突的部分；`pack::` 表本身是元数据，不会被当成"不是内容类别的表"来报警告。
* **`image` 是引擎唯一会读的字段**（其余字段是你的）：内容条目写 `image:"art/wall.png"`，
  路径相对**本包目录**。加载时引擎校验文件存在、是 PNG（Ore 自带解码器），不存在/不是 PNG 都会报告；
  能解码的图片被装进一张图集，沙盒直接画出**你写的贴图**而不是占位颜色：

  ![内容包里的贴图](images/sandbox_pack_art_en.png)

  ```sh
  # 本体占位内容 + 仓库里的三个测试包（其中两个带 art/*.png），bands 填充：每项内容一条横带
  ./build/debug/games/mine/mine_game --world sandbox --start 1 \
      --content games/mine/tests/data/placeholder_content.ecfg \
      --packs games/mine/tests/packs --fill bands
  ```

  美术风格是多边形，资源仍是图片：引擎不做矢量多边形渲染（见 `docs/GAME_DESIGN.md` §1.10）。
* 内容包不能新增 `ContentKind`，也不能跑代码；要这两样就用下面的模组包。


    mods/
      example_data/             # 只有数据的模组：这是大多数模组的样子
        mod.ecfg
        content/structures.ecfg
      example_native/           # 带 C++ 模块的模组：用代码生成内容
        mod.ecfg
        native.cpp -> libexample_native.so

    ./build/debug/games/mine/mine_game --world sandbox --start 1 \
        --content <本体内容.ecfg> --mods mods

`--mods` 可重复。模组的内容与本体内容进入**同一个注册表**，id 按注册顺序分配，本体内容先注册。

![本体内容 + 两个模组](images/sandbox_mods_en.png)

```sh
# 上图（仓库里的两个示例模组，原生模块在构建目录里）：本体 5 项 + 模组 7 项 = 12 项，bands 填充
./build/debug/games/mine/mine_game --world sandbox --start 1 \
    --content games/mine/tests/data/placeholder_content.ecfg \
    --mods build/debug/games/mine/tests/mods --fill bands
```

上图里 `structure #1..#3`、`machine #1..#2` 是本体内容，`structure #4..#5`、`machine #3..#6` 来自两个模组；
其中 `mod_tier_1_drill`…`mod_tier_3_drill` **不是写在数据里的**，是原生模组按自己的参数生成的。

---

## 1. 模组清单（mod.ecfg）

```
id:"example_native"            # 必填。唯一，只能用字母/数字/_/-/.
name:"Example (native)"        # 可选，给人看的；缺省用 id
version:"1.0"                  # 可选
description:"..."              # 可选
api:1                          # 原生模组必填：它编译时用的模组 API 版本
requires:["example_data"]      # 可选：依赖的模组 id（字符串或数组）
content:["content/a.ecfg"]     # 可选：要注册的内容文件，按顺序
native:"libexample_native.so"  # 可选：相对包目录的动态库
data::                         # 可选：模组自己的参数，本体不解释
    tiers:3
    label:"generated"
```

规则（都是"读不懂就拒绝"，不是猜）：

* 清单里出现**未知的键**会被拒绝——拼错的键就是一个不按作者意图工作的模组。模组自己的参数放
  `data::` 表里，本体只负责解析并通过 `mod_value()/mod_int()` 原样交回。
* `id` 必填且唯一；两个包用同一个 id 会被报告，第二个不加载。
* `requires` 指向不存在的模组 → 报错且**不加载该模组**（缺依赖不能半跑）。
* 依赖成环 → 报错，环上的模组都不加载。
* `native` 必须配 `api`；文件不存在、不是动态库、导不出入口、ABI 版本不符、库自称的 id 与清单不符
  → 报告并跳过，游戏继续跑。
* 内容文件先**全部解析**再注册：第二个文件坏了，这个包一个名字都不会进去。

## 2. 模组包的加载顺序

1. 扫描每个 `--mods` 目录（目录本身是包，或目录下每个子目录是一个包），文件名排序保证确定性。
2. 校验清单、查重、检查依赖。
3. 按依赖**拓扑排序**（同一层内按目录顺序，确定性）。
4. 逐个加载：先数据（注册进注册表），再代码（打开库、校验、`on_load`）。
5. 最后一次性把沙盒里已放置的格子**按名字**重新定位，并重建面板。

名字冲突**不合并**：谁先注册谁保留，后来者被报告为错误，但它的其它内容照常加载。冲突可能来自
本体内容、更早的模组，或同一个模组的更早文件——三种都报。

## 3. 原生模组

原生模组就是一个共享库，导出唯一一个符号：

```c
MINE_MOD_EXPORT const mine::MineModDesc* mine_mod_entry();
```

它编译时只需要头文件（`games/mine/include/mine/mod_api.h` 及其引用的 `t2d/core/types.h`、
`mine/registry.h`），**不链接本体的任何静态库**：模组是独立编译单元，用谁的编译器、哪个标准库都行。

因此跨过这条线的只能是**纯数据与函数指针**：没有 `std::string`、没有异常、没有布局未写明的 C++ 对象。
头文件本身是 C++（模组就是 C++），但它声明的 ABI 是 C。

```c
struct MineModApi {
    u32 abi_version;      // = kModApiVersion
    u32 struct_size;      // 这个结构体多大，见下
    void (*log)(void* self, u32 level, const char* message);
    ContentId (*register_content)(void* self, u32 kind, const char* name);
    ContentId (*find_content)(void* self, u32 kind, const char* name);
    const char* (*content_name)(void* self, u32 kind, ContentId id);
    const char* (*mod_value)(void* self, const char* key);   // 读自己的 data::
    i64 (*mod_int)(void* self, const char* key, i64 fallback);
};

struct MineModDesc {
    u32 abi_version; u32 struct_size;
    const char* id; const char* name; const char* version;
    int  (*on_load)(const MineModApi* api, void* self);   // 0 = 接受
    void (*on_unload)(void* self);
};
```

* `self` 是宿主交给这个模组的上下文。所有"代表这个模组"的操作都带着它，所以宿主没有全局状态，
  也不怕将来同时存在多个宿主。
* `struct_size` 让 API 可以**只追加**地长大：新函数加在末尾，老模组按自己的大小读，永远不会越界。
  只有已有字段改变含义时才动 `kModApiVersion`，那时老模组会被明确拒绝，而不是用错布局调用。
* `on_load` 返回非 0 即拒绝运行，宿主随即关掉这个库、什么都不留。`on_unload` 在库被关闭**之前**调用，
  且保证此时宿主不在任何模组回调里（重载发生在帧之间）。
* 动态库**不是沙箱**：模组以游戏的权限运行，能做游戏能做的一切。ABI 校验管的是兼容性，不是安全。

一个最小原生模组（完整可运行版本见 `games/mine/tests/mods/example_native/native.cpp`）：

```cpp
#include <mine/mod_api.h>
#include <format>

static int on_load(const mine::MineModApi* api, void* self) {
    const i64 tiers = api->mod_int(self, "tiers", 1);          // 自己的 data:: 参数
    for (i64 tier = 1; tier <= tiers; ++tier) {
        const std::string name = std::format("mod_tier_{}_drill", tier);
        if (api->register_content(self, (u32)mine::ContentKind::Machine, name.c_str()) == 0) return 1;
    }
    return 0;
}

static const mine::MineModDesc g_desc{mine::kModApiVersion, sizeof(mine::MineModDesc),
                                      "example_native", "Example (native)", "1.0", &on_load, nullptr};

MINE_MOD_EXPORT const mine::MineModDesc* mine_mod_entry() { return &g_desc; }
```

## 4. 真机验证

仓库里带两个示例包（`games/mine/tests/mods/`，CMake 把它们组装进构建目录，和真实安装的布局一致），
`test_mod_package` 有 9 个用例 / 102 个断言覆盖它们，真机运行输出：

    mods: 2 of 2 loaded (1 native), 6 content registered, 0 error(s)
    sandbox: RELOAD: +0 -0   0 CELLS REMAPPED   0 LOST   MODS: 2 LOADED, 1 NATIVE, 6 CONTENT

覆盖到的失败路径（每一条都有断言）：清单缺 id、id 含非法字符、未知清单键、原生模组缺 `api`、
`requires` 指向不存在的模组、依赖成环、两个包同 id、两个模组注册同名内容、内容文件语法错（带行列号）、
内容表名拼错（警告而非错误）、原生库缺失、原生库不是库、ABI 版本不符、库自称 id 与清单不符、
`on_load` 返回非 0。以及：重载后 id 完全一致，不清理注册表而重复加载时全部报冲突。

## 5. 现在还不能做的（边界）

* **不能新增 `ContentKind`**：kind 列表是工程定义（`registry.h`），模组只能在既有 kind 里注册内容。
  设计者需要新类别时，那是本体的一处小改动。
* **没有 tick / 行为钩子**：游戏自己的模拟层还没写（M2），所以现在没有可以挂钩的地方。API 的追加式设计
  就是为它准备的：模拟出现后，`on_tick` 之类的函数加到 `MineModApi` 末尾即可。
* **没有补丁/覆盖机制**：同名内容报冲突而不是覆盖。做"修改别人的内容"需要明确的加载顺序与覆盖语义，
  那是后续的设计。
* **不隔离**：原生模组能崩溃游戏。调试工具、开发期加载，不要加载来路不明的包。
* **单线程**：加载与卸载都在帧之间；一个正在回调里的模组不会被卸载。
