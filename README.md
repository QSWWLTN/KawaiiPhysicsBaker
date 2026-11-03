# Kawaii Physics Baker 使用说明

Kawaii Physics Baker 用于在 Unreal Editor 中预览并烘焙 KawaiiPhysics 骨骼物理。它以固定时间步长运行动画图，将身体、衣物等多个 Skeletal Mesh 的最终姿态分别保存为同步的 `AnimSequence`。

插件还提供两项配套功能：

- **物理校准**：用多个动作进行 GPU 参数筛选和 CPU 复验，生成可供实时 KawaiiPhysics 使用的配置资产。
- **烘焙衣物 IK 修正**：在游戏运行时，根据身体最终 IK 姿态修正已烘焙的衣物骨链。

## 准备工作

1. 确保项目启用 `KawaiiPhysics` 和 `KawaiiPhysicsBaker`，编译项目的 Editor 目标并重启编辑器。
2. 使用本仓库配套的 `Plugins/KawaiiPhysics`。Baker 依赖其中的共享碰撞、衣物碰撞和校准接口，不能假定任意原版 KawaiiPhysics 都能直接替换。
3. 准备角色 Skeletal Mesh 和非叠加（Non-Additive）的 `AnimSequence`。直接播放动画时，动画与对应 Mesh 必须使用同一个 Skeleton；工具不自动重定向。

烘焙蓝图及校准工具属于编辑器功能。游戏中播放生成的动画，或使用 `KawaiiPhysicsBakerRuntime` 提供的 IK 修正节点；不要把烘焙蓝图作为角色的运行时 Anim Class。

## 快速开始：烘焙一个 Mesh

### 1. 创建烘焙蓝图

在关卡编辑器选择 **Tools → 创建 Kawaii 物理烘焙蓝图**，选取目标 Skeletal Mesh，再保存蓝图，例如 `ABP_KawaiiBake_Body`。

工厂会设置对应的 Skeleton、预览 Mesh 和 `KawaiiPhysicsBakeAnimInstance` 父类，创建 `Bake Source Pose → Output Pose`，并打开 **Kawaii 物理烘焙** 面板。以后可通过该蓝图工具栏的 **物理烘焙** 按钮重新打开。

### 2. 配置 AnimGraph

在默认连接中插入 KawaiiPhysics 及空间转换节点：

```text
Bake Source Pose
    → Local To Component
    → KawaiiPhysics
    → Component To Local
    → Output Pose
```

在 KawaiiPhysics 节点上设置需要模拟的根骨骼、物理参数和碰撞约束。多个物理节点可以按需要串联。新建蓝图默认没有物理节点，仅保留默认连接会输出源姿态。

每个烘焙图必须有且仅有一个连接到输出求值路径的 `Bake Source Pose`。不要使用 Sequence Player、Blend Space Player 等动画播放器代替它；源动画和采样时间由烘焙面板统一控制。

### 3. 选择动画并预览

在 **物理烘焙** 面板设置：

| 参数 | 默认值 | 用途 |
| --- | --- | --- |
| Main Mesh | 创建时选择的 Mesh | 主部件，内部名称为 `Main` |
| Source Animation | 未设置 | 主部件的非叠加动画，与 Main Mesh 使用同一 Skeleton |
| Sample Rate | `60` | 固定采样帧率，范围 `1–240` |
| Warm Up Frames | `60` | 正式采样前保持源动画第 0 秒姿态的物理预热步数，范围 `0–3600`；预热不写入动画 |
| Output Directory | `/Game/KawaiiBakes` | Unreal 内容目录，不是磁盘路径 |
| Output Prefix | `Baked` | 输出资产名前缀，必须是非空的合法资产名 |

左侧 **物理预览** 直接显示身体和全部衣物部件的最终物理姿态，无需导出动画。配置完整时打开面板会自动预览；修改源动画、采样或部件设置后会重新预览。也可以点击 **编译并预览** 编译主图和所有部件图，从预热开始播放，或双击右侧动画列表切换动作。

视口下方提供动画时间轴、当前时间、帧号和缓存帧数：

