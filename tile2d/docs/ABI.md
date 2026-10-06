# 代码表模组的 ABI：不一致在哪里，怎么发现，怎么修

本文回答一个问题：**代码表把"编译好的 C++"搬进另一个程序，靠符号名解析——那么"名字一样"在两边真的是同一样东西吗？**

答案是不是。下面十二处不一致**全部实测过**（§2，每条都有复现方式），方案在 §3，按性价比排。

---

## 0. 结论（先看这段）

**两条路线，都已经落地**（§1.5）：**构建时把 ABI 统一**，以及**构建方式不同的表完全共同加载**——
只要差异不影响类型。没有中间态：指纹不同就拒绝并指名，不做"受限加载"。

1. **构建时统一 ABI：工具链就是 ABI。** `codetab build` 用一套规范开关驱动系统编译器，并**问编译器**这个构建的
   ABI 是什么（一个生成的探针 TU 打印宏、`sizeof`/`alignof`、CPU 特性），把用到的头文件按内容哈希，
   把结果写进表的元数据。**本体自己的表也走同一扇门**（`games/mine/CMakeLists.txt` 用 `codetab build`），
   启动器自己那份记录是 `engine.abi`，和 `engine.api` 一起随可执行文件走。于是"不同 ABI"在实践中不会出现。
2. **指纹刻意做窄**，只覆盖真正决定布局的东西：C++ 库 ABI 宏、共享类型的大小与对齐、头文件内容、CPU 特性。
   实测 `-O0` / `-O2` / `-O3 -DNDEBUG` 三种构建**指纹完全相同**，合并后就是一个程序，差异只在日志里说一句。
   `-D_GLIBCXX_USE_CXX11_ABI=0` 指纹不同：同一批符号名要对应两种布局，没有任何加载器能办到——**拒绝**。
3. **拒绝时指名到事实**：`_GLIBCXX_USE_CXX11_ABI: 0 here, 1 there`、`sizeof(std::string): 8 here, 32 there`、
   `header t2d/core/log.h differs`、`it was compiled for 'avx512f' and this machine does not have it`。
4. **十二处不一致里这一轮修掉三处**：H1（异常：构建期拒绝 + 本体那处活体）、H4 的主体（指纹与头哈希）、
   以及新发现的 H13（销毁映像会把析构函数要跑的代码解除映射，实测退出时段错误）。其余按 §3 的状态表推进。
   两条历史结论不变：**表里的 `try`/`catch` 是死的**、**vtable 槽位漂移会段错误**。

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


## 1.5 两条路线：构建时统一，或完全共同加载（已落地）

**为什么没有"受限运行"这个中间态。** 一个类型与本体不同的模块，调用本体时用的就是错的布局——它不是"权限少一点"，
而是"算出来的地址是错的"。把它加载成"只能调 C 符号"看着温和，实际是给作者一个能跑但会错的模块，比拒绝更糟。
所以规则只有两条：**指纹相同 → 完全合并；指纹不同 → 拒绝并指名**。而"指纹相同"的判据被刻意做窄，只覆盖真正
决定布局的东西，这样"构建方式不同"不会被误伤——这正是"不同 ABI 的表能够共同加载"落地的方式。

| 构建差异 | 判据 | 结果 | 实测 |
|---|---|---|---|
| `-O0` / `-O2` / `-O3 -DNDEBUG` | 指纹相同 | **完全合并**，差异写进日志 | 当时实测三次构建 id 都是 `f3ca0b9119e3c2cb`（id 随头文件内容变，现在是 `bdcb9b070ab30e78`）；日志 `__OPTIMIZE__: 0 here, 1 there (does not change a layout)` |
| `-fno-exceptions` / `-fno-rtti` | 指纹相同 | **完全合并**（能力差异，报出来） | `__EXCEPTIONS: 1 here, 0 there` |
| `-DNDEBUG`、别的 `-D` | 指纹相同 | **完全合并** | `NDEBUG: 1 here, 0 there` |
| 另一个 `std::string` ABI | 指纹**不同** | **拒绝**，指名事实 | `_GLIBCXX_USE_CXX11_ABI: 0 here, 1 there`、`sizeof(std::string): 8 here, 32 there` |
| 引擎头文件改了一行 | 指纹相同、**头哈希不同** | **拒绝**，指名文件 | `header greeting.h differs` |
| 模块要 `avx512f`，机器没有 | **对照本机 CPU** | **拒绝** | `it was compiled for 'avx512f' and this machine does not have it` |
| 表里没说自己的 ABI | — | 加载，**警告一次** | `module 'mod' does not say what ABI it was built as` |

