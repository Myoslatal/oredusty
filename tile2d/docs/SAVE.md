# 存档：`.t2dsave` 与 `t2d::save`

设计者原话："做存档功能，格式命名为.t2dsave，存档做成单独的模块方便多个游戏，甚至是多个机制不同的游戏复用"。

## 为什么是一个独立模块

存档里有两件事，必须分开：**容器**（一个文件、它的版本、它的校验和、它怎么被读回来）和**内容**
（这个世界存了什么）。容器对所有游戏都一样，内容只有那个游戏知道。`t2d::save` 只做容器：
它把一份存档拆成若干个 **chunk**（四字符 id + 版本 + 字节），从不看任何一个 payload 里面是什么，
也不认识任何游戏的名字、格子、实体或数值。

所以一个回合制战棋、一个物理沙盒、一个和它们机制完全不同的游戏，可以共用同一套读/写/校验代码，
各自决定自己的 chunk id、版本与字段；**读不懂的 chunk 原样留着**（游戏忽略它，存档不丢它），
存档工具与玩家之间的兼容性因此不依赖任何一个游戏的内部结构。

## 文件格式（所有整数小端、无填充、无尾部字节）

| 字段 | 大小 | 说明 |
|---|---|---|
| magic | 4 字节 | `T` `2` `D` `S`（`t2d::save::kMagic`，即 `fourcc('T','2','D','S')`） |
| format version | u16 | 容器格式版本，当前 `1`；不认识就拒绝，绝不猜 |
| game name | u16 + 字节 | UTF-8，1..64 字节；区分两个游戏的存档 |
| game version | u32 | 游戏自己的版本号，容器只负责带上 |
| chunk count | u32 | 0..256 |
| chunk（每个） | | |
| &nbsp;&nbsp;id | u32 | 四字符 id（fourcc），不能是 0 |
| &nbsp;&nbsp;version | u32 | **游戏自己的** chunk 版本；容器只记录、不解释 |
| &nbsp;&nbsp;payload size | u32 | 该 chunk 的字节数，最大 64 MiB |
| &nbsp;&nbsp;crc32 | u32 | payload 的 CRC-32（IEEE 802.3，与 zip/PNG 同一个多项式） |
| &nbsp;&nbsp;payload | 字节 | 游戏自己的数据 |

chunk 的 `version` 是**游戏**的：容器不做版本比较，玩家用旧游戏读新存档时，"这个 chunk 我认不认得/
能不能读"是游戏自己判断的事。容器只保证：同样的字节读出来还是一样的字节。

## 用法

```cpp
#include <t2d/save/archive.h>
namespace save = t2d::save;

save::File file;
file.game = "puzzle";                      // 1..64 字节
file.game_version = 3;

save::Writer writer;                       // 一个 chunk 的 payload
writer.write_u32(7);                       // 第 7 回合
writer.write_string("blue");               // 轮到谁
file.chunks.push_back({save::fourcc('T','U','R','N'), 1, writer.finish()});

std::string error;
const std::vector<u8> bytes = save::encode(file, &error);
if (bytes.empty()) { /* error 里是可读的原因 */ }

const std::optional<save::File> back = save::decode(bytes, &error);
if (!back.has_value()) { /* 坏文件：error 说明哪一步不成立 */ }

if (const save::Chunk* turn = back->find(save::fourcc('T','U','R','N')); turn != nullptr) {
    save::Reader reader(turn->payload);
    u32 round = 0;
    std::string player;
    if (reader.read_u32(round) && reader.read_string(player)) { /* ... */ }
}

save::write_file("campaign.t2dsave", file, &error);
const std::optional<save::File> loaded = save::read_file("campaign.t2dsave", &error);
```

`Writer` / `Reader` 是 **payload** 的工具（u8/u16/u32/u64/i32/f32/bool/string/bytes）：
`i32` 用 zigzag（和引擎的 `ByteWriter` 一致），`f32` 按位写（浮点原样往返），`string` 是 u16 长度 + 字节。
读越界就返回 `false` 并带一句可读的 error，不抛异常、不返回半个结果；写坏了之后 `ok()` 变假、后续写入被忽略。

