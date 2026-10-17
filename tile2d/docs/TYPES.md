# 内容逻辑层：`mine::types`

内容有两半。**定义**是数据：设计者写在 `.ecfg` 表里（`item::`、`structure::`、`machine::` …），
注册表按注册顺序发号（`docs/MODS.md`、`mine/registry.h`）。**逻辑**是 C++，住在 `mine::types`：
一个东西进入矿场之后"是什么、做什么"，由这里的类型说了算。

| 放这里 | 不放这里 |
|---|---|
| 矿场由什么组成：**地块**（`types/tile.h`）与它们的类别（场景/实体/地板）、**物品**（§6）、**实体**（§7，不在格子上、会动的东西），以及之后的物流、需求、层规则 | 定义本身——**代码里不出现任何资源、结构、机器、配方的名字**，有什么由设计者的数据决定（`docs/GAME_DESIGN.md` §7） |
| 把数据变成对象的**定义器**（`types/tile_definition.h`、`types/item_definition.h`）：读引擎会读的字段、按类别造出对象 | 数据的解析与登记（`content_loader.h`、`content_pack.h`）、贴图解码与图集（`app.h`） |
| 这些类型干活需要的东西（只要它是关于矿场的） | 存储与加载（`content_grid.h`、`registry.h`）、界面与调试工具（`app.h`、`sandbox.h`）、任何知道窗口/文件/GPU 的东西 |

```cpp
#include <mine/types/types.h>
using namespace mine::types;
```

---

## 1. 地块（`Tile`）

**地块是地图中占据格子的固定物体**：地板、矿物、机器都算。它被放在某处就不动。

* **一个地块可以不止一格**。为了之后的玩法，2×2、3×3、2×3 这样的大型结构地块要能表达：
  地块的"占地"是一个**格子矩形**，矩形左上角是它的锚点（`anchor()`），
  `covers()` 判断某一格是不是它的，`cell_count()` 是格数，`within()` 判断整个占地是否在地图内
  （放下 3×3 之前要问的就是它）。
* **地块不存名字**，只存注册表发的 kind 与 id——和地图格、存档完全一致（`mine/registry.h`）。
  名字随时可以从注册表问出来：`name(registry)`；内容消失时它回 `#<id>`，与沙盒对"已缺失"格子
  的显示是同一套说法。内容回来了，地块**保留 id 与位置**、由名字修好，而不是丢掉。
* **同名/占位判断**：`overlaps(other)` 只在**同一瓦片层**且格子相交时为真——地板与站在地板上的机器
  不重叠，因为它们在两层。
* **渲染朝向**：基类带一个 `random_reverse` 开关与一个 `mirrored()` 结果（§5）：为真时，**地图初始化**
  用层自己的随机数决定这个地块是否**左右翻转渲染**（50%），从此不再变。这个开关**由内容作者在数据里写**
  （`random_reverse:true`），`mirrored` 不进存档。
* **"我是不是地板"**：基类带一个 `is_floor()` 标记（默认假），地板类型（§4）把它答成真。
* 基类**没有公开构造函数**：地块一定是场景地块（可能还是"会跑的"场景地块，见 §3），没有"既不是场景
  也不是机器"的地块。

```cpp
// 3×3 的大家伙，锚点在 (10,20)，在第 0 层
SceneTile big(ContentKind::Structure, id, 0, GridPos{10, 20}, 3, 3);
big.covers(GridPos{11, 21});     // true
big.within(512, 512);            // 整块都在图里吗
```

## 2. 场景地块（`SceneTile`）：只在需要时更新

场景地块是"矿场由什么构成"：地板、墙、矿脉、装饰。**它不被 tick**——两种地块存在的全部理由就是这条：
几百格见方的图里，场景的数量比机器多几个数量级，而**每帧不花钱的场景才能到处都是**。

它只在有人叫它更新时更新：旁边的机器改变了世界、需求要问这里有什么、存档要写。**脏标记**就是
"有人叫它"的实现——调用方遍历自己的场景地块，只有脏的那些真的干活：一帧的成本因此是"改了多少"，
而不是"存在多少"。

```cpp
class OreVein final : public SceneTile {
public:
    using SceneTile::SceneTile;          // 构造：kind、id、层、锚点、占地
protected:
    void on_refresh() override { /* 重算自己缓存的东西 */ }
};

vein.mark_dirty();        // 旁边的机器改了世界，顺手告诉它一声（很便宜）
if (vein.refresh()) { }   // 这一帧真的干了活；没脏就是一次判断
```