- **拖动时间轴**：暂停并显示指定帧。拖动期间视口会持续刷新，已缓存的帧立即显示；尚未计算的帧按固定步长继续模拟，状态栏显示准备进度。继续拖动可以改变目标位置。
- **播放／暂停**：继续播放或暂停，保留当前物理姿态；在末帧点击 Play 会从头播放。
- **`|<` / `<` / `>` / `>|`**：跳到首帧、上一帧、下一帧、末帧。跳到尚未缓存的末帧会先补算整个动作。
- **循环**：默认开启，播放完毕后循环回放本次缓存；关闭后停在末帧。
- **聚焦角色**：重新把角色放入视野，可配合旋转、缩放视口检查穿模。

时间轴缓存仅保存在内存中，不创建动画资产。修改或重新编译参与的图、修改源动画会使旧缓存失效；修改图后点击 **编译并预览** 重新生成预览。烘焙时视口同样显示当前采样姿态，时间轴禁止拖动，导出始终从全新的预热状态开始。

### 4. 导出动画

点击 **烘焙新动画**，等待状态显示 `已保存 … 条同步动画`，然后点击 **定位结果** 在内容浏览器中选中结果。

输出会自动保存，例如：

```text
/Game/KawaiiBakes/Baked_Walk_Main_a1b2c3d4
```

命名格式为 `<前缀>_<主源动画名>_<部件名>_<本次组 ID 前 8 位>`。每次生成新资产，不覆盖源动画或已有结果。记得保存烘焙蓝图，以保留图和面板设置。

| 按钮 | 行为 |
| --- | --- |
| 编译并预览 | 编译全部参与的图，从预热开始预览 |
| 重置 | 重建预览场景并复位，不自动播放或编译 |
| 烘焙新动画 | 编译全部图，从头模拟并生成动画 |
| 停止／取消 | 停止当前操作，丢弃尚未导出的采样 |
| 定位结果 | 定位当前会话生成的动画资产 |

采样期间保持烘焙面板打开。关闭面板会取消任务；修改源动画或重新编译参与的图也会使当前采样失效，需要重新开始。

## 多部件角色：身体与衣物同步烘焙

每个部件使用独立的 Skeletal Mesh 和烘焙蓝图。先按上面的流程为衣物创建蓝图，例如 `ABP_KawaiiBake_Clothing`，再在主蓝图的 **Parts** 中添加：

| 字段 | 示例 | 说明 |
| --- | --- | --- |
| Name | `Clothing` | 非空且唯一；`Main` 是保留名称 |
| Mesh | 衣物 Mesh | 可以与主角色使用不同 Skeleton |
| Graph | `ABP_KawaiiBake_Clothing` | 必须是匹配此 Mesh Skeleton 的烘焙蓝图 |
| Animation | 留空 | 留空时复制源部件姿态；指定时播放此部件自己的动画 |
| Copy Pose From | `Main` | Animation 留空时生效，只能引用 Main 或列表中更早的部件 |
| Relative Transform | Identity | 部件在预览场景中的相对摆放，按实际装配关系设置 |

一次会话的顺序是 `Main → Parts[0] → Parts[1] → …`。复制姿态时读取源部件本步已经完成的最终姿态，按骨骼名称匹配，并换算组件空间；没有匹配的骨骼保留目标 Mesh 的参考局部姿态，再交给该部件的动画图处理。两者至少需要一个同名骨骼，不支持前向引用或循环依赖。

如果为部件填写 **Animation**，它必须与该部件 Mesh 使用同一 Skeleton、为非叠加动画，并与主动画具有相同长度。整个会话的采样率、预热、输出和部件列表由主蓝图控制；作为 Part 使用时，衣物蓝图面板中的 Source Animation 不负责提供输入。

完成后会生成 `Main` 和每个 Part 各一条动画，使用相同组 ID、采样率和帧数。运行时让各部件播放同一组结果并保持时间同步。

### 可选：衣物对 Main 的碰撞代理

主面板的 **Clothing Collision** 可为 Main 创建项目已有的衣物碰撞桥：

