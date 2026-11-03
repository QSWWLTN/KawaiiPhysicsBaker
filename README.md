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

点击 **烘焙新动画**，等待状态显示已保存同步结果，然后点击 **定位结果** 在内容浏览器中选中结果。

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

新代理可以独立用于预览，无需维护旧碰撞配置：

1. 在衣物附加部件指定 **衣物代理定义**。
2. 开启 **预览衣物代理碰撞（仅预览）**，将 **碰撞来源部件** 填为该部件名称，例如 `Base`。
3. **预览接收骨链** 留空会自动选择 Main 已编译的全部 Kawaii 节点。衣物代理定义资产中的 **忽略目标骨链** 优先于该列表，按节点主根名精确排除整个 Kawaii 节点。若只想预览少数接收节点，也可填写接收列表，如 `Hair_back`、`Hair_side_L`、`Hair_side_R`。
4. 修改烘焙面板的预览设置会自动重新预热、重建缓存。编辑衣物代理资产的忽略规则会让现有预览缓存失效，点击 **编译并预览** 重新生成即可。状态栏显示代理来源、碰撞体数量、过滤后接收骨链数量及是否影响导出；全部目标被忽略时会明确提示没有接收骨链。

**忽略目标骨链是衣物代理本身的规则，同时用于预览和运行时。** 在 `DA_BaseClothing_ClothingProxy` 等定义资产中填写 `Tail_Root`，这套代理就不再对根名为 `Tail_Root` 的目标骨链产生碰撞。预览匹配 Main 的 Kawaii 节点主根名，运行时匹配 `Baked Clothing IK Correction` 中配置的链根名；不按衣物代理端点骨骼筛选，也不自动匹配子树。耳朵等同样填写其实际消费节点／骨链的根名。规则只排除这套代理的接触，保留目标自身物理、身体碰撞及其他衣物的碰撞；旧 `ClothingCollisionProfile` 是独立配置。

规则保存在定义资产中，不写死进每个动作的几何快照。修改后**无需重新烘焙代理运动**：运行时下一次采样读取新规则，并通过值类型胶囊传给动画线程。自定义胶囊消费者也需要按 `IgnoredTargetRootBones` 过滤；插件自带 IK 节点已执行该过滤。名单可包含其他角色才有的根名（当前无匹配时不影响其他骨链），最多 64 项且不能包含 `None`。

这是实时物理碰撞预览：从来源部件上一完成采样帧的骨骼构建新定义中的球体／胶囊，再供 Main 的 KawaiiPhysics 模拟使用。临时预览至少预热 2 帧，不修改资产中的预热帧数。它可用于检查代理覆盖和物理响应，**不等同于运行时代理轨道采样 + IKCorrection 的同帧修正结果**。开启后可能仍有近似形状、约束冲突或一帧延迟造成的残余穿透。

**烘焙新动画** 会重新创建场景，忽略此预览开关及其缓存，代理运动仍正常导出。若需要可换装的通用头发烘焙，保持 **启用旧配置碰撞（写入烘焙）** 关闭；该旧开关独立存在，打开时仍会将衣物影响写入头发动画。新代理预览和旧开关同时打开时，预览使用新定义，导出使用旧配置，状态栏会提示这一区别。

旧配置碰撞的使用方式：

1. 开启 **启用旧配置碰撞（写入烘焙）**。
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

## 衣物代理：自定义运动资产与烘焙头发修正

此功能全部位于 KawaiiPhysicsBaker 插件。衣物代理运动使用独立的 Data Asset 保存**代理端点所需的最小骨骼层级、局部变换轨道和局部形状定义**，运行时不解码衣物 AnimSequence，也不读取衣物组件的上一帧姿态。衣物显示仍可使用同次烘焙输出的动画。

### 创建与编辑衣物代理

1. 打开 **Tools → Kawaii 衣物代理**。也可以先在内容浏览器选择衣物 Mesh 或已有代理定义，再打开面板。
2. 选择衣物网格体，点击 **创建代理定义**，保存 `Kawaii 衣物代理` 资产（默认目录 `/Game/KawaiiProxies`）。已有资产可直接从 **代理定义** 选择器载入。
3. 选择起点骨骼并点击 **添加代理**。默认连接该骨骼与第一个子骨骼；没有子骨骼时为球体。右侧属性可以修改唯一名称、两端骨骼、局部偏移和半径；两端重合即为球体。最大支持 64 个代理，绑定真实骨骼，不支持虚拟骨骼或插槽。
4. 左侧实时显示衣物参考姿态和代理线框，可旋转、缩放并使用 **聚焦衣物** 检查贴合情况。当前通过属性数值编辑，不提供拖动变换控件或动作预览。
5. 如果衣物有 PhysicsAsset，可以点击 **追加 PhysicsAsset 代理** 导入其球体和胶囊；不修改原 PhysicsAsset，不导入盒体或凸包，导入后仍需检查贴合情况。
6. 点击 **校验并保存**。修改直接作用于所选定义资产，并支持撤销／重做；修改网格、形状或骨骼绑定后，需要重烘对应运动资产。