怎么做到的（`codetab` + `code_table.h` 的 `CodeAbi`）：

* **探针**：工具生成一个 TU，用**与模块相同的** `-std`/`-O`/`-I`/`-D` 编译并运行，逐行打印事实——
  `_GLIBCXX_USE_CXX11_ABI`、`_GLIBCXX_DEBUG`、`__GXX_ABI_VERSION`、`__cplusplus`、`__SIZEOF_*`、`__CHAR_BIT__`、
  `__STDCPP_DEFAULT_NEW_ALIGNMENT__`，以及 `sizeof`/`alignof`：`std::string`、`std::string_view`、`std::vector<int>`、
  `std::map`、`std::unordered_map`、`shared_ptr`、`unique_ptr`、`std::function`、`std::filesystem::path`、
  `optional`、`variant`。实测 `-D_GLIBCXX_USE_CXX11_ABI=0` 下 `string` 8 字节 / `path` 16 字节，默认 32 / 40。
* **事实分三类**：必须一致的（布局）、可以不同的（`NDEBUG`、`__EXCEPTIONS`、`__GXX_RTTI`、`__OPTIMIZE__`、
  `_GLIBCXX_ASSERTIONS`、sanitizer、构建传的 `-D`）、机器特性（`__AVX2__` → `avx2`，**对照本机 CPU**）。
* **指纹** = 必须一致的事实（排序后）的 FNV-1a 64。头文件按**内容**哈希、按**相对路径**记录：源树与 dev 包的路径不同、
  内容相同，只有相对路径对得上；系统头文件不记——C++ 库的 ABI 是事实，不是文件。
* **记录怎么走**：表的元数据块（`key=value`）里加 `abi=` 与 `abi.require.*` / `abi.allow.*` / `abi.header.*` /
  `abi.cpu`。**加键不动格式版本**，旧运行时忽略不认识的键（`code_table.cpp:571-602`）。
* **加载**：参考是 `engine.abi`（启动器自己的记录，`CodeImage::declare_host_abi`）；没有它时用第一个带记录的表。
  指纹不同或共享头文件不同 → 该模块**整体拒绝**（与重定位失败同一条路径：它定义的符号不进符号表）；
  CPU 特性缺失 → 拒绝；没记录 → 警告后加载（不因为"没说"就拒绝一个可能好好的表）。
* **构建期也拦一次**：`codetab build|pack` 拒绝带异常机制的表（`.gcc_except_table*`），
  `--exceptions` 是作者说"我知道"，那时只提示一行。

实测（本机，debug 与 release 两个预设）：`engine.abi` 与 `mine.codetab` 的指纹都是 `bdcb9b070ab30e78`，
演示模组也是；启动日志只有一行 `table: module 'mine': __EXCEPTIONS: 0 here, 1 there (does not change a layout)`，
随后 `tables: 1 module(s), 17780 symbol(s), 23086 relocation(s), 0 override(s), 0 error(s)`（debug）。
这一行「只有一行」是量出来的：H3 的两函数体报告曾经让这张表每次启动刷 36 行，根因与修法见 §2 H3。

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

**已修，而且展开现在真的能用**。三件事：本体那处 `std::stoll` 换成 `std::from_chars`（不抛）、本体表改由工具链编
（`-fno-exceptions`）——Release 表里 `.gcc_except_table*` 与 `__cxa_begin_catch` / `_Unwind_Resume` 都是 **0**；
表开始**带着自己的帧描述走**（`.eh_frame` 不再被丢），运行时在重定位之后、跑构造函数之前注册给 unwinder。

注册这一步有两个坑，都是实测踩出来的：`.eh_frame` 是**一张以零长度项结尾的表**，而目标文件里没有这个结尾——
链接器合并时补，运行时不补就会让 unwinder 走过节尾读下一节（实测：libgcc 里段错误）；注册要交**整节**，
交单个 FDE 一样越界。补上 4 个零字节之后，夹具里 `throw 7` 被表自己的 `catch` 接住（返回 7），
也被**加载它的程序**接住（unwind 穿过表的帧）——正是修复前 exit 134 的那个用例。

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

**已修（两边都带 vtable 时）**：合并现在比较同名 vtable 的**槽位数**与**每一槽指向的符号名**，不一致就拒绝整个模块并指名
（`the vtable for '_ZTV…' has N entries here and M in …` / `slot 3 is 'x' here and 'y' there`）。夹具实测两条都被拒。
挡不住的那一半——模组只通过 vptr 调用、自己不实例化（表里没有 vtable）——由**头文件哈希**兜住：那种情形必然是头文件不同。

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