1. 开启 **Enable Clothing Collision**。
2. 将 **Collision Source Part** 设为已存在的部件名称，默认 `Clothing`，不能填 `Main`。
3. 指定 `KawaiiPhysicsClothingCollisionProfile`，配置其胶囊及 **Target Root Bones**；根骨骼列表为空时没有消费节点。
4. 将 **Warm Up Frames** 设为至少 `2`。

此处接收碰撞的始终是 Main。动态胶囊来自衣物完成后的骨骼快照，最早下一步供 Main 使用，适合头发与衣物等交互；它不是把所有 Mesh 自动变成三角网格碰撞体。普通共享碰撞同样需要在 KawaiiPhysics 节点配置发布、接收和分组，并让依赖源排在消费部件前面。

## 烘焙结果与限制

- 输出记录各 Mesh 的真实骨骼最终局部变换和浮点曲线，不复制 Anim Notify、同步标记或 typed attributes；这些内容需按用途另行处理。
- 保留局部根骨骼运动。预览 Actor 固定在原地，不模拟角色移动逻辑或提取组件位移；输出继承相应源动画的 `Enable Root Motion`，并关闭 `Force Root Lock`。
- 时长按采样帧率向上对齐，末帧读取源动画的准确终点，因此结果可能比源动画长不到一帧。
- 源动画时长必须大于 `0` 且不超过 `600` 秒。全部部件的总骨骼数 × 采样关键帧数不能超过 `4,000,000`。
- 模拟在独立预览世界中进行，禁用 Mesh 的 Post Process Anim Blueprint。需要烘焙的处理应放入烘焙图。
- 不支持 World Collision、场景 Wind、自定义 Blueprint 外力或启用的 Wind 外力。其他启用的外力要求 `Random Force Scale Range` 两端相等且有限，以保证重复采样。
- 骨骼和部件变换必须有效，缩放必须为正且非零。
- 只有完成全部采样和前置校验后才创建结果。若导出构建或回读校验失败，新资产可能保留为未保存状态；若磁盘保存失败，可能只有部分结果已写盘，请依据面板错误检查并手动保存。

播放已烘焙的物理骨链时，通常应关闭同一骨链上的实时 KawaiiPhysics，避免再次叠加物理模拟。

## 物理校准：生成可复用的实时配置

### 操作流程

1. 打开 **Tools → Kawaii 物理校准**，或点击衣物 AnimBP 工具栏的 **物理校准**。从工具栏打开时会自动填入该蓝图及其 Preview Mesh。
2. 设置 **人物骨骼网格体、衣物骨骼网格体、衣物相对变换、衣物动画蓝图**。用 **物理根骨骼** 唯一选中一个 KawaiiPhysics 节点；`None` 只适用于图中仅有一个节点的情况。节点在 Linked AnimBP 中时，选择实际包含节点的蓝图。
3. 点击 **读取碰撞与骨链**，检查并调整碰撞形状及骨链活动范围。导入优先读取所选节点及其 Limits Data Asset 的胶囊和外侧球体；没有可用形状时再读取人物 Physics Asset。此按钮会重建编辑列表，覆盖面板中已有的手动调整。
4. 将人物动画序列拖入下方测试动作区域，可多选追加。动画必须与人物 Skeleton 一致且非叠加。列表按顺序播放；关闭 **播放前重置** 可测试动作衔接，过渡时长默认 `0.2` 秒。也可设置压力测试骨骼及局部偏移，模拟身体 IK 带来的姿态变化。
5. 至少保留一个参与搜索的动作。建议另外添加独立片段，同时勾选 **仅用于验证** 和 **播放前重置**；这些片段不参与 GPU 筛选，只参与最终 CPU 验收。
6. 点击 **开始 GPU 校准**，等待动作采样、GPU 筛选和原始 Kawaii CPU 模拟复验完成。
7. 验收通过后，点击 **保存配置**，再点 **应用到蓝图**。应用会绑定所选节点的校准配置并编译蓝图，可撤销；检查效果后需自行保存蓝图。如果源节点在校准后发生变化，需要重新校准。

校准只读取所选 Kawaii 节点的原始设置，不执行完整衣物 AnimGraph；旧校准配置仅在临时副本中忽略，以避免倍率叠加。生成配置后，游戏内仍由 CPU 执行 KawaiiPhysics。

