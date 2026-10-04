# 内容逻辑层：`mine::types`

内容有两半。**定义**是数据：设计者写在 `.ecfg` 表里（`item::`、`structure::`、`machine::` …），
注册表按注册顺序发号（`docs/MODS.md`、`mine/registry.h`）。**逻辑**是 C++，住在 `mine::types`：
一个东西进入矿场之后"是什么、做什么"，由这里的类型说了算。

| 放这里 | 不放这里 |
|---|---|
| 矿场由什么组成：**地块**（`types/tile.h`）与它们的类别（场景/实体/地板），以及之后的物流、需求、层规则 | 定义本身——**代码里不出现任何资源、结构、机器、配方的名字**，有什么由设计者的数据决定（`docs/GAME_DESIGN.md` §7） |
| 把数据变成地块的**定义器**（`types/tile_definition.h`）：读引擎会读的字段、按类别造出对象 | 数据的解析与登记（`content_loader.h`、`content_pack.h`）、贴图解码与图集（`app.h`） |
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
**没有任何定时器**：没被标记过的场景地块，过一万帧也不会运行一次（`test_mine_types` 里有这条）。

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

## 4. 地板（`Floor`）：一个标记

地板是"其他东西站上去的那一层"：它继承场景地块（放一次、只在需要时刷新），**只多加一件事**——
`is_floor()` 答"是"。就这一个标记，让所有别的东西能问出自己真正想问的问题——"这里有没有东西可以站"
——而**不需要知道任何一块地板的名字**：机器在放下之前问脚下的格子，寻路在迈进去之前问前面的格子。

```cpp
class Floor : public SceneTile {
    bool is_floor() const override { return true; }
};

Floor dirt(ContentKind::Floor, id, 0, GridPos{4, 4});
dirt.is_floor();              // true
machine_plot->is_floor();     // false：机器不是地板
```

**为什么是方法而不是数据里的一个开关**：是不是地板是**东西的种类**，不是一份文件能发给谁的性质——
是地板的内容就是 `floor::` 表里的（`types/floor.h`），不是的不会因为在文件里写一句就变成地板。

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

## 6. 数据怎么变成地块：定义器

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
* `tile_definitions(document)` 走一份 `.ecfg`，只收"占格子"的类别（floor / structure / machine），
  其它表跳过。
* `make_tile(definition, id, layer, anchor, 宽, 高)` 按**类别**决定类：
  `floor` → `Floor`，`machine` → `EntityTile`（机器是"还会跑的"场景地块），
  `structure` → `SceneTile`；不是地块的类别返回空。**这份对应关系是工程**（东西是什么），
  不是内容（有什么东西）。
* 加载时管线也会读一遍这两个字段：**数据里写错了当场报告**，而不是等到建层的时候才发现。

## 7. 加一个新类型

1. 在 `include/mine/types/` 下加一个头文件，写清它是什么、为什么这样切；
2. 在 `src/types/` 下加实现，进 `mine_core`（**不依赖 Ore**：内容逻辑要能在专用服务器上跑）；
3. 在 `types/types.h` 里 include 它；
4. 在 `tests/test_mine_types.cpp` 里用**测试替身**验证行为（不是验证内容——内容还没有）。

## 8. 现在还没有的

* **没有模拟**：地块还没有被任何东西创建（M2 是它的第一个用户），因为"一个机器具体做什么"是设计者的
  内容（`docs/GAME_DESIGN.md` §7.3/§7.4/§7.7）。定义器（§6）已经能把一条内容变成地块，
  缺的是**建层**的那一步：谁在什么时候把哪些内容放到哪些格子上。
* **大地块还没有落地**：`make_tile` 收得下 2×2、3×3 的占地，但"把 3×3 写进 `ContentGrid`、
  以及哪一格是它的锚点如何随存档往返"还是地图侧的事，和 M2 一起做（§1 的矩形已经为它准备好了）。
* **没有世界访问**：`on_update(elapsed)` 现在只拿到时间。机器要读邻居、背包、需求时，那个上下文
  随模拟一起设计，而不是现在猜。
* **没有地块与地图的桥**：把一个 3×3 地块写进 `ContentGrid`（以及"哪一格是它的锚点"如何随存档往返）
  属于地图侧，和 M2 的模拟一起做——现在的地图格只存"这一格是哪条内容"。
* **没有存档**：地块怎么进快照，等模拟的形状定了再说。
* **模组的逻辑不在这里**：原生模组跨的是 C ABI（`docs/MODS.md` §3），跨过那条线的只能是纯数据与
  函数指针，所以模组不能提供 `Tile` 子类。`mine::types` 是**本体**的内容逻辑；模组的定义进同一个
  注册表（数据侧完全一样），模组的行为走它自己的模块。