**已修，而且现在在构建期就定了**（三件事）：

* **构建期折叠**：`codetab build` 在打包之前先做一次**部分链接**（`<compiler> -r`）——一次构建是**一个程序**，
  链接器留下先到的那份、所有引用都指向它。本体表里 9 个函数的 **55 份死拷贝**因此不再进表（845 → **686 节**，
  2634 → **2190 符号**，1.13 → 1.07 MiB），启动日志里那一页报告随之消失；同一场景新旧两张表渲染出来
  **逐像素相同**（`compare -metric AE` = 0）。回归用例 `the_toolchain_links_a_build_before_it_packs_it`：两个
  故意写得不一样的 `twin_width` 被链接成**一个**函数（两份地址相等），加载 **0 条警告**。
* **加载期报告**：真正被合并的同名弱定义（`codetab pack` 把别人编好的目标文件放在一起，或者跨模块）仍然比较
  **字节**（大小没记录时用整节），不同就报告 `… both define these and the two bodies differ …`，按"哪两份"
  合成一行并数出条数。夹具 `twin_small`/`twin_big` 实测报警——那正是模组作者问「我改的那份生效没有」时
  唯一的答案。
* **`R_X86_64_NONE` 是「没有重定位」**：部分链接在它丢掉补丁的地方留下这种条目（本体表 **76** 条），它指的是
  0 号空符号，谁都不定义。运行时**靠不应用来应用它**（不解析符号、不写字节），而不是把一张完全正确的表拒掉
  ——回归用例 `a_relocation_that_writes_nothing_is_applied_by_being_skipped`。

根因：弱符号先到先得，而 `from_objects` 只在**同一张表内**按字节去重，**跨表从不比较**——模组作者无法从任何
输出里看出自己改的那份有没有生效。今天工具链编出来的表里，同名弱定义**只有一份**（构建期就定了），所以加载期
要比较的只剩"别人编好的那些"。

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

**已修**：两种形式都做范围检查（无符号的那种原来没有，而表由 mmap 放置、地址永远放不进 32 位，所以那是静默写坏指针）。
夹具 `abs32.cpp`（一行内联汇编）实测：加载报 `does not fit the 32 bit field`，模块被拒。

`.long abi_target` 产生 type 10 重定位；加载后"存进去的 32 位值 == 真实地址"返回 **0**（不等），**0 error**。
`code_table.cpp:941-951` 里 type 10 与 11 共用一段，但范围检查只在 `Absolute32Signed`（11）时做。
本仓库的 C++ 不产这种重定位——实测 12 个 Release 表对象 + 10 个夹具 + 演示模组，可分配节里只有
`PLT32 / PC32 / 64 / REX_GOTPCRELX`（10 987 + 68 + 2 条）——但非 PIC 目标文件或手写汇编会产，
而且**失败是静默的**。（表里现在还有一种：`R_X86_64_NONE`，部分链接丢掉补丁时留下的，本体表 76 条——
它不写字节，运行时跳过，见 H3。）

### H6 平台符号被部分拦截

一张表定义了 `sin`：表里的调用得 **42**，**宿主自己的 `std::sin(0.5)` = 0.479426**，
另一张从没定义 `sin` 的表也得 **42**。一个名字、一个进程、两种行为，0 error。

根因：表内定义优先于 `dlsym`（`code_table.cpp:882-895`），而平台库内部的调用永远走它自己的那一份。
对 `operator new`、`std::cout`、`typeinfo` 这类带状态的符号，这不是"行为差异"而是"坏掉"。

**已修（说出来了）**：模块的**强定义**顶掉一个宿主进程也导出的名字时，加载报告一句
`a module defines 'x', which the running program provides too…`（弱定义不算：那是每个模块都有的 libstdc++ 内联副本）。
夹具 `host_name.cpp` 实测：表里的调用得 7，宿主自己的调用仍是 1。

### H7 `.init_array.NNNNN` 的优先级被忽略

对象 A 里一个默认优先级的全局构造，对象 B 里一个 `__attribute__((constructor(101)))`：
ELF 规则是 101 先跑，实测是 **41（"D,A"）**——`run_initialisers()`（`code_table.cpp:1004-1019`）
按表的节顺序跑，不看节名后缀的数字。同一个对象内 GCC 恰好按 101→200→默认的顺序吐节，所以这条
平时看不出来（实测 124 = "A,B,D" 正确），**跨对象就错**。

**已修**：`run_initialisers()` 按节名后缀的数字排序（没有后缀的算 65535，即最后），stable，跨对象也对。
夹具 `ctor_default.cpp` + `ctor_priority.cpp`（默认优先级的对象排在前面）实测：**41（"D,A"）→ 14（"A,D"）**。