新地块一开始就是脏的（还没人看过它），`refresh()` 清标记并调用 `on_refresh()`；
**没有任何定时器**：没被标记过的场景地块，之后的多少帧都不会运行一次（`test_mine_types` 里有这条）。

* **它有没有"容器"是可问的**（`container()`，`types/scene_tile.h`）：默认没有，仓库（`types/warehouse.h`）
  覆盖它返回自己的容器。问这一句是**界面的动作**——双击选中一个地块就是问它（`docs/GAME_DESIGN.md`
  §1.31）——所以它返回指针而不是引用："地板没有容器"这件事要说得出来。

## 3. 实体地块（`EntityTile`）：场景地块 + 自己的节奏

实体地块是**功能地块**：机器、传送带、钻机、需求终端。**它继承场景地块**（`EntityTile : SceneTile`）：
世界变化时的按需刷新、脏标记，它一样有——一台机器旁边的机器被拆了，它也可以被 `mark_dirty()`，
在下一次 `refresh()` 里重算自己缓存的东西。在此之上，它还要在游戏运行时被更新，而"多久一次"是它
自己的一部分——快的每 tick，慢的每几秒。这个节奏住在地块里，不住在模拟循环里，于是循环对谁都一样，
而慢机器只花慢机器的钱。

因为实体地块**是**场景地块，"这个要不要 tick"不该在运行时问地块——**层知道自己创建了什么**：
层把要 tick 的地块放在自己的列表里（创建时决定），于是刷新那一遍可以走过全部地块、只为脏的付钱，
而 tick 那一遍只走机器。这就是省下来的东西。

* `period_seconds()`：`0` = 每 tick；其它值是秒。`set_period_seconds()` 把负数读成 0。
* `advance(delta_seconds)`：把这一帧的时间交给它，到点就运行一次并返回 `true`。
* **节奏是时间，不是 tick 数**，而且**余数会留下**：一秒的周期不会因为帧长的小数部分而漂移。
* **一帧比一个周期长时，只运行一次**，并把**整段时间**交给它（`on_update(elapsed)`）——
  地块没法补跑它错过的 tick，假装能补会让"矿场产出多少"取决于帧是怎么掉的。这段时间怎么算
  是内容的事：按五秒记账的钻机可以产出五秒该有的量。

```cpp
class Drill final : public EntityTile {
public:
    using EntityTile::EntityTile;
protected:
    void on_update(f32 elapsed) override { /* elapsed 是这次要记账的时间 */ }
};

Drill drill(ContentKind::Machine, id, 0, GridPos{4, 4});
drill.set_period_seconds(0.5f);          // 每半秒一次
if (drill.advance(delta_seconds)) { }    // 到点了才 true
```

## 4. 两个标记：地板（`Floor`）与矿物（`SingleOre`）

**地板**是"其他东西站上去的那一层"，**矿物**是"一格一块、可以挖出来的东西"：两者都继承场景地块
（放一次、只在需要时刷新），**只多加一件事**——`is_floor()` / `is_ore()` 答"是"。就这一个标记，
让所有别的东西能问出自己真正想问的问题——"这里有没有东西可以站"、"这里有没有东西可以挖"——
而**不需要知道任何一块地板或矿物的名字**：机器在放下之前问脚下的格子，钻机在开工之前问它要挖的那一格。

```cpp
class Floor : public SceneTile {
    bool is_floor() const override { return true; }
};

class SingleOre : public SceneTile {
    bool is_ore() const override { return true; }
};

Floor dirt(ContentKind::Floor, id, 0, GridPos{4, 4});
SingleOre copper(ContentKind::Ore, id, 1, GridPos{4, 4});
dirt.is_floor();              // true
dirt.is_ore();                // false
copper.is_ore();              // true
machine_plot->is_ore();       // false：机器不是矿物
```

**矿物是"单"的**：一格一块——散点生成器一格放一个（`docs/MODS.md` §0），所以"哪一格有矿"和
"哪一块地块是那个矿"是同一个问题。它产出的东西是它背后的**内容**（注册表发出去的 id），
不是这个类自己编的数字；跨好几格的矿脉以后是另一种东西。