文件名助手：`save::ends_with_extension(path)`（大小写不敏感）、`save::with_extension(path)`（没有就补
`.t2dsave`）。约定：**所有存档以 `.t2dsave` 结尾**，界面上的 "保存/另存为" 只用这一个扩展名。

## 错误、版本与限制

`decode()` 拒绝一个文件的条件，每一条都给可读的 error：

* magic 不对、`format version` 不认识；
* 截断（在任何字段中间结束）、尾部多余字节；
* `game` 为空或超过 64 字节；chunk 数超过 256；
* chunk id 为 0、payload 超过 64 MiB、同一 id 出现两次；
* payload 与它记录的 CRC-32 不符（翻一位就能抓到）。

`encode()` 同样拒绝这些（空/过长 game、>256 chunk、id 0、重复 id、payload 超限），返回空 vector + error：
一个写不出合法格式的编码器不能悄悄写一个坏文件。

容器版本（`format version`）变了就是**不兼容**：旧游戏读新文件会明确拒绝，而不是误读。游戏自己的
`game_version` 与每个 chunk 的 `version` 都由游戏解释——容器只把它们带回来。

## 第二个游戏（机制完全不同）怎么复用它

同一个容器、完全不同的字段。一个物理沙盒：

```cpp
#include <t2d/save/archive.h>
namespace save = t2d::save;

save::File file;
file.game = "sandbox";
file.game_version = 12;

save::Writer bodies;
bodies.write_u8(2);                          // 两个刚体
for (u32 id = 1; id <= 2; ++id) {
    bodies.write_u32(id);
    bodies.write_f32(1.5f);                  // x
    bodies.write_f32(-2.25f);                // y
    bodies.write_bool(false);                // 是否休眠
}
file.chunks.push_back({save::fourcc('B','D','Y','1'), 4, bodies.finish()});

save::Writer clock;
clock.write_u64(123456789ull);
clock.write_bool(true);                      // 暂停中
file.chunks.push_back({save::fourcc('C','L','K','1'), 2, clock.finish()});

const std::vector<u8> bytes = save::encode(file);
```

它和战棋的例子共用同一份 `archive.h`、同一套 CRC 与限制，但两边的 chunk id、版本、字段没有一个字相同；
一个游戏读另一个游戏的存档也不会崩——只是 `find()` 找不到自己的 chunk。`tile2d/tests/test_save.cpp` 里
这两个游戏就是测试用例（`two_games_with_different_mechanics_share_one_archive`）。

## Mine 的存档

`mine` 的存档（`tile2d/games/mine/include/mine/game_save.h`）用同一个容器：`game` = `"mine"`，
`game_version` = 1，每个 chunk 的 `version` 也都是 1（`kGameSaveChunkVersion`）。六个 chunk 缺一不可：

| chunk | 内容 |
|---|---|
| `META` | 会话（模式/角色/种子/端口/连接地址）、世界形状与种子、层的编号/形状/种子、世界时钟、镜头中心与缩放 |
| `CONT` | 写档时的注册表**名字表**（kind + name），读档时按名字把存档的 id 翻成本次构建的 id |
| `UNLK` | 建筑解锁表（`BuildingUnlocks::serialize`：'UNL1' + 版本 + 每行 kind/id/unlocked） |
| `PLOT` | 每块地的 kind/id、所在层、锚点、尺寸、镜像、建造中、计时参数、钻头产量、容器内容；**按放置顺序** |
| `ENTT` | 地上的掉落物：kind/id、位置、数量（堆） |
| `BUIL` | 进行中的建造扫描线：地块在 `PLOT` 里的下标、已扫时间、总时间 |

读档是**要么整座矿、要么什么都没有**（`load_game_save`）：

* 六个 chunk 必须都在、版本都是 1；认不得的 chunk 被忽略而不是报错（新存档旧游戏仍能读）。
* `CONT` 的名字表必须能**完整**翻译：存档里有、本次构建没有的内容，整档拒绝（并报出缺了哪些）。
* 地块按存档顺序经 `MineLayer::place` 重新放置，所以放置规则（层、格子、钻头底下要有矿）第二次也照样生效；
  一块放不下、或容器装不下存档里的东西，整档拒绝。