### 常用参数

| 参数 | 默认值 | 范围 / 说明 |
| --- | --- | --- |
| 采样帧率 | `60` | `30–120` |
| 预热帧数 | `30` | `0–240`，预热不参与评分 |
| GPU 候选数量 | `128` | `8–1024`，包含基准配置 |
| CPU 复验候选数量 | `4` | `1–16`，基准配置额外验证 |
| 随机种子 | `1337` | 相同输入和种子产生相同候选组合 |
| 阻尼 / 刚度倍率范围 | `0.5–2` | 相对于原节点参数，合法范围 `0.1–4` |
| 骨骼碰撞半径倍率范围 | `0.75–1.5` | 合法范围 `0.25–4` |
| 候选碰撞迭代上限 | `4` | `1–8`，越高通常运行时开销越大 |
| 柔性缓冲距离 / 响应速度 | `0.5 cm` / `12` | 响应速度为 `0` 时仅保留硬碰撞 |
| 蒙皮顶点采样数 | `512` | `0–512`；设为 0 时关闭蒙皮接触修正，仅验收骨骼代理 |
| 最大允许穿透 / 骨长误差 | 各 `0.5 cm` | 训练和验证均须通过 |
| 输出目录 | `/Game/KawaiiCalibration` | 必须是有效的 `/Game/` 子目录 |

### 保存、手动调整及限制

- **保存测试集** 创建 `DA_CalibrationTestSet_<id>`。在内容浏览器选中测试集，再点击 **载入所选测试集** 载入工作副本；再次保存会创建新资产。
- **保存配置** 创建 `DA_KawaiiCalibration_<id>`。完成 CPU 验收后，诊断报告写入项目 `Saved/KawaiiCalibration/<guid>/report.json`；验收未通过也保留报告，但不生成可应用结果。
- 不进行搜索时，可调整碰撞与骨链后点击 **保存手动配置（未验收） → 应用到蓝图**。此方式不需要测试动作，保留原节点阻尼、刚度和骨骼半径，不生成蒙皮接触点，也不代表通过穿模验收。再次调整后需重新保存手动配置。
- GPU 校准需要实际图形接口和 **SM5 计算着色器**，不支持 `NullRHI`，不会自动退回 CPU 筛选。
- 待校准节点应使用 **Component Space** 和显式重力，关闭世界碰撞、风力、外力数组、共享碰撞发布/接收、同步骨骼、旧版重力和项目默认重力，简单外力设为零。可用蓝图副本准备符合这些条件的节点。
- 身体碰撞需要 `1–32` 个启用的球体或胶囊，不支持插槽。形状编辑模式下全部禁用不会恢复自动导入。
- 若提示找不到受模拟根影响的可读取蒙皮顶点，检查所选根骨骼及衣物蒙皮；也可将蒙皮顶点采样数设为 `0`，但此时验收仅覆盖骨骼代理。
- 删除或禁用碰撞形状时，保留 **替换原碰撞的骨骼** 列表可避免原碰撞重新生效；清空该列表表示叠加原碰撞。修改骨链范围时须勾选相应覆盖开关；摆动角度 `0` 表示不限制角度。
- 配置默认从最近挂接的父级 Skeletal Mesh 读取人物姿态。人物应先完成动画和 IK，再更新衣物；碰撞来源父组件不能使用 Leader Pose。

碰撞代理及采样验收不能保证三角网格完全无穿模；仍需在实际角色和未参与搜索的动作中检查效果。

## 运行时：烘焙衣物的 IK 修正

当身体经过足部 IK 等处理后，烘焙衣物可能与身体错位。可在普通运行时 AnimBP 中使用 **Baked Clothing IK Correction** 节点（分类 `Kawaii Physics | Runtime Correction`）。

### 创建 Profile 并接入动画图