顺带一个事实：本体自己的表今天**一个 `.init_array` 都没有**（Release/Debug 都是 0），
所以这条对本体是潜伏的，对模组是活的。

### H8 可见性没读

**已修（读进来了，也说了）**：`st_other` 现在读进表（打包在 binding 那个字的空位上，格式版本不用动），
**protected** 的强定义赢下合并时会报告一句"直接绑定到它的调用不经过合并，这次覆盖可能到不了每个调用点"。
夹具 `protected_mod.cpp` 实测报警。

**更正一处早先的实测**：`__attribute__((visibility("protected")))` 写在定义前面，GCC 在这里**忽略它**
（`-Wattributes`：attributes are not permitted in this position），符号出来仍是 DEFAULT；要用
`#pragma GCC visibility push(protected)`。用 pragma 之后实测：符号是 `GLOBAL PROTECTED`，而对它的调用
**连一条重定位都没有**（`.text.caller` 里只剩对另一个函数的 `PLT32`）——所以合并确实改不了它，
这条警告说的是真事。`-fvisibility=hidden` 的定义照样参与覆盖（`_ZTI3Box` 是 `WEAK HIDDEN` 而不是 local）。

### H9 跨模块 RTTI 靠字符串比较

同一次运行里宿主与模组的 `typeid(Box)` 地址不同（`94851159428280` vs `140516889927712`），
`dynamic_cast<Box*>` 仍然成功——libstdc++ 在指针不等时回退到名字比较。本机 GCC 16 实测：
`-fvisibility=hidden` 的类型是 `WEAK HIDDEN`、名字**没有** `*` 前缀；匿名命名空间里的类型是
`LOCAL`、名字也没有前缀（但名字本身就带 `(anonymous namespace)`，跨模块本来就对不上）。
结论：今天能用，但这是标准库实现细节在兜底；换标准库、换编译器要重新实测。

### H13 被销毁的映像会解除映射析构函数要跑的代码（已修）

`__dso_handle` 被回答成模块自己的基址，所以模块的静态析构函数是**注册在模块基址上的**（`__cxa_atexit`）。
`CodeImage` 在栈上被销毁时（嵌入者最自然的写法）直接 `munmap`，而进程退出时 glibc 会跑那些 handler——
跳进一个已经解除映射的页。

实测：加一个带非平凡析构函数的 fixture（`tests/data/tables/dtor.cpp`）之后，`test_code_table` 在 debug 与 release
**两个预设里都是 exit 139**，而且所有用例都已经打印过 `0 failure(s)`——崩在退出时。

修法（`code_table.cpp` 的 `CodeImage::release()`）：销毁时先对每个模块 `__cxa_finalize(module.base)`——这正是
`dlclose` 做的事——**全部跑完再解除映射**（一个模块的析构可能调用另一个模块的代码）。回归测试
`a_destroyed_image_runs_the_destructors_it_registered` 让 fixture 的析构函数写宿主里的一个 flag，
销毁映像后断言它已经是 1。启动器仍然故意不销毁映像（退出时由 glibc 正常跑析构），那条路径不变。

### H10 ~ H12（低）

* **对齐（已修）**：一个模块的两半从页边界开始，所以 `align > page` 的对齐**做不到**：现在直接报错拒绝
  （夹具 `aligned.cpp`：`alignas(8192)` 实测被拒），而不是默默放在别处。`align ≤ 4096` 一定对。实测 Release 表 686 节的 align 分布
  `{1:115, 2:54, 4:6, 8:71, 16:381, 32:32, 64:27}`，没有超过 4096 的。
* **`SHN_ABS`（已修）**：绝对符号的**值**现在留在表里，写值类的重定位直接填它（夹具 `abs_sym.cpp`：`abs_probe()` 得 `0x1234`）；
  而需要**地址**的重定位（调用、GOT）对着它会报错拒绝——把数字当地址写下去就是跳到不知道哪里。
* **CET（已修一半）**：`__CET__` 进了探针，是**必须一致**的事实（`_FORTIFY_SOURCE` 则归"可以不同"）——
  两边开关不一致时指纹就不同、直接拒绝。本机 GCC 不产 `endbr64`（`-fcf-protection` 默认关），但每个对象都带 48 字节
  `.note.gnu.property`，而且它**进了表**（Release 表 17 节，alloc）。宿主开、模组不开时，
  在强制 IBT 的机器上间接调用表内函数会 #CP。今天不是问题，发行版默认一改就是。

## 3. 方案与状态