* `BUIL` 与 `PLOT` 的“建造中”必须一致：每块建造中的地块恰好有一条能恢复的 `BUIL`，`BUIL` 指向的地块必须
  正在建造、同一块不能出现两次、已经扫完的不算“进行中”。不一致就拒绝，而不是猜。
* 掉落物用 `adopt` 原样放回，不再合并：堆的大小和位置与存档一致。
* 已销毁但还没被 tick 扫掉的掉落物**不写进存档**（只写 `alive()` 的），否则它会在读档时复活。
* 镜像计数在全部地块放好之后用 `MineLayer::recount_mirrored()` 重新统计：放置时按层 RNG 掷的骰子不是存档里的那个。

**保存了**：会话、世界/层的种子与形状、层的编号、全部地块与容器、掉落物、进行中的建造、建筑解锁、世界时钟、
镜头位置与缩放。

**没有保存**：钻头的 RNG 流（读档后按内容 id/锚点重新播种，之后的产出序列不保证逐位相同）、界面状态
（选中的地块、双击计时、拖在指针上的掉落物、建造栏是否打开）、内容包/模组的加载列表本身（存档只记内容的
**名字**，读档用的是当前进程加载的那一套；名字对不上就拒绝，而不是退回本次构建里 id 相同的别的东西）。

### 入口与按键

* **F7 保存、F8 读取**。文件默认是当前工作目录的 `save.t2dsave`，路径用 `--save <path>` 指定；没写扩展名时由
  `t2d::save::with_extension` 补上。F8 读的就是 F7 写的那个文件。
* `--load <path>` 在启动时直接读档并进入世界视图（跳过开始/会话界面）；失败时回到开始界面并报错。写哪个文件由
  `save_file_path()` 决定：`--save` > 本局槽位（active，见下面“菜单里的存档”）> `--load` > 默认 `save.t2dsave`——
  读档后按 F7 是把这座矿存回它来的那个文件，不是另起一个名字。
* **自动存档**：在世界视图里每 5 分钟自动存一次（`--autosave-seconds <秒>` 改周期，0 关闭）；只算世界视图里的时间，
  F7、每次成功存档、读档与进入世界都会重新计时。成功与手动保存一样在状态行显示，失败走 `world.save.failed`。
* **退出即存**：点窗口的叉或 quit 时，只要世界里还有层（开始界面没有）就存一次——会话/内容界面也会存，因为矿还在内存里；
  存档在释放渲染资源之前完成，失败只 warn、不阻止退出。
* **headless 默认不写**：自动存档与退出存档只在显式给了 `--save` 或 `--load` 时才写（截图/脚本不该覆盖玩家的存档）；
  窗口模式始终写。手动 F7 不受限制。
* 世界视图底部提示行有 `F7 SAVE F8 LOAD`；状态行用 `world.save.ok` / `world.save.failed` / `world.save.no_layer`、
  `world.load.ok` / `world.load.failed` / `world.load.no_file` 文案（`assets/text/ui.ecfg`，三种语言）。
* 读档失败**不会动当前正在玩的矿**：`load_game_save` 先完整读、完整校验，全部成功才替换世界；也正因如此，
  替换时先 `building_.clear()`（旧地块还活着）、丢掉指针上拖着的掉落物，再换世界、恢复会话/解锁/时钟/镜头，
  重建建造栏并清掉选中与双击。
* 模型层测试：`tile2d/games/mine/tests/test_game_save.cpp`（9 例 / 152 断言）；容器本身在
  `tile2d/tests/test_save.cpp`（11 例 / 158 断言）。

### 菜单里的存档（槽位）

开始菜单的 **Start Game** 是一个子菜单：**New Game** / **Load Autosave** / **Save List** / **Back**。它把存档组织成
**槽位**（`mine/save_library.h`）：

    saves/<名称>/auto.t2dsave