**为什么是方法而不是数据里的一个开关**：是不是地板、是不是矿物，是**东西的种类**，不是一份文件能发给
谁的性质——是地板的内容就是 `floor::` 表里的（`types/floor.h`），是矿物的内容就是 `ore::` 表里的
（`types/single_ore.h`），不是的不会因为在文件里写一句就变成矿物。一格可以两个都答"是"：
地板在下一层、矿在上一层，就是两个地块。

## 5. 渲染朝向：`random_reverse`

同一份内容在一张图上重复成千上万次时，每一个都朝着同一边看会很假——矿脉、地板块、装饰尤其明显。
所以地块带一个开关：

```cpp
tile.set_random_reverse(true);     // 内容数据说"这块内容可以掉头"
tile.randomise_mirror(layer_rng);  // 地图初始化时掷一次：50% 左右翻转
tile.mirrored();                   // 之后渲染按它来（左右翻转 = u 轴取反）
```

* **只在地图初始化时掷一次**，之后 `mirrored()` 就是一个常量；渲染每帧读它，不再掷骰子。
* **掷的是层自己的随机数**（`t2d::Rng`，种子来自世界的 seed），所以同一个矿场两次生成完全一样
  （`docs/GAME_DESIGN.md` §2 的可复现要求）。
* **开关为假的地块不消耗随机数**：把一块内容改成可翻转，不会把排在它后面的地块的骰子整体挪一位。
* **它只关于渲染**：地块**做什么**从不被镜像（一台机器的进料口不会因为画反了就真的换边）。
* 想手工指定（存档里存了这个结果、编辑器里手动摆）就用 `set_mirrored()`。

## 6. 物品（`Item`）：不在图上的内容

**物品**是矿场**搬运**的东西，而不是摆在某一格里的东西：机器产出它、配方进出它、需求点名它、玩家携带它
（`docs/GAME_DESIGN.md` §1.22）。它没有地块、没有占地、没有层——"这个物品在哪一格"问的是**容器**，
而容器是设计者的数据（§7.10）——所以它**不继承 `Tile`**：共用一个基类等于说两者是同一种东西，而地块有
层与占地、物品两样都没有，一格放得下地块、永远放不下物品。

```cpp
const ContentId plate_id = registry.register_content(ContentKind::Item, "plate");
const Item plate = make_item(definition, plate_id);   // types/item_definition.h

plate.kind();                  // ContentKind::Item
plate.id();                    // 注册表发出去的那个 id（存档写的就是它）
plate.image();                 // 数据里写的那张图："art/plate.png"
plate.name(registry);          // "plate"；内容没了就是 "#<id>"（与地块同一段文字）
plate.valid(registry);         // 它背后的内容还在不在
Item empty;                    // 空槽：没有内容、没有贴图
```

* **身份与地块同一套**：类别 + 注册表发出去的 id，**名字永远在注册表里**（`mine/registry.h`）——所以数据
  改了名，存档按名字对得上；内容被删掉，物品保留自己的 id 并**说出来**（`#<id>`、`valid()` 假），而不是
  悄悄变成占了这个数字的另一条内容（与地块的 `missing()` 同一个故事，`docs/MODS.md` §4）。
* **引擎为它读的字段只有一个**：`image`。`random_reverse` 是**地块**的字段（"这份内容画在地图上能不能
  掉头"），物品不在图上，写了也只是设计者字段（`docs/MODS.md` §0）。
* **一个类覆盖所有物品**（定义器因此比地块那半边小，`types/item_definition.h`）：地块按类别分好几种类，
  因为地块**做的事**不同（机器会跑、地板会被踩）；物品只有一件事——被搬运——所以让一个物品区别于另一个的
  是它背后的**内容**，不是类型。

## 7. 实体（`Entity`）：不在格子上的东西

**实体**是矿场里**会动**的东西：它有一个**位置**，而不是一格；它**移动**、能被要求**交互**，并且**默认每
tick 被更新**（`docs/GAME_DESIGN.md` §1.24）。它与物品也不是一回事：物品是"能被搬运的内容"（§6），
实体是"在某一层里、有位置、会动的东西"。

```cpp
MineLayer layer(0, LayerShape{64, 48, 1}, 1);

Entity* cart = layer.spawn(ContentKind::Machine, cart_id, t2d::Vec2{3.5f, 2.25f});
cart->position();                     // 3.5, 2.25 —— **不与瓦片地图对齐**，没有东西会把它对齐到格子上
cart->move(t2d::Vec2{0.25f, 0.0f});   // 移动会告诉它所在的那一层，索引跟着走
cart->set_period_seconds(1.0f);       // 一秒一次；不设就是每 tick
cart->set_auto_update(false);         // "不要自动更新我"：它还在、还能动，只是由别的东西驱动
cart->on_interact(other);             // 默认什么都不做，并且说出来；有反应的内容返回真

layer.entities_within(t2d::Vec2{4.0f, 4.0f}, 2.0f);   // 四叉树：这一点附近有哪些实体
layer.interact(*cart, *other);                        // 一次交互，两边都被问
```

