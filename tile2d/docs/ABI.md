# 代码表模组的 ABI：不一致在哪里，怎么发现，怎么修

本文回答一个问题：**代码表把"编译好的 C++"搬进另一个程序，靠符号名解析——那么"名字一样"在两边真的是同一样东西吗？**

答案是不是。下面十二处不一致**全部实测过**（§2，每条都有复现方式），方案在 §3，按性价比排。

---

## 0. 结论（先看这段）

1. **今天检查的是名字，不是字节。** `engine.api` 是一张符号名清单（`ApiSurface::parse`，`code_table.h:122-136`），
   版本规则只管"差多远"（`api_verdict`，`code_table.cpp:176-183`）。名字对得上就放行——而 ABI 不一致恰恰
   都发生在"名字一样、东西不一样"的地方。
2. **十二处里有九处今天就能查**，因为数据已经在表里：节字节、符号大小（`st_size`）、重定位、
   `.gcc_except_table*`、`.note.gnu.property`；一处部分能（H2：两边都带 vtable 时能比大小）；
   一处完全不能（H4：类型布局与编译开关，表里根本没有这些信息）。后两处要表里多记两样东西——
   头文件哈希与编译开关——而元数据块本来就是 `key=value`（`code_table.cpp:571-602`），
   **加键不用动格式版本**，旧运行时忽略不认识的键。
3. **两条最要紧，都是"加载 0 error，然后崩"**：
   * 表里的 `try`/`catch` **是死的**（实测 `terminate`，exit 134）；活体在 `games/mine/src/mod_package.cpp:630-634`，
     触发者是模组自己的 `mod.ecfg`。
   * **vtable 槽位漂移直接段错误**（实测 SIGSEGV，exit 139）：类里多一个虚函数就够了。
4. **有一条不是"可能"而是"现在就是这样"**：本体表 Release 用 `-O3 -DNDEBUG` 且**异常开着**，
   而 `codetab build` 给模组用 `-O2` 且 `-fno-exceptions`。它是上面两条的放大器。

## 1. 今天检查什么，粒度有多粗

| 检查 | 在哪 | 粒度 | "名字一样、东西不一样"时 |
|---|---|---|---|
| 符号在不在公开面 | `engine.api` / `ApiSurface`（`code_table.h:122-136`） | 符号名 | **看不见** |
| 版本差多远 | `api_verdict`（`code_table.cpp:176-183`） | major.minor | 看不见 |
| 依赖在不在 | `requires` / `requirements_met`（`code_table.cpp:689-721`） | id@版本 | 看不见 |
| 重定位能不能填 | `relocate`（`code_table.cpp:906-1002`） | 地址 | 能解析就填，不问类型对不对 |
| 符号谁赢 | `resolve_symbols`（`code_table.cpp:819-904`） | 名字 | 弱符号先到先得，**不比较字节** |
| 机器对不对 | 表头 arch + ELF machine | x86-64 | — |

一句话：**名字级已经做完了，字节级还没开始。** 而字节级要的数据，大部分表里已经有了。

## 2. 十二处不一致

| # | 不一致 | 实测结果 | 今天能发现吗 | 严重度 |
|---|---|---|---|---|
| H1 | 表里 `try`/`catch` 不生效 | `terminate`，exit 134，0 error | 能（表里就有痕迹） | **高** |
| H2 | vtable 槽位漂移 | SIGSEGV，exit 139，0 error | 部分 | **高** |
| H3 | 内联函数：同名两行为 | 本体 17 / 模组 19；`noinline` 时两边都 17 | 能（跨表比字节） | 中高 |
| H4 | 没有 ABI 指纹（布局 + 开关） | 读错元素（103 → 5）；返回类型不进 mangled 名 | 不能 | **高** |
| H5 | `R_X86_64_32` 不做范围检查 | 存进去的 32 位值 ≠ 真实地址 | 能（一行） | 中 |
| H6 | 平台符号被部分拦截 | 表里 `sin`=42，宿主 `sin(0.5)`=0.479426 | 能（`dlsym` 已经查过） | 中 |
| H7 | `.init_array.NNNNN` 优先级被忽略 | 41（"D,A"），ELF 规则是 14（"A,D"） | 能（节名里有数字） | 低中 |
| H8 | 可见性（`st_other`）没读 | `-fvisibility=hidden` 的定义照样参与覆盖 | 能 | 低 |
| H9 | 跨模块 RTTI 靠字符串比较 | 两个 `typeid(Box)` 地址不同，`dynamic_cast` 仍成功 | — | 低（要写下来） |
| H10 | 节对齐 > 页大小会放错 | 本仓库最大 64，无实例 | 能 | 低 |
| H11 | `SHN_ABS` 符号被当未定义 | 模块被拒（响亮，不静默） | 能 | 低 |
| H12 | CET / `-fcf-protection` 不一致 | 本机不产 `endbr64`，但 48 字节 note 进了表 | 能 | 低 |

