# 内容包工作区

这里放**内容包项目**。游戏运行时如果工作目录下有 `packs/`，它会自动加载——所以在这个目录里
（`tile2d/`）启动游戏，你写的包就已经生效了：

    cd tile2d
    ./build/debug/games/mine/mine_game --world sandbox --start 1        # 自动加载 ./packs
    ./build/debug/games/mine/mine_game --world sandbox --start 1 --packs packs/template

## 什么是内容包

**一个 `.ecfg` 文件就是一个包**，可选一个 `pack::` 头说明自己是谁。没有清单文件、没有目录结构、
没有代码——需要"跑点什么"时才用模组包（`docs/MODS.md`）。

    packs/
      template/                  # 一个包项目 = 一个目录
        template.ecfg            #   包本体（一个 .ecfg）
        README.md                #   怎么写这个包
      my_pack/                   # 复制 template/ 改个名就是新包
        my_pack.ecfg
        art/                     #   多边形贴图的坐标草稿、配色表……随便放什么，只要不是 .ecfg
        README.md

`--packs <dir>` 会**递归**找出目录下所有 `.ecfg`（跳过以 `.` 开头的项），按路径排序，再按 `requires` 调整顺序。
所以一个工作区里放多少个包项目都可以。

## 写完之后怎么试

1. 跑起来看：`mine_game --world sandbox --start 1`，右侧面板列出注册到的每一个名字与它拿到的 id。
2. 改文件按 **F5**：不重启重新加载全部来源（本体内容 → 内容包 → 模组包），已放置的格子按**名字**重新定位。
3. 校验：`ctest --test-dir build/debug -R test_content_pack` 里有一个用例专门加载这个工作区——
   模板包语法错、`pack::` 里写错键、依赖成环，测试就会红。

## 两条已确认的美术/操作需求（见 docs/GAME_DESIGN.md §1）

* **贴图采用多边形风格**：这是**美术风格**（低多边形、平色块、硬边），**贴图仍然是图片资源**——
  PNG 文件放在包目录里，内容条目用相对路径引用（`image:"art/wall.png"`），引擎加载时校验文件存在。
  引擎不做矢量多边形渲染。图片尺寸/切片/命名等美术规则还没定（§7 第 13 项）。
* **没有玩家角色，所有玩家使用上帝视角操作**：内容里**不需要**任何角色贴图或角色数据；玩家输入驱动的是
  相机与指针。所以包里不会有"玩家"这一类条目。