* 存档根目录是**当前工作目录下的 saves/**（app 用 `current_path()/saves` 构造 `SaveLibrary`，测试用临时目录）。
* 每个槽位是一个目录，目录名就是玩家输入的名字；里面固定一个 `auto.t2dsave`——**最近一次自动存档**，不存历史。
* **名称规则**（`save_name_error_id`，失败返回 locale id）：非空、合法 UTF-8、最多 48 个字符且最多 128 字节；不允许
  `/ \ : * ? " < > |` 与控制字符（tab 除外：它是名字的一个字符，见下）；不允许前后空格、结尾点、`.` / `..`、Windows 保留名
  CON/PRN/AUX/NUL/COM1..9/LPT1..9（大小写不敏感，比较首个点前的部分，所以 `con.txt` 也拒绝）；CJK 合法；已存在目录算重名。
* **New Game**：输入名称（CJK、四字节 emoji、退格、Delete、Left/Right/Home/End；**Up/Down 在 Input / Create / Back 之间
  环走焦点**；**Tab 在输入框有焦点时插入一个制表符**，Create/Back 焦点下不插入；模型里存真实的 tab，输入框画成空隙；其它
  C0 控制字符、换行与 DEL 仍拒绝）；Enter 在输入框或 Create 上都创建，Back/Esc 返回。创建成功后以该槽位的 `auto.t2dsave`
  为 active 路径，进入世界并立刻写一份初始自动存档。
* **Load Autosave**：取所有槽位里 mtime 最新的 `auto.t2dsave`（空槽不参与）；没有时状态行显示 `save.load_autosave.none`。
* **Esc 离开矿场**：世界视图里按 Esc **先存档一次**（有层时；失败只在开始界面的状态行报错，不阻止离开），然后回到这个
  存档子菜单——不再经过会话屏。
* **窗口平台与输入法（fcitx5）**：默认 `WindowPlatform::Auto`（Linux 上 Wayland 优先）。GLFW 的 Wayland 后端没有
  text-input，所以 Ore 自己在窗口层接了 `zwp_text_input_v3`：名称框聚焦时 enable，commit 的文本进 `InputState::add_text`，
  preedit 单独给界面显示（见 `src/platform/wayland_text_input.h`）；compositor 不支持时只是 no-op。`--platform x11` 仍可
  兜底，X11 的输入法走 GLFW 的 XIM/char callback。
* **Save List**：每个槽位一行；有自动存档的槽位可以展开出"最近的自动存档"子行。上/下移动，Right 或 Enter 在头行
  展开/收起，Left 折叠，子行 Enter 读取，Esc 回子菜单；空列表显示 `save.list.empty`。
* **active 路径与覆盖**：New Game / Load Autosave / Save List 读取成功都把该槽位的 `auto.t2dsave` 记为 active；
  `save_file_path()` 的优先级是 `--save` > active > `--load` > 默认 `save.t2dsave`，所以 F7/F8、自动存档与退出存档都写同
  一个文件。Host/Join 开始新会话时清掉 active。
* 这三个模型在 `games/mine/tests/test_text_input.cpp`（7/81）、`test_save_library.cpp`（8/92）、
  `test_save_list.cpp`（8/66）。

## header-only 的取舍

这个模块**只有头文件**（`include/t2d/save/archive.h`），CMake 里是一个 `INTERFACE` target（`t2d::save`）：

* **好处**：引擎的公开接口面（`engine.api`）不因为一个只有游戏用的格式而扩大；游戏把实现直接编进自己的
  程序或代码表（mixin/模组都用同一个头），launcher 不需要为它加任何东西；任何游戏只要 include 这个头就能用。
* **代价**：每个游戏拿到自己的一份 inline 函数（几 KiB 代码），没有一处共享的 "存档助手" 可以修一次全体受益——
  升级时各游戏重新编译自己的那份；格式本身由 `format version` 兜底。

对一个"方便多个游戏复用"的格式，这个取舍是故意的：**格式在框架里，数据在游戏里，ABI 面不扩大**。