### H1 异常：表里的 `try`/`catch` 是死的

实测（夹具 5 行，用**本体表的开关** `-O3 -DNDEBUG`、异常开着编）：

```cpp
extern "C" long long probe() {
    try { throw 42; } catch (int value) { return value; }
    return 0;
}
```

    loaded: 1 module(s), 0 error(s)
    terminate called after throwing an instance of 'int'
    exit=134

连"抛出点和 catch 在同一个函数里"都不行。**活体**：`mod_package.cpp:630-634` 就是同一个形状——

```cpp
if (value->type() == t2d::EcfgType::String) {
    const std::string text(value->as_string());
    try {
        return std::stoll(text);                 // 抛 std::invalid_argument
    } catch (const std::exception&) {
        return fallback;
    }
}
```

同样的夹具实测：`terminate called after throwing an instance of 'std::invalid_argument' what(): stoll`，
exit 134。触发路径完全在模组作者手里：`mod_int` 是公开的模组 ABI（`include/mine/mod_api.h:48`），
值来自模组自己的 `mod.ecfg`（`mod_package.cpp:602` 的 `mod_value_of`）——**写一个字符串给一个整数键，
游戏就 abort**。

根因：`code_table.cpp:243` 丢掉 `.eh_frame`，而且是**精确名匹配**——`-ffunction-sections` 下编译器产的是
`.gcc_except_table.<函数>`，于是异常**表**留下来了、**帧描述**没留下。Release 表实测：
`.gcc_except_table.*` **138 节**、`.eh_frame` **0 节**、指向 `__cxa_begin_catch` 的重定位 **8 条**、
`_Unwind_Resume` **199 条**（Debug 表：397 / 0 / 10 / 301）。没有 FDE，unwinder 连抛出点所在那一帧的
personality 都找不到，handler 自然永远找不到。

### H2 vtable 槽位漂移 → 段错误

夹具：宿主按 `struct Shape { virtual ~Shape(); virtual int kind() const; };` 编（`kind` 得 25），
模组头文件是 `struct Shape { virtual ~Shape(); virtual int extra() const; virtual int kind() const; };`，
模组做的是 `shape->kind()`：

    loaded: 1 module(s), 0 error(s)
    exit=139          （段错误）

槽位号是编译期常量：模组认为 `kind` 在第 3 槽，宿主的 vtable 只有 3 个槽，读到表尾之外就调过去了。
合并只按符号名对齐（`code_table.cpp:849-862`），**vtable 的形状从来没被比较过**。

数据在哪：有 out-of-line key function 的类，vtable 是 weak 符号、`st_size` 就在表里
（`CodeTableSymbol::size`，`code_table.h:75`；`TABLES.md` §7 的 machine 夹具正是这种）。但**只通过 vptr 调用、
自己不实例化的模组，表里根本没有 vtable**（实测 `shape_mod.codetab`：`_ZTV`/`_ZTI` 命中 0 处），
所以"比大小"只能算顺手一条，挡不住这一例。完整答案在 §3 P2-3。

### H3 内联函数：一个进程里两个行为

同一份头文件里的 `inline int pick(int)`，模组那份从 `x*10+7` 改成 `x*10+9`：

* **被内联时**（`-O2` 的常态）：两张表都没有这个符号，合并后本体得 **17**、模组得 **19**——
  同一个函数在一个进程里两个行为，**0 error**。
* **强制不内联时**（`__attribute__((noinline))`）：两张表各带一份 8 字节 `.text._Z5pick2i`，
  合并后**两个模块都得 17**——模组那份成了死代码，同样没人报。

根因：弱符号先到先得（`code_table.cpp:830-834`），而 `from_objects` 只在**同一张表内**去重时比较字节
（`code_table.cpp:257-272`），**跨表从不比较**。模组作者无法从任何输出里看出自己改的那份有没有生效。

### H4 没有 ABI 指纹