* **不继承 `Tile`**：地块是格子（占着格、固定、有朝向），实体是一个点（位置 + 移动）。两者共享的是**身份**：
  类别 + 注册表发出去的 id + 名字（内容没了就是 `#<id>`，`missing()` / `valid()` 与地块同一套）。
* **节奏与地块同一条规则**（`types/update_cadence.h`）：默认每 tick；声明间隔就按间隔；声明不自动更新就
  一次也不跑。**一帧比一个周期长时只跑一次**，把整段时间交给它，不重放错过的 tick——一次卡顿不能改变矿场
  产出什么。
* **四叉树**（`mine/quadtree.h`）：一层里的实体编入一张四叉树（根是这一层的矩形，节点超过 16 个条目就
  分裂，最多 8 层），所以"这一点附近有什么"付的是**答案的代价**：10000 个实体铺在 1000×1000 里，一次半径
  20 的查询走 21 个节点、只测 31 个条目，而线性遍历要测 10000 个。查询结果按**出生顺序**给出，同一个问题
  两次得到同一个答案。
* **谁负责让索引跟着走：实体自己**。`set_position()` / `move()` 会告诉它所在的那一层（它知道 `layer()`），
  所以"一层里所有实体都在索引里"是**不变量**，而不是一条要靠调用者记住的规矩；`destroy()` 是**立即出索引、
  帧末出存储**，正在遍历实体的回调手里的指针因此仍然有效。
* **实体能反过来用层**：`layer()` 让它的逻辑在自己的回合里问"附近有什么"、造一个、毁一个、要求一次交互——
  这是内容将来写"矿车撞上就装货"这类东西的地方。**交互具体是什么**仍然是设计者的（§7.14）。

## 8. 掉落物（`Drop`）：不自动更新、可以被拖动

**掉落物**是实体（§7）的一种，它把两件事定下来：**层不更新它**（躺在那儿不会自己变化），
**指针可以拖它**（按住左键拿起来、移动、松手放下）——这是设计者给的**第一个指针动作**
（`docs/GAME_DESIGN.md` §1.25；其余动作仍等 §7.14 的动作表）。

```cpp
MineLayer layer(0, LayerShape{64, 48, 1}, 1);

Drop& drop = static_cast<Drop&>(layer.adopt(
    make_drop(definition, registry.find(ContentKind::Drop, "scrap"), t2d::Vec2{10.0f, 10.0f})));

drop.auto_update();   // false —— 层不会更新它
drop.draggable();     // true  —— 指针可以拖它（普通实体是 false）
drop.position();      // 10, 10 —— 自由位置，不与瓦片地图对齐
drop.name(registry);  // "scrap"；内容没了就是 "#<id>"（与地块、物品同一段文字）

PointerDrag drag;                              // pointer_drag.h
drag.begin(layer, t2d::Vec2{10.25f, 10.0f});   // 取最近的、可拖的那个（0.75 格以内）
drag.move_to(t2d::Vec2{20.25f, 15.0f});        // 跟着指针走，抓取偏移保留，夹在地图内
drag.end();                                    // 放下：它停在放下它的地方
```

* **它不是地块**：`kind_is_a_tile(Drop)` 为假——掉落物不占格子，它躺在一个位置上。
* **引擎为它读的字段只有一个**：`image`（它躺在地上长什么样）。`random_reverse` 是**地块**的字段
  （地图初始化时掷的，§5），掉落物是后来才放下的、之后还会被拖走，所以它没有这个字段
  （`docs/MODS.md` §0）。
* **"不自动更新"是它是什么，不是字段**：`Drop` 的构造函数就把它关掉，条目里写什么都改不了这一点——
  就像一块地板不能靠数据让自己每帧跑一次（§2）。
* **数据侧**（`drop::` 表，`types/drop_definition.h`）：`read_drop_definition()` / `drop_definitions()` /
  `make_drop()`，与物品那一半同一个形状（§9）。
