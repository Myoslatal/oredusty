# Ore 架构说明

本文说明框架的分层、生命周期与关键取舍。阅读顺序建议：先看[分层与依赖](#分层与依赖)，
再看[帧生命周期](#帧生命周期)，然后是[GPU 内存分配器](#gpu-内存分配器)与
[同步与图像布局](#同步与图像布局)，最后是[ECS 设计](#ecs-设计)与[扩展点](#扩展点)。

---

## 分层与依赖

```
        examples/*            应用（用户代码）
             │
        ore/app               Application：命令行、生命周期、主循环、截图
             │
   ┌─────────┴─────────┐
ore/renderer         ore/platform
Mesh/UploadRing/     Window(GLFW)/InputState/
Renderer/World(ECS)  InputMap
   └─────────┬─────────┘
        ore/rhi            Vulkan 后端：Instance/Device/GpuAllocator/Buffer/Texture/
             │             Sampler/Descriptor*/Pipeline*/Command*/Fence/Semaphore/
             │             RenderTarget/Swapchain/GraphicsContext/ShaderModule
        ore/math           glm 别名、Vulkan 约定矩阵、相机与控制器（不依赖 rhi）
        ore/core           类型/日志/断言/计时/文件/图像(PNG)/空闲链表/命令行（不依赖 Vulkan）
```

依赖方向严格单向：`core → math → rhi → renderer → app`，平台层只依赖 `core` 与 GLFW，
`math` 只依赖 glm 与 `core`。因此 `core`、`math`、ECS 与输入映射都能在没有 GPU、
没有窗口的情况下单测。

命名与所有权约定：

| 约定 | 含义 |
|---|---|
| `Scope<T>` | `std::unique_ptr<T>`，独占所有权；工厂函数返回它 |
| `Ref<T>` | `std::shared_ptr<T>`，共享所有权（框架内很少用） |
| `ORE_NON_MOVABLE(T)` | 持有 Vulkan 句柄的类型禁止拷贝/移动，避免句柄被复制 |
| `create()` 返回 `Scope<T>` | 失败时返回 `nullptr` 并已打印错误；不会抛异常 |
| `ORE_ASSERT` / `ORE_VERIFY` | 违反 API 契约（未绑定管线就写描述符等）时立即中止 |
| `ORE_VK_CHECK` | Vulkan 调用失败即 fatal（`VK_SUBOPTIMAL_KHR` 与业务相关的返回值除外） |

---

## 帧生命周期

```
Application::run()
  ├─ 解析命令行 → on_configure() → 创建 Window / GraphicsContext / Renderer
  ├─ on_start()                        一次性的资源创建
  └─ while (running)
       ├─ window->poll_events()        InputState 边沿与增量在这里刷新
       ├─ 热重载轮询 → on_shader_reload()
       ├─ on_update(dt)                游戏逻辑（ECS 系统、相机）
       ├─ Renderer::begin_frame(dt)
       │    ├─ fence->wait()           等本 slot 的上一帧完成（CPU 最多领先 N 帧）
       │    ├─ command_pool->reset() / descriptor_pool->reset()
       │    ├─ ring->begin_frame(slot) 回绕本帧的环形缓冲段
       │    ├─ swapchain->acquire_next_image()（离屏模式跳过）
       │    └─ command_buffer.begin()
       ├─ on_render(frame)
       │    ├─ Renderer::begin_pass(clear)   → RenderTarget::begin()
       │    │     ├─ 颜色/深度图 layout → attachment layout（必要时插 barrier）
       │    │     └─ vkCmdBeginRendering
       │    ├─ 绑定管线/描述符/push constants，Mesh::draw()
       │    └─ Renderer::end_pass()          → RenderTarget::end()
       │          ├─ vkCmdEndRendering
       │          └─ layout → final（离屏 SHADER_READ_ONLY / 交换链 PRESENT_SRC）
       └─ Renderer::end_frame()
            ├─（可选）截图拷贝：颜色图 → 主机可见回读缓冲
            ├─ vkQueueSubmit2（等待 image_available，signal render_finished + fence）
            └─ vkQueuePresentKHR（等待 render_finished）
```

**为什么用 fence 而不是 timeline semaphore 做帧同步**：帧级同步只需要"这个 slot 的上一帧
完成了没有"，二进制 fence 语义最简单、开销最低。timeline semaphore 用在
`ImmediateCommands`（上传/回读）上，那里需要"等待到某个具体的提交值"，且不能阻塞整个队列。

**每帧资源三件套**：命令池、描述符池、upload ring 段。三者在 `begin_frame()` 一起重置，
因此应用在 `on_render()` 里分配的任何每帧资源都只属于当前帧，不会与仍在使用中的上一帧冲突。

**resize 与 swapchain 重建**：窗口回调只设置 `resize_pending_`；真正的重建发生在下一帧
`begin_frame()` 开头（先 `wait_idle()`，再销毁并重建 swapchain、图像视图与每个交换链图像的
`RenderTarget`）。`VK_ERROR_OUT_OF_DATE_KHR` / `VK_SUBOPTIMAL_KHR` 同样走这条路径。
离屏模式则直接以新尺寸重建颜色/深度图。

---

## GPU 内存分配器

`GpuAllocator` 的目标是"够用的 VMA"：一个 `VkDeviceMemory` 块内做次分配，避免每个资源一次
`vkAllocateMemory`（驱动侧开销大、且有数量上限）。

```
VkDeviceMemory 块 (默认 16 MiB device-local / 4 MiB host-visible)
  └─ BlockAllocator（按 offset 排序的空闲链表）
       ├─ best-fit 查找：选出满足 (size, alignment) 且剩余最小的空闲区
       ├─ 前导 padding 切分为独立空闲区；尾部余量切分为新空闲区
       └─ free 时与前后空闲区合并（防碎片）
```

关键取舍：

- **块按 (内存类型, 线性/非线性) 分组**。当 `bufferImageGranularity > 1` 时，buffer（线性）与
  optimal tiling 图像不能共享同一段内存，否则违反 Vulkan 规范；granularity 为 1 的设备
  （多数桌面 GPU）则共用同一池，节省内存。
- **大分配走独立 `VkDeviceMemory`**（≥ `dedicated_threshold`，默认 32 MiB），
  避免一个巨型资源把整块内存钉死。
- **host-visible 块持久映射**：整块 `vkMapMemory` 一次，次分配直接给指针；
  非 coherent 内存由 `Buffer::flush()/invalidate()` 处理。
- **不整理碎片**：`trim()` 释放完全空闲的块（关卡切换时调用），运行期不搬迁资源——
  搬迁需要重写所有描述符，代价与复杂度都不划算。
- **块复用必须匹配"是否需要映射"**：在核显上 `find_memory_type(DEVICE_LOCAL)` 与
  `find_memory_type(HOST_VISIBLE|HOST_COHERENT)` 常常返回同一个内存类型，但为 device-local
  请求创建的块并没有映射。若复用它来满足 host-visible 请求，就会拿到空映射指针
  （`Buffer::write` 直接断言失败）。因此选择复用块时会额外要求 `block->mapped != nullptr`，
  并且一旦 host-visible 分配拿不到映射就报错返回而不是把空指针交给调用方。
  回归测试：`test_gpu_render` 的 `gpu_host_visible_staging_is_always_mappable`。
- **flush/invalidate 必须按 `nonCoherentAtomSize` 对齐**：规范要求
  `VkMappedMemoryRange` 的 offset 是该值的整数倍、size 是它整数倍或 `VK_WHOLE_SIZE`，
  并夹在分配范围内；非 coherent 内存（核显常见）不会替你兜底。范围换算被抽成纯函数
  `align_flush_range()` 并由 `test_rhi_helpers` 覆盖。
- **coherent 与否取自实际拿到的内存类型**，而不是"申请时写了 HOST_COHERENT 就当它 coherent"——
  否则在非 coherent 类型上会静默跳过 flush。
- 分配算法（`BlockAllocator`）是**纯主机逻辑**，与 Vulkan 无关，因此可以在没有 GPU 的
  机器上完整单测（见 `tests/test_core_block_allocator.cpp`）。

上传路径：`GraphicsContext::upload_buffer()/upload_texture()` 用一块 host-visible staging
缓冲 + `ImmediateCommands`（一次性命令缓冲 + timeline semaphore + 主机等待）完成拷贝，
纹理随后自动转到 `SHADER_READ_ONLY_OPTIMAL`，可选 blit 生成完整 mip 链。

---

## 同步与图像布局

- 提交统一走 **`vkQueueSubmit2`**（synchronization2）：等待/信号都是 `VkSemaphoreSubmitInfo`，
  队列等待 stage 用 `VkPipelineStageFlags2`，与 barrier 语义一致。
- barrier 统一走 `vkCmdPipelineBarrier2`；`CommandBuffer::transition_image()` 为常见
  layout 组合（UNDEFINED→COLOR_ATTACHMENT、COLOR_ATTACHMENT→PRESENT_SRC、
  TRANSFER_DST→SHADER_READ_ONLY、SHADER_READ_ONLY→COLOR_ATTACHMENT 等）推导默认的
  stage/access 掩码，需要精细控制时用 `image_barrier()` 显式指定。
- **layout 由 `RenderTarget` 跟踪**：`begin()` 前是 `UNDEFINED`（离屏新图）或上一帧的
  final layout，`begin()` 转到 attachment layout，`end()` 转到
  `final_color_layout`。因此应用只写"开始渲染/结束渲染"，不会漏 barrier 也不会重复转换。
- **交换链**：每个交换链图像配一个 `RenderTarget`（含自己的深度图，避免多帧同时使用同一张
  深度图）。acquire/present 使用二进制信号量（每 slot 一个 image_available，每交换链图像一个
  render_finished），帧完成用 fence。

---

## 描述符与管线

- `DescriptorSetLayout` 由 `DescriptorBinding{ binding, type, count, stages, flags }` 数组描述；
  `flags` 用于 descriptor indexing（partially bound / runtime array）。
- `Renderer::allocate_descriptor_set(layout)` 从**当前帧**的描述符池分配，池的大小按上一帧的
  实际用量自动翻倍增长（`needs_pool_rebuild_`），因此不需要手动估算容量。
- 生命周期长于单帧的描述符（例如 1×1 默认纹理、材质纹理数组）应自建
  `rhi::DescriptorPool`，示例 02/03 演示了这种"持久池 + 每帧池"的组合。
- 管线构建器把 `VkPipelineRenderingCreateInfo`（动态渲染的附件格式）直接接进
  `vkCreateGraphicsPipelines`，没有 `VkRenderPass`/`VkFramebuffer` 概念。
  **注意**：颜色/深度附件格式是管线状态的一部分，切换目标格式（例如换成 HDR 中间目标）
  需要重建受影响的管线。

---

## ECS 设计

```
World
 ├─ slots_: vector<Slot{ generation, alive, parent, children }>   实体表 + 空闲索引栈
 └─ pools_: unordered_map<type_index, Scope<IComponentPool>>
      ComponentPool<T>（稀疏集）
        dense:      vector<T>          紧凑存储，遍历友好
        entities:   vector<Entity>     与 dense 平行，用于回调参数
        sparse:     vector<u32>        实体索引 → dense 下标（kInvalidIndex 表示没有）
```

- **句柄安全**：`Entity{index, generation}`。销毁时 generation 自增，旧句柄的
  `World::valid()/try_get()` 一律失败；索引本身进入空闲栈复用。
- **`each<Ts...>(fn)`**：选取组件数最少的池作为遍历基准，再检查其它组件是否存在，
  跳过不匹配的实体；**零堆分配**（有测试用全局 `operator new` 计数器证明），
  遍历中增删其它实体安全。删除正在遍历的组件是未定义行为（swap-and-pop 会打乱顺序）。
- **层级**：`set_parent()` 维护 `Slot::children`（权威数据）与可选的 `Hierarchy` 组件镜像；
  `update_transforms()` 从根开始按拓扑序（迭代链式上行，非递归）计算
  `world = parent_world * local_matrix()`，检测到环时告警并断开而不是死循环。
- **为什么不用 archetype**：稀疏集的插入/删除是 O(1) 且实现短小、可读、可测；
  本框架的目标规模（数千实体）下遍历性能足够，换来的是几十行可审计的代码。

---

## 扩展点

| 想做的事 | 接入方式 |
|---|---|
| 自定义渲染目标（阴影、后处理） | `rhi::RenderTarget::create()` + `Renderer::begin_pass(target, clear)` |
| 换光照/材质模型 | 改 `shaders/mesh.frag`，或用 `ore_add_shaders()` 加自己的着色器与管线 |
| 加载 glTF/OBJ | 转成 `MeshData` + `Mesh::create()`；图像转成 `Image` + `create_texture()` |
| 计算着色器 | `ComputePipeline` + `CommandBuffer::dispatch()`（`ImmediateCommands` 适合一次性计算） |
| 多线程录制 | 每线程一个 `CommandPool`，主线程统一提交；`GpuAllocator` 需外部加锁 |
| 运行期改着色器 | `recompile_shader()` + `ShaderModule::create()`，重建管线后替换 |
| 单文件发布 | `-DORE_EMBED_SHADERS=ON` 把 SPIR-V 编进可执行文件 |

---

## 验证策略

框架自身用三层验证，且都能在无显示器环境运行：

1. **纯逻辑单测**（分配器、PNG、命令行、ECS、相机、输入映射）——不碰 GPU。
2. **真实 GPU 端到端测试**——两条独立路径：
   - `test_gpu_render`：离屏渲染后回读像素并与期望值比较（清屏颜色、push constant 变色、
     缓冲/纹理上传往返、mip 链、目标尺寸变更、分配器统计）。
   - `test_gpu_swapchain`：用 `VK_EXT_headless_surface` 建一个**真实交换链**，
     完整跑 acquire → 渲染 → submit → present、从呈现图像截图、以及 resize 后重建交换链。
     这条路径覆盖了窗口模式下的同一份代码，却不需要窗口系统。
   没有 Vulkan 设备时两组都自动 SKIP，不会把 CI 拖红。
3. **参考截图**——三个示例在软件 Vulkan 上渲染真实 PNG（`docs/images/`），
   用于人工确认视觉结果没有回归。

**已知的环境限制**：开发容器里没有 GPU，只有 Electron 打包的 SwiftShader。它的 Wayland WSI
会在 `vkGetPhysicalDeviceSurfaceSupportKHR` 内部解引用空指针而崩溃（最小 C 探针即可复现，
与框架无关），因此窗口 + 交换链的组合无法在该环境验证；交换链代码改由上面的
`VK_EXT_headless_surface` 测试覆盖。`AppConfig::allow_headless_fallback` 会在窗口上下文
创建失败时退化成离屏渲染并告警。