* **布局**：宿主 `struct Sprite { int x; int y; };`，模组头文件 `{ int x; int y; int tint; };`——
  同一个类名、同一批 mangled 名。宿主写 `sprites[3].y = 103`，模组读同一数组的 `[3].y` 得 **5**，0 error。
* **签名**：**返回类型不进 mangled 名**——实测 `int f()` 与 `double f()` 都是 `_Z1fv`。
  所以"符号还在清单里"完全不能说明签名没变。
* **开关**：本体表 Release 实测 `-O3 -DNDEBUG`（`compile_commands.json`）且**异常开着**；
  `codetab build` 实测 `-O2` 且 `-fno-exceptions`（`tools/codetab/main.cpp:237-239`）。
  `_GLIBCXX_USE_CXX11_ABI`、`-fvisibility`、`-march`、`-fsanitize`、`-fno-rtti` 一个都没记录。

今天**发现不了**：表里没有这些信息。修法见 §3 P1-1 / P2-2。

### H5 `R_X86_64_32` 不做范围检查

`.long abi_target` 产生 type 10 重定位；加载后"存进去的 32 位值 == 真实地址"返回 **0**（不等），**0 error**。
`code_table.cpp:941-951` 里 type 10 与 11 共用一段，但范围检查只在 `Absolute32Signed`（11）时做。
本仓库的 C++ 不产这种重定位——实测 12 个 Release 表对象 + 10 个夹具 + 演示模组，可分配节里只有
`PLT32 / PC32 / 64 / REX_GOTPCRELX`（10 987 + 68 + 2 条）——但非 PIC 目标文件或手写汇编会产，
而且**失败是静默的**。

### H6 平台符号被部分拦截

一张表定义了 `sin`：表里的调用得 **42**，**宿主自己的 `std::sin(0.5)` = 0.479426**，
另一张从没定义 `sin` 的表也得 **42**。一个名字、一个进程、两种行为，0 error。

根因：表内定义优先于 `dlsym`（`code_table.cpp:882-895`），而平台库内部的调用永远走它自己的那一份。
对 `operator new`、`std::cout`、`typeinfo` 这类带状态的符号，这不是"行为差异"而是"坏掉"。

### H7 `.init_array.NNNNN` 的优先级被忽略

对象 A 里一个默认优先级的全局构造，对象 B 里一个 `__attribute__((constructor(101)))`：
ELF 规则是 101 先跑，实测是 **41（"D,A"）**——`run_initialisers()`（`code_table.cpp:1004-1019`）
按表的节顺序跑，不看节名后缀的数字。同一个对象内 GCC 恰好按 101→200→默认的顺序吐节，所以这条
平时看不出来（实测 124 = "A,B,D" 正确），**跨对象就错**。

顺带一个事实：本体自己的表今天**一个 `.init_array` 都没有**（Release/Debug 都是 0），
所以这条对本体是潜伏的，对模组是活的。

### H8 可见性没读

`object_file.cpp:202-222` 只读 type 与 binding，不读 `st_other`。两面后果：
`-fvisibility=hidden` 的定义照样参与覆盖（对模组作者是好消息，实测 `_ZTI3Box` 是 `WEAK HIDDEN`
而不是 local）；而 `protected` 可见性下编译器可以不经过 PLT，**覆盖会报告成功、实际不生效**。

### H9 跨模块 RTTI 靠字符串比较

同一次运行里宿主与模组的 `typeid(Box)` 地址不同（`94851159428280` vs `140516889927712`），
`dynamic_cast<Box*>` 仍然成功——libstdc++ 在指针不等时回退到名字比较。实测 `-fvisibility=hidden`
**不会**给类型名加 `*` 前缀（本机 GCC 16；前缀是内部链接类型的做法）。结论：今天能用，
但这是标准库实现细节在兜底；换标准库、换编译器要重新实测。

### H10 ~ H12（低）

* **对齐**：数据区从页边界开始（`code_table.cpp:773`），节内 `align_up`（797-812）——
  `align ≤ 4096` 一定对，`alignas(8192)` 会错位。实测 Release 表 986 节的 align 分布
  `{1:309, 2:92, 4:15, 8:91, 16:420, 32:33, 64:26}`，没有超过 4096 的。
* **`SHN_ABS`**：`object_file.cpp:214-220` 把绝对符号的值丢掉，当"未定义"处理 → 走 `dlsym` → 找不到 →
  **模块被拒**（响亮，不是静默）。留着值更好。