### 同步导出代理运动与动作集合

在烘焙面板的 Main 或对应衣物 Part 上指定 **衣物代理定义**，定义的网格必须与该部件相同。**代理动作集合** 可选择该衣物之前生成的集合；第一次可以留空。未选代理定义时保持原有动画烘焙行为。

除原有 AnimSequence 外，每个选定部件还会输出：

| 资产 | 内容 |
| --- | --- |
| `…_ProxyMotion` / `Kawaii 衣物代理运动` | 代理形状快照、最小父子层级、逐帧局部变换、源动作身份、对应衣物动画、采样信息及生成组 |
| `…_ProxySet` / `Kawaii 衣物代理动作集合` | 同一衣物的源动画到代理运动资产映射；已有集合按源动画替换条目，旧动画及旧运动资产不会被覆盖 |

集合保存的是运行时可读取的引用，不依赖编辑器包元数据。首次生成集合并成功保存后，工具会将它回填到对应部件的烘焙设置，记得保存烘焙蓝图。此后烘焙新动作会追加映射，重烘同一动作会替换映射。集合不能混用不同衣物网格、代理定义或装配变换。所有生成结果通过校验后才更新集合；磁盘保存失败会明确报告，可能留下部分已保存结果和待保存的集合。

时间轴与衣物输出动画一致：采样点为 `i / SampleRate`，最后一个采样点保存源动作准确终点，输出时长向上对齐到整帧。正式轨道不包含预热。动画轨道和代理轨道合计沿用 400 万骨骼样本上限。

### 运行时选择、采样与头发修正

蓝图接口在 **Kawaii Physics → Baked Clothing Proxy** 分类中：

1. 身体实际播放器提供本帧的 `FKawaiiClothingProxyActionSample` 数组：`SourceAnimation`、该源序列内的秒数 `Time`、非负 `Weight`。Montage 必须先换算当前动画片段内时间；该接口不会根据身体最终 Pose 自动推断源动作，也不会自动订阅 Montage 或状态机。
2. 调用 **Resolve Clothing Proxy Actions**，传入当前衣物的 `Kawaii 衣物代理动作集合`，得到本帧对应的代理运动采样数组。缺少映射或源动作重复时返回失败并清空结果。输出条目的 `Motion.BakedAnimation` 是对应衣物显示动画的软引用，应在进入动作前加载；不要每帧同步加载。
3. 调用 **Sample Blended Clothing Proxy Motion**；单动作也可调用 **Sample Clothing Proxy Motion**。`ProxyToTarget` 是衣物 Mesh 组件空间到头发所在 Mesh 组件空间的变换，必须由调用方提供。资产的 `RelativeTransform` 仅记录烘焙时摆放，不会再隐式叠加；按原装配关系且目标为 Main 时，可直接使用该记录值。
4. 将输出值数组传入 **Baked Clothing IK Correction → Clothing Proxy Capsules**。为头发骨链创建 `KawaiiPhysicsIKCorrectionProfile`：Chains 填头发 Root/Tip，Driver 选择修正子树以外的头部骨骼；只需要碰撞修正时可将 Driver Weight 设为 0。`Baked Pose` 输入同一时刻的烘焙头发姿态，`Driver Pose` 输入身体当前姿态，两者仍须属于头发所在 Mesh 的 Skeleton。

采样函数在游戏线程调用，动画工作线程只接收已经变换到目标组件空间的胶囊值数组。每帧更新数组；采样失败时传入空数组并处理返回的 Error，避免继续使用上一次动作的碰撞。Alpha 建议为 1，低于 1 会削弱最终接触修正。原有 Profile 身体胶囊与外部衣物代理可以同时参与求解。

采样器会先在局部骨骼空间进行插值／混合，再重建父子层级及碰撞形状。正权重动作必须使用相同定义、骨骼层级、衣物网格和装配变换；全零权重没有可采样姿态，返回失败。时间超出片段时钳制到首末帧，不自行循环或推进播放时钟。代理形状变化后，旧运动资产会被拒绝。

### 一致性与当前边界

- 支持非叠加动作使用统一骨骼权重的局部姿态混合。任意分层遮罩、Mesh Space 混合、Inertialization、Control Rig 和额外衣物 IK 不会被自动重现；衣物显示与代理必须使用一致的姿态计算规则。
- 记录的局部根运动原样保留。若衣物显示启用根锁定或提取 Root Motion，调用方须保证代理使用同样的根处理，不能只靠组件变换抵消不一致的骨骼轨道。
- 不包含 Chaos 布料、Morph、材质顶点位移和网格精确接触。球体／胶囊是近似形状；高领等无法通过有限骨链位移避让的接触仍可能有残余穿透。
- **Validate Proxy Motion** 做完整轨道校验，并在源资源已加载时检查参考骨架签名及编辑器源动画版本。适合装配／加载时或编辑器验收时调用，不应每帧执行。常规采样只检查结构、定义及实际访问的轨道，且不会加载软引用。
- 本功能提供资产、映射和运行时接口；项目角色 AnimBP 的动作采样发布与衣物／头发接线需要按上述关系接入。不会自动改写现有角色蓝图。