| 项 | 内容 | 状态 |
|---|---|---|
| **P0-1** | 异常：本体那处 `std::stoll` → `std::from_chars`；本体表由工具链编；**帧描述随表走并注册**（H1） | **已落地**（表里 `throw` 能被自己与宿主接住；Release 表 0 节 `.gcc_except_table`） |
| **P0-2** | `R_X86_64_32` 范围检查（H5） | **已落地**（夹具被拒） |
| **P0-3** | 模块定义了宿主也导出的符号 → 警告（H6） | **已落地** |
| **P0-4** | `.init_array.NNNNN` 按后缀数字排序（H7） | **已落地**（41 → 14） |
| **P0-5 / P2-3** | 同名 vtable 的**槽位数**与**每槽符号**不一致 → 拒绝（H2） | **已落地**（两边都带 vtable 时；另一半由头哈希兜住） |
| **P1-1** | 构建时统一 ABI：探针 + 头文件哈希 + 表的记录 + 加载期比较（§1.5） | **已落地** |
| **P1-2** | 同名弱定义的**字节**比较 → 报告（H3）；`codetab build` 在打包前先链接，一个程序只有一份，
加载期只剩"别人编好的那些"要比较 | **已落地**（`twin_small`/`twin_big` 报警；工具链自己那张表 0 条警告） |
| **P1-3** | 编译开关指纹（探针事实 + 构建传的 `-D` + `__CET__`/`_FORTIFY_SOURCE`） | **已落地** |
| **P3-1** | 读 `st_other`；protected 的强定义 → 警告（H8） | **已落地**（打包在 binding 字的空位，版本不动） |
| **P3-2** | `align > page` → 拒绝（H10） | **已落地** |
| **P3-3** | `SHN_ABS` 保留常量；需要地址的重定位 → 拒绝（H11） | **已落地** |
| **P3-4** | CET 位进指纹（H12） | **已落地**（`__CET__` 必须一致） |
| **P2-2** | 类型探针清单（`--abi-type`：把"某个头变了"细化成"某个类型变了"） | **未做**——头文件哈希已经覆盖漂移本身，这一条只是把诊断说得更细，留作下一步 |
| **H13** | 销毁映像前先跑模块注册的析构（`__cxa_finalize`） | **已落地** |

回归测试都在 `tile2d/tests/test_code_table.cpp`（**33 用例 / 333 断言**）：`a_32_bit_absolute_address_that_does_not_fit_is_refused`、
`a_relocation_that_writes_nothing_is_applied_by_being_skipped`、`the_toolchain_links_a_build_before_it_packs_it`、
`an_absolute_symbol_is_its_value`、`an_absolute_symbol_where_an_address_is_wanted_is_refused`、
`constructors_run_in_priority_order_not_section_order`、`two_bodies_of_one_definition_are_reported`、
`a_module_that_defines_what_the_program_provides_is_reported`、`a_vtable_that_disagrees_about_its_slots_is_refused`、
`a_protected_definition_says_its_override_may_not_reach_every_caller`、
`a_section_that_asks_for_more_alignment_than_a_page_is_refused`、`a_throw_inside_a_table_finds_its_handler`、
以及 ABI 那一组（§1.5）与 `a_destroyed_image_runs_the_destructors_it_registered`。

## 3.9 原来的分步计划（留档）

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

* **不做"受限加载"**：没有 C-only 模式，也没有"少给点权限先跑起来"。类型不同就是不能调用，拒绝比一个会错的模块诚实。
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
| ABI | 同一源码 `--opt -O0` 与 `--opt -O3 --define NDEBUG` 各建一次 | `codetab abi <表>`、两表一起加载 | 同一指纹，两个表都加载 |
| ABI | `--define _GLIBCXX_USE_CXX11_ABI=0` | 同上 | 指纹不同，第二个表被拒并指名 |
| ABI | 改一行头文件后用 `--include` 指向改动的那份 | 同上 | `header <名字> differs` |
| ABI | `--define __AVX512F__=1` | 同上 | `it was compiled for 'avx512f' and this machine does not have it` |
| ABI | `--no-abi` 建一张表 | 同上 | 加载 + `does not say what ABI it was built as` |
| H13 | 带非平凡析构的 fixture，栈上 `CodeImage` | 运行测试二进制 | 修复前退出时 SIGSEGV（139）；修复后析构在销毁映像时跑 |
| H1 | `codetab pack` 一个用 `-fexceptions` 编出来的目标文件 | 看退出码 | 退出 1，并说明为什么（`--exceptions` 则只提示） |

每条修复的验收标准都写在 §3 对应条目里（命令 + 期望输出）。