* **CET**：本机 GCC 不产 `endbr64`（`-fcf-protection` 默认关），但每个对象都带 48 字节
  `.note.gnu.property`，而且它**进了表**（Release 表 17 节，alloc）。宿主开、模组不开时，
  在强制 IBT 的机器上间接调用表内函数会 #CP。今天不是问题，发行版默认一改就是。

## 3. 方案

### P0（半天，几行到几十行；全是"别再静默"）

1. **异常：先别让它悄悄死。**
   `codetab build|pack` 对带异常机制的表（有 `.gcc_except_table*` 节，或引用
   `__cxa_begin_catch` / `__cxa_throw` / `_Unwind_Resume`）**拒绝**并说明"表里 `try`/`catch` 不会生效"。
   现在的 `--exceptions` 帮助文本说"抛出即终止"，实际比这更糟：**连 catch 都不生效**。
   同时把本体自己的那一处去掉：`mod_package.cpp:630-634` 换成 `std::from_chars`（不抛）。
   验收：`codetab dump mine.codetab | grep -c __cxa_begin_catch` → **0**。
   （真正的支持是 P2-1。）
2. **`R_X86_64_32` 范围检查**：与 `from_objects` 里已有的宽度检查同一处（`code_table.cpp:364-372`），
   值放不进 32 位就报错，不写。验收：H5 的夹具从"静默错值"变成加载报错、退出码 1。
3. **模块定义了宿主也导出的符号 → 警告**：`resolve_symbols()` 里 `dlsym` 那一段顺手记下来，
   报"这个模块替换了平台符号 X，平台自己的调用不会经过它"。验收：H6 夹具加载时打印 1 条警告。
4. **`.init_array.NNNNN` 按后缀数字排序**（`run_initialisers()`，几行）。验收：H7 夹具从 41 变 14。
5. **同名 vtable 符号、`st_size` 不一致 → 拒绝**：数据已经在表里，加一遍比较即可。
   验收：两边都带 vtable 的夹具（40 vs 48）被拒并指名。

### P1（本周；把"能查的"查全）

1. **头文件哈希**（这一条挡住的是 H2/H3/H4 那一整类）：
   * `codetab build` 加 `-MMD -MF`，把头文件清单与内容哈希写进元数据：
     `abi.headers=<总哈希>`、`abi.header.<相对路径>=<8 字节哈希>`。
     元数据是 `key=value`，**加键不动格式版本**，旧运行时忽略。
   * 本体表：本仓库是 Ninja（`build.ninja`，全树只有 10 个着色器 `.d`，没有 C++ 的 `.o.d`），
     所以要么给 `codetab pack` 传清单（`--headers`），要么在 CMake 里加 `-MMD`。
   * 合并时：两边都有 → 逐条比，不同就**拒绝**并列出不同的头文件；一边没有 → 警告一次
     "这张表没说它按哪些头文件编的"。
   * 为什么够狠：dev 包**就带着 `include/`**（53 个头文件、356 KB）——照它编，哈希必然一致；
     不一致=作者用了自己那份头文件，正是要拦的情况。代价：Release 表 +约 3 KB 元数据。
     误报方向是安全的（改注释也会不一致），给作者留 `--allow-header-drift`。
2. **跨表同名定义的字节比较**：两张表都定义同一个弱符号而字节或大小不同 → 报告。
   H3 的两种情形（被内联 / 没被内联）都能看见。
3. **编译开关指纹**：`abi.compiler=`、`abi.flags=`（`build` 知道自己传了什么；`pack` 从 `.comment`
   与 `-frecord-gcc-switches` 读，读不到写 `unknown`）、`abi.macros=`（`_GLIBCXX_USE_CXX11_ABI`、
   `NDEBUG`、`_GLIBCXX_ASSERTIONS`）、`abi.march=`、`abi.exceptions=`、`abi.rtti=`、`abi.sanitizers=`。
   不同的项：`_GLIBCXX_USE_CXX11_ABI` / `-march` / sanitizer / 异常与 RTTI 开关 → 拒绝；
   `-O` 级别、`NDEBUG` → 警告。
4. **把两边的开关变成有意的**：至少让 `codetab build` 的默认与本体的构建对齐（今天是一边 `-O3 -DNDEBUG`
   加异常、一边 `-O2` 关异常），或者在两边都写清楚为什么不同。

### P2（结构性；几天到一周）