1. 创建普通 **Data Asset**，类型选择 `KawaiiPhysicsIKCorrectionProfile`。
2. 在 **Chains** 中配置 `Root Bone`、`Tip Bone` 和 `Driver Bone`。例如衣物链从裙摆根到末端，Driver Bone 选对应大腿。Root 必须有父骨骼，Tip 必须是其后代；各链根子树不能重叠，Driver 必须位于所有修正子树之外。
3. 在 **Capsules** 中配置身体胶囊的起止骨骼、骨骼局部偏移和半径。`End Bone` 留空时使用 Start Bone，半径必须大于 `0`。
4. 添加节点并指定 Profile，按下面的关系接入。这两路都是局部空间 Pose，不需要为此节点添加 Local/Component 转换。

```text
烘焙动画姿态 ───────────────→ Baked Pose
                              Baked Clothing IK Correction → Output Pose
身体最终 IK 后的姿态 ───────→ Driver Pose
```

两路输入必须对应同一动画时刻，且都属于当前 Mesh 的 Skeleton。节点保留 Driver Pose 的身体姿态，将指定衣物子树的烘焙局部动作放到当前身体锚点下，再进行链条与胶囊修正；侧枝和 Tip 之后的后代保留烘焙局部动作。

| 参数 | 默认值 | 说明 |
| --- | --- | --- |
| Driver Weight | `0.65` | `0–1`，附加身体驱动影响由根部的 0 增长到末端设定值 |
| 链 Collision Radius | `1 cm` | 骨链碰撞半径，允许为 0 |
| 胶囊 Radius | `5 cm` | 必须大于 0 |
| Iterations | `16` | `1–64` |
| Alpha | `1` | `0–1`，减小会削弱碰撞修正；0 仍保留当前锚点下的烘焙衣物，并非直接透传 Driver Pose |
| Use Attached Parent For Collision | `false` | 默认从 Driver Pose 读取胶囊骨骼；启用后读取最近挂接的父级 Skeletal Mesh |

分体衣物启用父级碰撞时，确保身体先完成更新，例如为衣物组件设置 `AddTickPrerequisiteComponent(Body)`；父组件不能使用 Leader Pose。此开关只改变胶囊来源，两路 Pose 仍必须属于衣物 Mesh 的 Skeleton，需要在衣物图中准备匹配的驱动姿态。

最多支持 `64` 条链和 `64` 个胶囊，当前 LOD 必须保留所需骨骼。缺少 Profile、缺骨或链配置无效时，节点回退到完整 Driver Pose；可在动画调试信息中查看原因、剩余穿透和骨长误差。这是骨骼链与胶囊的近似修正，不是完整布料模拟。

## 常见问题

| 现象 / 提示 | 处理方式 |
| --- | --- |
| 看不到 物理烘焙 按钮 | 通过 Tools 菜单创建专用烘焙蓝图；普通 AnimBP 不会显示此按钮 |
| `Select a Main Mesh and Source Animation` | 在面板补齐 Main Mesh 和 Source Animation |
| `uncontrolled animation player` / `exactly one connected Bake Source Pose` | 删除普通播放器，确保只有一个 Bake Source Pose 且实际参与输出求值 |
| `Bake Source Pose was not evaluated` | 检查断线、混合权重或绕开源节点的分支 |
| Skeleton 或动画长度不匹配 | 直接播放要求 Skeleton 相同；独立部件动画还必须与主动画等长；不同 Skeleton 的部件可使用 Copy Pose From |
| `translation retargeting would discard that motion` | 报错骨骼的平移已被图修改，但 Skeleton 模式会丢弃它；在目标烘焙 Skeleton 中将相关骨骼的 Translation Retargeting 设为 Animation，或取消该平移变化后重新烘焙。工具不会自动修改 Skeleton |
| `shared collision registry is unavailable` | 启用插件后完整重启编辑器 |
| Output Directory 无效 | 使用 `/Game/KawaiiBakes` 一类已注册的内容路径；不能填 `D:\...` 或 Engine 内容目录 |
| 动画开始时物理未稳定 | 增加 Warm Up Frames，并重新预览；它会在动作起始姿态上预热 |
| 预览耗时与动画时长不同 | 预览按采样率控制播放节奏，但每个编辑器帧最多推进一个固定模拟步，以保持共享碰撞时序；编辑器帧率低于采样率时会放慢。首次跳到未缓存位置还需要预热和顺序补算 |