## 性能探针与耗时采集

运行时代理、头发／衣物骨链修正及烘焙导出已加入静态 CPU Trace 计时区间。使用 Unreal Insights 的 **Timing Insights**，筛选 `KawaiiBaker_`，在选定时间段查看调用次数、平均／最大耗时、Inclusive 和 Exclusive 时间。游戏线程的代理采样与动画工作线程的修正分别记录，不应只看 GameThread。

| 计时区间 | 测量范围 |
| --- | --- |
| `KawaiiBaker_Proxy_ResolveActions` | 身体源动作到当前衣物代理运动的映射查找 |
| `KawaiiBaker_Proxy_SampleBlended` | 整次代理采样与混合；子项拆分为 `ValidateHeader`、`SampleLocalPose`、`BlendLocalPoses`、`BuildHierarchy`、`BuildCapsules` |
| `KawaiiBaker_Proxy_ValidateAsset` | 完整资产校验，单独区分装配／验收成本与每帧采样成本 |
| `KawaiiBaker_Proxy_DefinitionSignature` / `SkeletonSignature` | 定义及参考骨架签名计算 |
| `KawaiiBaker_Correction_PreUpdate` | 游戏线程上的配置和父级碰撞快照 |
| `KawaiiBaker_Correction_Update` / `Evaluate` | 动画节点更新／求值；包括上游 Pose 的开销，分别以 `InputDriverPose`、`InputBakedPose` 子区间标出 |
| `KawaiiBaker_Correction_SolveChain` | 单条骨链的约束求解；`MeasureResiduals` 单独记录残余穿透及骨长诊断 |
| `KawaiiBaker_Correction_ResolveChains` / `PrepareCapsules` / `SolveAndReconstruct` / `WriteOutputPose` | 骨链解析、碰撞准备、求解及变换重建、最终姿态写回 |
| `KawaiiBaker_Bake_*` | 准备、各部件求值、轨道采集、代理数据构建、动画生成与回读校验、映射更新、各资产保存 |
| `KawaiiBaker_PreviewProxy_BuildProfile` / `EvaluateParts` | 创建临时预览碰撞配置／启用新代理碰撞时的整次部件求值；后者包含全部部件模拟，不是纯碰撞增量成本 |

总区间包含子区间，**不要将父子 Inclusive 时间相加**。多工作线程耗时相加是 CPU 工作量，不等于整帧墙钟时间；以时间轴判断并行及阻塞。烘焙会在开始、完成、失败和取消时生成 `KawaiiBaker` 书签，便于定位一次操作；不输出逐帧日志。

在编辑器或 Development 游戏的控制台中可以直接开启统计面板：

```text
stat KawaiiPhysicsBaker
```

面板显示动作映射、代理采样、完整校验、快照、节点求值、骨链求解的 CPU 周期统计，以及每帧求解调用数、骨链点数和实际迭代数。计数累计所有实例；节点求值仍包含输入 Pose 开销，求解是其子项。

完整采集示例（开始前确保没有正在写入的其他 Trace，并使用新的输出文件名）：

```text
Trace.File D:/UEProject/RPGProject/Saved/Profiling/KawaiiBaker/Attack.utrace cpu,counters,frame,bookmark
Trace.Bookmark KawaiiAttackBegin
```

执行攻击／落地或烘焙操作，结束后输入：

```text
Trace.Bookmark KawaiiAttackEnd
Trace.Stop
```

也可启动时传入 `-trace=cpu,counters,frame,bookmark -tracefile=<绝对路径>.utrace`。采集后用同版本 Unreal Insights 打开文件。Counters 面板的 `KawaiiBaker/Proxy/InputSamples`、`ActiveSamples`、`Bones`、`Capsules` 表示**最后一次游戏线程采样调用**的负载，不是所有角色之和；`KawaiiBaker/Correction/*LastSnapshot` 是最后一个节点的配置快照。同帧多角色会更新这些共享 gauges，应结合对应 CPU 事件位置读取。

需要进一步拆分求解器内部时，开启：

```text
a.KawaiiPhysicsBaker.ProfileSolverDetails 1
```

这会记录每次迭代的 `LengthConstraints`、`PointContacts`、`SegmentContacts`、`FinalPointContacts`，默认值为 `0`。详细探针本身有开销，定位后设回 `0` 再测整体预算。未启用对应 Trace 通道时不会写入事件；Shipping 是否保留探针取决于引擎的 Trace／Stats 编译选项。以上探针测 CPU 耗时，不代表 GPU 渲染或布料求解耗时。

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