* **世界视图怎么画它**：一格大、**以位置为中心**——所以它躺在四格之间时就是压在四格上，这正是"位置不是
  格子"的样子；它画在所有地块之后，因为躺在地上的东西在上面。

![两颗掉落物躺在矿场里（夹具内容）](../games/mine/docs/images/world_drops_en.png)

## 9. 钻头（`Drill`）：实体地块，按"硬度 ÷ 能力"出料

**钻头**是**实体地块**（§3）：它占着一格、会跑，跑起来做的事情是**挖它脚下的矿**。它站在矿**上面**——
矿在它下面的瓦片层，因为一个格子每层只放一个地块（`docs/GAME_DESIGN.md` §1.12、§1.26）。数据侧两个
数字决定节奏：矿的 `hardness`（硬度）与钻头的 `dig_power`（挖掘能力），**周期 = hardness / dig_power**；
到点它就往外丢一个**那条矿对应的掉落物**（§8），四个方向轮流、一格远。

```cpp
// drill:: rig:: dig_power:1.0     ore:: vein:: hardness:2.0 drop:"chunk"
MineLayer layer(0, LayerShape{24, 16, 3}, 1);
layer.place(registry, definitions, at(ContentKind::Floor, "dirt", 0, 5, 5));   // 地板
layer.place(registry, definitions, at(ContentKind::Ore, "vein", 1, 5, 5));     // 矿：下面那一层
layer.place(registry, definitions, at(ContentKind::Drill, "rig", 2, 5, 5));    // 钻头：上面那一层
Drill* drill = dynamic_cast<Drill*>(layer.plot_at(2, GridPos{5, 5}));

drill->ready();            // true —— 脚下有矿，而且那条矿说清了自己多硬、掉什么
drill->period_seconds();   // 2.0  = hardness 2.0 / power 1.0
layer.tick(2.0f);          // 它的回合到了：丢出一个掉落物
layer.entity_count();      // 1 —— 就是那个掉落物，落在钻头旁边一格
```

* **它不自己找矿**：层在**建层时**就把脚下那条矿、它的硬度、它掉什么、以及算出来的周期交给钻头
  （`DrillJob`，`types/drill.h`）——那是注册表（名字 → id）与定义（设计者的数字）同时在手的唯一时刻。
  于是"脚下没矿""矿没说自己多硬""矿没说自己掉什么""钻头没说自己多能挖"都是**建层时就报出来的拒绝**
  （`LayerBuildReport`），而不是一台永远默默不动的机器。
* **一帧比一个周期长时按时间出料**：`on_update` 拿到的是这一帧覆盖的整段时间（§3 的节奏规则），
  所以一次卡顿丢掉三个周期就是三个掉落物，而不是一个——"一次卡顿不改变矿场产出什么"。
* **掉落物落在外面**：四个方向轮流、一格远，所以钻头不会把自己埋在一个点上；落在图外时贴边。
  丢出来的是**实体**（§8），所以它能被指针拖走——"机器出料、玩家搬走"这条路已经通了。
* **矿不会被挖完**：一条矿脉有多少、挖完怎么办是设计者的数据（`docs/GAME_DESIGN.md` §7.3/§7.10）；
  引擎现在只按时间出料，不减少矿。
* **地块第一次够得到世界**：`Tile::owner()` 是它所在的层（`types/tile.h`），钻头就是靠它把掉落物放进层里
  （`owner()->adopt(...)`）。在这之前只有实体有这条路（§7）。

## 10. 仓库（`Warehouse`）：场景地块 + 三个子对象

**仓库**是**场景地块**（§2）：它占着格子、是布景、**永远不被 tick**——不是"会跑的功能地块"（§3 的实体地块，
钻头那种），而是**有人对它做什么的时候才发生什么**：一件掉落物被拖过来，或者被点一下
（`docs/GAME_DESIGN.md` §1.27）。

```cpp
// warehouse:: shed:: image:"art/shed.png" capacity:4
Warehouse& shed = ...;                        // 放在某一层、某一格上（1x1 或更大）

shed.container().capacity();    // 4 —— 定义器给的容量
shed.container().total();       // 0 —— 现在装着几件
shed.draginto().accepts(drop);  // 还有空间就为真
shed.draginto().receive(drop);  // 整件收下；收不下就什么都不变
shed.on_click(GridPos{6, 5});   // 点在这一格上：面板展开（或收起）
shed.click().open();            // 面板开着没有 —— 画面按它决定画不画
shed.container().slots();       // 里面有什么，按到达顺序：{kind, id, count}
```

