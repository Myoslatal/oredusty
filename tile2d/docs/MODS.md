# 模组包：内容与逻辑放在本体之外

本体只做**核心逻辑**（tilemap、渲染、文字、传输、注册表、沙盒）。内容——定义与逻辑——通过**模组包**
从外部加载：一个目录、一份清单、若干内容文件，外加可选的 C++ 模块。

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

上图里 `structure #1..#3`、`machine #1..#2` 是本体内容，`structure #4..#5`、`machine #3..#6` 来自两个模组；
其中 `mod_tier_1_drill`…`mod_tier_3_drill` **不是写在数据里的**，是原生模组按自己的参数生成的。

---

## 1. 清单（mod.ecfg）

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

## 2. 加载顺序

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
