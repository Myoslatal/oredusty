# 内容逻辑层：`mine::types`

内容有两半。**定义**是数据：设计者写在 `.ecfg` 表里（`item::`、`structure::`、`machine::` …），
注册表按注册顺序发号（`docs/MODS.md`、`mine/registry.h`）。**逻辑**是 C++，住在 `mine::types`：
一个东西进入矿场之后"是什么、做什么"，由这里的类型说了算。

| 放这里 | 不放这里 |
|---|---|
| 矿场由什么组成：**地块**（`types/tile.h`），以及之后的物流、需求、层规则 | 定义本身——**代码里不出现任何资源、结构、机器、配方的名字**，有什么由设计者的数据决定（`docs/GAME_DESIGN.md` §7） |
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
* 基类**没有公开构造函数**：地块只可能是下面两种之一，没有"既不是场景也不是机器"的地块。

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

## 3. 实体地块（`EntityTile`）：按自己的节奏更新

实体地块是**功能地块**：机器、传送带、钻机、需求终端。它要在游戏运行时被更新，而"多久一次"是它
自己的一部分——快的每 tick，慢的每几秒。这个节奏住在地块里，不住在模拟循环里，于是循环对谁都一样，
而慢机器只花慢机器的钱。

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

## 4. 加一个新类型

1. 在 `include/mine/types/` 下加一个头文件，写清它是什么、为什么这样切；
2. 在 `src/types/` 下加实现，进 `mine_core`（**不依赖 Ore**：内容逻辑要能在专用服务器上跑）；
3. 在 `types/types.h` 里 include 它；
4. 在 `tests/test_mine_types.cpp` 里用**测试替身**验证行为（不是验证内容——内容还没有）。

## 5. 现在还没有的

* **没有模拟**：地块还没有被任何东西创建（M2 是它的第一个用户），因为"一个机器具体做什么"是设计者的
  内容（`docs/GAME_DESIGN.md` §7.3/§7.4/§7.7）。
* **没有世界访问**：`on_update(elapsed)` 现在只拿到时间。机器要读邻居、背包、需求时，那个上下文
  随模拟一起设计，而不是现在猜。
* **没有地块与地图的桥**：把一个 3×3 地块写进 `ContentGrid`（以及"哪一格是它的锚点"如何随存档往返）
  属于地图侧，和 M2 的模拟一起做——现在的地图格只存"这一格是哪条内容"。
* **没有存档**：地块怎么进快照，等模拟的形状定了再说。
* **模组的逻辑不在这里**：原生模组跨的是 C ABI（`docs/MODS.md` §3），跨过那条线的只能是纯数据与
  函数指针，所以模组不能提供 `Tile` 子类。`mine::types` 是**本体**的内容逻辑；模组的定义进同一个
  注册表（数据侧完全一样），模组的行为走它自己的模块。