三个子对象是**各自独立的部件**，因为它们在别处也用得上：

* **`container`**（`types/container.h`）：容量 + 里面有什么 + `add` / `take`。两个方法都**返回实际收下/
  给出多少**：满了就是 0，快满了就给能放下的那些——调用者不必猜。它装的是"类别 + id + 数量"，与存档写的是
  同一对东西，所以机器、需求、存档、模组都能直接跟它打交道（这正是设计者要的"便于和其他代码交互"）。
* **`draginto`**（`types/drag_into.h`）：接收拖过来的掉落物，**要么整件收下、要么什么都不变**——
  半件进一个满箱子是在撒谎。
* **`click`**（`types/click.h`）：接收点击，**点在它身上**展开/收起面板，**点在别处**收起（只有一个指针，
  所以只有一件东西是选中的）。

* **容量来自条目**（`capacity`，必须是大于零的整数）：没写就是装不下任何东西的仓库，而不是被塞了一个谁也
  没写的尺寸的仓库。
* **拖动入库**在 `PointerDrag::end()` 里（`pointer_drag.h`）：看掉落物落在哪一格，**从最高瓦片层往下**找
  那一格上的仓库——与画面上看到的一致；进得去就进容器、地上那件随即消失，进不去就留在放开的地方，
  HUD 在错误色里说"满了"。
* **面板画在地块的右下角**（`app.cpp` 的 `draw_warehouse_panel`）：标题是仓库自己的**本地化名称**
  （`content.name.<种类>.<名字>` → `content.name.<名字>` → 都没有就用种类自己的词），每一行是
  **[贴图][本地化名称][数量]**（`docs/GAME_DESIGN.md` §1.28）。名称的查找在 `mine/content_name.h`
  （`content_label`）；贴图是内容自己的那一张，所以面板由**两趟**画——字在 overlay 里、图在自己的图片趟里，
  两趟都问同一个 `warehouse_panel_layout()`，不会各画各的。

## 11. 数据怎么变成地块：定义器

定义在数据里（`docs/MODS.md`），地块在 C++ 里，中间这一步就是**定义器**
（`types/tile_definition.h`）：

```
floor::                          TileDefinition{ kind=Floor, name="dirt",
    dirt::                ──▶                     image="art/floor_dirt.png",
        image:"art/floor_dirt.png"                random_reverse=false }
        random_reverse:true
                                 ──▶  make_tile(...)  ──▶  Floor{ ... }
```

* `read_tile_definition(kind, name, entry)` 读**引擎会读的那两个字段**（`image`、`random_reverse`），
  其余字段原样不动；读不懂的字段**拒绝并说明**（`random_reverse:1` 不会悄悄当成假）。
* `tile_definitions(document)` 走一份 `.ecfg`，只收"占格子"的类别（floor / ore / structure /
  machine），其它表跳过。**名字不是类别**：`item:: ore::` 是一条叫 ore 的**物品**，不是矿物。
* `make_tile(definition, id, layer, anchor, 宽, 高)` 按**类别**决定类：
  `floor` → `Floor`，`ore` → `SingleOre`，`machine` → `EntityTile`（机器是"还会跑的"场景地块），
  `structure` → `SceneTile`；不是地块的类别返回空。**这份对应关系是工程**（东西是什么），
  不是内容（有什么东西）。
* 加载时管线也会读一遍这两个字段：**数据里写错了当场报告**，而不是等到建层的时候才发现。
* **物品的定义器是同一个形状的另一半**（`types/item_definition.h`，§6）：`read_item_definition()` /
  `item_definitions()` / `make_item()`，读的字段只有 `image`（物品不在图上），加载时同样读一遍并留下
  （`ContentDefinitions::find_item()`）。两个定义器**各走各的表**：`item_definitions()` 只收 `item::`，
  `tile_definitions()` 只收占格子的类别，所以一份文档里叫同一个名字的地板与物品互不干扰。

## 12. 加一个新类型

1. 在 `include/mine/types/` 下加一个头文件，写清它是什么、为什么这样切；
2. 在 `src/types/` 下加实现，进 `mine_core`（**不依赖 Ore**：内容逻辑要能在专用服务器上跑）——同时
   进 `MINE_TABLE_SOURCES`，因为本体自己是一张表（`docs/TABLES.md`）；