1. **异常与展开真正支持**：把丢弃规则从精确名改成前缀（保留 `.eh_frame` 与 `.gcc_except_table*`），
   放置后 `__register_frame`（每模块一段），`__dso_handle` 已经给了。验收：H1 的两个夹具
   从 terminate 变成正常返回 42 / -1，且本体表里 `__cxa_begin_catch` 的重定位能真的工作。
2. **类型探针（`engine.abi`）**：`codetab abi --manifest engine.abi --include <dir>` 生成一个 TU，
   为清单里每个类型发一个**弱数据符号**（`sizeof`、`alignof`、逐字段 `offsetof`、枚举值）。
   本体与模组各带一份，合并时**同名弱符号比字节**——用的还是现有机制，不新增比较逻辑。
   清单手写（与 `engine.api` 同一性质：实测产出、diff 里看得见），生成器以后再写
   （`ENGINE_API.md` §四 已经记着这件事）。
3. **vtable 槽位指纹**：清单里为每个多态类写 `vtable <类> <槽位顺序的成员名>`；探针 TU 为每个类生成一个
   检查函数，比对 vptr 各槽指向的成员地址（不能比时退化成槽位数），本体构建时跑一次、结果进表。
   这一条是 H2 的完整答案。
4. **TLS**：现在明确拒绝（`code_table.cpp:236-240`），保持拒绝即可，不必做。

### P3（收尾）

1. 读 `st_other`：`protected` 的强定义 → 警告"这次覆盖可能不生效"。
2. `align > page` → 报错，而不是默默放错。
3. `SHN_ABS` 保留常量值。
4. CET 位进指纹。
5. 文档：`TABLES.md` §8、`MODS.md` §6 各加一行指向本文。

## 4. 不做的事（写在明处）

* **不按 mangled 名推签名**：返回类型不在里面（实测），推不出来；签名级检查只能靠探针或哈希。
* **不承诺混用兼容**：指纹只负责**发现**，不负责让 `-fno-rtti` / 别的 `_GLIBCXX` ABI / 别的 `-march`
  变得能用。
* **不自动生成 `engine.api`**：那是另一件事（`ENGINE_API.md` §四）。
* **表模组仍然不承诺热重载**：指针已经散进整个程序（`TABLES.md` §6）。

## 5. 复现（全部夹具都是"编一个目标文件 + `codetab pack` + 一个 20 行宿主"）

宿主是一个最小程序：`CodeImage image; image.declare_host("engine", "1.0");` 逐个 `image.add(load(表))`，
`load()` 后 `image.find(符号)` 再调用。链接 `build/release/src/libt2d_core.a` 与 `-ldl`，需要宿主导出符号时加
`-rdynamic`。夹具编译开关分两种，正是 §H4 说的那两种：

    # 本体表的开关（Release）
    -std=c++20 -O3 -DNDEBUG -fPIC -fsemantic-interposition -ffunction-sections -fdata-sections -fno-stack-protector
    # codetab build 给模组的开关
    -std=c++20 -O2 -fPIC -fsemantic-interposition -ffunction-sections -fdata-sections -fno-stack-protector -fno-exceptions

| 编号 | 夹具要点 | 命令 | 期望（今天） |
|---|---|---|---|
| H1 | `try { throw 42; } catch (int)` | `codetab pack` → 宿主调用 | `terminate`，exit 134 |
| H1b | `std::stoll("not a number")` 包在 catch 里 | 同上 | `terminate`，exit 134 |
| H2 | 模组头文件多一个虚函数 | 宿主造对象、模组调 `kind()` | SIGSEGV，exit 139 |
| H3 | 同一 `inline` 函数两个函数体 | 两张表合并，各调一次 | 17 / 19（内联）或 17 / 17（不内联） |
| H4 | `struct Sprite` 多一个字段 | 宿主造 10 个、模组读 `[3].y` | 103 → 5 |
| H5 | `asm(".long abi_target")` | `codetab pack` → 宿主比较 | 32 位值 ≠ 真实地址 |
| H6 | 表里定义 `sin` | 宿主自己调 `std::sin(0.5)` | 表 42 / 宿主 0.479426 |
| H7 | 两个对象各一个构造（101 与默认） | 合并后看顺序 | 41（"D,A"） |
| H9 | 宿主与模组各一份 `typeid(Box)` | 两边打印地址 | 地址不同、`dynamic_cast` 仍成功 |

每条修复的验收标准都写在 §3 对应条目里（命令 + 期望输出）。