3. 在 `types/types.h` 里 include 它；
4. 在 `tests/test_mine_types.cpp` 里用**测试替身**验证行为（不是验证内容——内容还没有）。

一个类型**不属于地块家族**时（物品 §6、实体 §7），这条一样成立，只是它进的是自己的测试文件
（`test_entities.cpp`），并且要在`types/types.h` 之外想清楚它**是不是内容**：实体现在就不是。

## 13. 谁在用它们：世界

地块的第一个使用者是**世界**（`mine/world.h`，`docs/GAME_DESIGN.md` §4）：

* `MineLayer` 把一条内容变成地块（`place()` → `make_tile`）、**掷一次翻转骰子**、
  把**整个占地写进 `ContentGrid`**（3×3 的九格都指向同一条内容），并把它登记进 32×32 的块索引——
  于是"这一格是谁的"既有 4 字节一格的答案（`ref_at`），也有占地感知的答案（`plot_at` 对九格回同一个地块）。
* `MineLayer::refresh()` 走过全部地块、只为脏的付钱；`tick(dt)` 只走**创建时**被分进 tick 列表的那些
  （机器是 `EntityTile`，所以它在里面；地板与结构不在）。
* `blocks(掩码, 格)` 回答"这一格挡不挡路"——**哪一层挡路由设计者的数据决定**，掩码是参数。
* 世界视图（`Enter` / `--world-view 1`）就是它们的屏幕：地块按占地画、`mirrored` 翻 u 轴。
* 一层**从哪来**也是数据：故事模式下是 `layer::` 里的层规则（`mine/layer_rules.h`，
  `docs/GAME_DESIGN.md` §4），它产出的仍是同一个 `LayerSpec`——世界因此不需要知道自己在故事里还是
  无尽里。层规则点名的地板/矿物不存在时，`place()` 的拒绝就是唯一的答案（层照建，原因写进报告与画面）。
* **散点生成器**（`layer::` 的 `scatter::`）把矿物一格一块撒在指定的瓦片层上：可用格子、数量与地板
  限制都在规则里（`docs/MODS.md` §0）。它撒下去的就是普通的矿物地块——世界这一侧没有第二种东西。

* **实体走另一条路**（§7）：它们不在格子上，所以不经过块索引，而是 `MineLayer` 的**四叉树**——
  `entities_within(点, 半径)` 与 `for_each_entity_within` 回答"这附近有什么"（结果按出生顺序），
  `spawn` / `adopt` / `destroy` 是它们的一生，`interact(a, b)` 把一次交互问给两边。`tick(dt)` 同时走
  地块的 tick 列表与实体：地块多、继续用创建时分好的列表，实体少、由层走过而每个自己决定。
  层本身用 `unique_ptr` 持有（`MineWorld`）——**层里的东西指回层**，层被搬走那些指针就废了。
* **掉落物是实体里第一个"有图、还会被拖"的**（§8）：世界视图把它画成一格大、以位置为中心的图，画在所有
  地块之后；左键按下由 `PointerDrag` 接管（`app.cpp` 只负责把屏幕坐标换成世界坐标）。
  `--layer-drops 1` 把 `drop::` 的每一条在层里铺一个，好在"按数据放掉落物"的规则出现之前先看见它们。
* **钻头把"会跑的地块"与"掉落物"接起来**（§9）：层在建层时把脚下的矿与算出来的周期交给它
  （`configure_drill`），它到点就往层里放一个掉落物实体（`owner()->adopt(...)`）——地块第一次够得到世界
  （`Tile::owner()`）。`--layer-drills 1` 把 `drill::` 的每一条放在一个有矿的格子上，好在"按数据放设备"
  的规则出现之前先看见它出料。
* **仓库是"有人对它做什么才发生什么"的那一类**（§10）：世界视图把掉落物拖到它上面就进容器
  （`PointerDrag::end()`），点一下就画面板；两条路都只经过它的子对象，层与画面都不需要知道它里面是什么。

* **世界视图说出它画的是什么**：HUD 的格子那一行、内容列表与沙盒调色板上的名字都走 `content_label()`
  （`docs/TEXT.md` §"Naming content"）——字符串表里有名字就用它，没有就回注册表里的名字；而**一格画多大**
  由设计者给的 40–100 像素/格决定（`mine/view_limits.h`，`docs/GAME_DESIGN.md` §1.30），进层取景也停在
  这个范围里。
* **双击选中的是"有容器的地块"**：`SceneTile::container()`（§2）是那一问，`mine/double_click.h` 是判据
  （同一个格子、两次按下、0.4 秒之内），选中之后容器内容由 `container_panel()` 排一次、画两趟（字与贴图）——
  与右下角那个面板同一份（`docs/GAME_DESIGN.md` §1.31）。单测 `test_double_click` 8 用例 / 28 断言。

单测 `test_world`（13 用例 / 146 断言）覆盖上面每一条，包括**同一 `(seed, 层号)` 两次生成逐字节相同**；
实体那半边是 `test_entities`（8 用例 / 129 断言，§7）。

## 14. 现在还没有的

* **没有内容规则**：谁在什么时候把哪些内容放到哪些格子上，是**设计者的层规则**（`docs/GAME_DESIGN.md`
  §7.3/§7.4/§7.8/§7.9）。世界已经能把一条内容变成地块、把一层建出来、把两条推进路径跑起来；
  故事模式的层规则（`mine/layer_rules.h`）已经能读 `size::` 与 `floor::` 的 `full_flash`，
  所以"每个格子是什么地板"有答案了——**"每层有哪些资源/结构、故事有多少层"还没有**（§7.3/§7.8）；
  无尽模式与两个调试生成器之外仍然什么都不放（空层也是层）。"一个机器具体做什么"同样等设计者（§7.7）。
* **物品只是内容**：`Item` 说的是"这是哪条内容"（§6）——能注册、能定义、能画，但**还不能被拿着**：
  没有容器、没有配方、没有需求，它们一起等设计者的 §7.2/§7.6/§7.10。
* **实体还没有内容表**：`Entity` 能被造出来、能跑、能被查到，但**没有 `entity::` 定义器**——
  设计者要的是"实体类"（§1.24），不是"实体 ecfg 注册器"（§1.22 那次两样都要）。现在能造出实体的只有 C++：
  本体，或代码表模组。**掉落物是例外**：它有 `drop::` 表（§8），所以"躺在地上的东西"已经能用数据定义。
* **没有数据说"哪里该有掉落物"**：`drop::` 说得清"有这么一种掉落物"，说不清"在这一层放一个"——
  现在能放下它的只有 `--layer-drops` 这个调试填充，或 C++。掉落物也**不说它是谁的**：
  "这是哪条物品掉出来的、有几个"是设计者的数据模型（`docs/GAME_DESIGN.md` §7.2/§7.6/§7.10），
  引擎现在只读它的 `image`；拾取、背包、需求点名同样都还没有。
* **没有数据说"哪里该有钻头"**：`drill::` 说得清"有这么一台机器"，说不清"在这一层放一台"——
  现在能放下它的只有 `--layer-drills` 这个调试填充，或 C++。**矿也不会被挖完**：一条矿脉有多少、
  挖完怎么办是设计者的（§7.3/§7.10），引擎只按时间出料。
* **仓库里装的是掉落物，不是物品**：掉落物仍然不说自己是哪条物品（§8 的遗留，等 §7.2/§7.6/§7.10）。
  **而且只有放进去、没有取出来**：`take` 这一半在容器上（§10），但还没有任何界面或规则调用它——
  物流与机器取料都还没有。
* **地块没有世界访问**：`EntityTile::on_update(elapsed)` 现在只拿到时间（实体有 `layer()`，§7）。
  机器要读邻居、背包、需求时，那个上下文随模拟一起设计，而不是现在猜。
* **没有存档**：地块怎么进快照（含"哪一格是它的锚点"）等模拟的形状定了再说。现在的地图格只存
  "这一格是哪条内容"，地块列表在内存里；一层从**当前**注册表建出来，所以内容没了是"放不下并报告"，
  而不是留下一块过期的东西——按名字修好旧地块是**存档**那条路的事（`docs/GAME_DESIGN.md` §6）。
* **模组的逻辑不在这里**：`mine::types` 是**本体**自己的内容逻辑。原生模组跨的是 C ABI
  （`docs/MODS.md` §3），跨过那条线的只能是纯数据与函数指针，所以 `dlopen` 进来的模组提供不了
  `Tile` 子类——它的定义进同一个注册表（数据侧完全一样），行为走它自己的模块。**代码表模组**
  （`docs/MODS.md` §3.5、`docs/TABLES.md`）是另一回事：它和本体一起编译、在启动时合并成一个程序，
  所以那条边界不在 C ABI 上，而在公开面上。
