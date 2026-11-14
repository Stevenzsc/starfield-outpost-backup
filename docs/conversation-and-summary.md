# Starfield 哨站备份与恢复 MOD：可行性探索记录

> 本文记录了与 Claude Code 的讨论与测试过程，以及最终结论。
> 2026-11-14 更新：找到 `WorkshopItemKeyword` 链接方案，纯 Papyrus 原型已在游戏中完成“备份 → 拆光 → 恢复”全流程。
> 2026-11-14 再更新：逆向找到降落区经纬度与星图降落函数，SFSE 插件 OSBLanding 实现精确降落；**跨存档、同一星球同一位置恢复哨站已在游戏中验证成功**（位置、高度均正确）。

## 总结

| 项目 | 结论 |
|---|---|
| 目标 | 备份哨站，并在**同一位置**原地恢复；支持跨存档 |
| 纯 Papyrus | ✅ **可行（原型已验证）**：`GetRefsLinkedToMe(WorkshopItemKeyword)` 能列全哨站物件；`PlaceAtMe` + `SetLinkedRef(信标, WorkshopItemKeyword)` + `Enable()` 能把新物件挂回哨站 |
| 跨存档、同一位置 | ✅ **已验证**：备份时记录降落区中心经纬度（读游戏内存）；新存档用 OSBLanding 插件精确降落到同一中心，坐标系与原存档一致，用绝对坐标恢复 |
| SFSE C++ | 只用于精确降落（`plugin/`，GitHub Actions 编译） |

### 跨存档原位恢复流程（已验证）

1. **原存档**：站在哨站旁，`cgf "OSB.Backup" <信标ID>`，等提示；游戏不关，运行 `tools\make-restore.ps1 -Mode Absolute`（会调用 `sfgps.exe` 记下降落区中心经纬度、星球，写入 `OSB_Restore.json`）。
2. 安装 `OSBLanding.dll` 到 `Data\SFSE\Plugins`（用 `sfse_loader.exe` 启动）。
3. **新存档**：运行 `tools\arm-landing.ps1`（写 `OSB_LandTarget.txt` 和 `OSB_Guide.txt`）。到同一星球轨道，星图上随便点一个地方降落 → 插件把降落位置替换为原降落区中心（一次性，用后文件改名为 `.used.txt`）。
4. `bat OSB_Guide` 走到原信标位置，放信标；`bat OSB_Restore` 恢复。

### 逆向结果（游戏 1.16.244）

| 项目 | 位置 |
|---|---|
| `BGSPlanet::Manager`（静态对象） | Address Library ID **945580**（CommonLibSF 单例 ID 937609 指向它） |
| 当前星球 FormID | Manager `+0x80`（uint32） |
| 降落区中心经度 / 纬度 | Manager `+0x98` / `+0xA0`（double，弧度）；`+0xB0`、`+0x1A8` 各有一份副本 |
| 星球半径 | Manager `+0x100`（float，km） |
| `SetLandingSite` | ID **51740**；参数 r9 = 降落点结构，`+0` 经度、`+8` 纬度（弧度） |
| 调用方 | `LandAtLatLon` ID 51739（**读档时**恢复降落区）；**星图点选降落** ID 97897 `+0x1EB`；ID 97898 `+0x2D2`；`LandAtRef` ID 97899 `+0x170` |
| 起飞时复位 | ID 51746 |

- 局部坐标（`GetPositionX/Y/Z`）单位是米；朝向 0° = 北，顺时针；+Y 北、+X 东。
- 在**已探索降落区附近**降落时，游戏会沿用旧坐标系并把降落点挪开约 1.2 km，所以同一存档里测不出精确效果；在未探索的位置降落，降落中心与目标逐位一致。
- 相对模式以新信标为基准，新信标高度不同时物件整体偏高/偏低；精确降落后应使用绝对模式。
- 不可用的方法：硬件写断点（降落时游戏崩溃，写入线程对调试器隐藏）；在内存里按经纬度/方向向量搜索星图点选目标（不以这些形式存放）；读取 Papyrus 里飞船坐标（`GetPlayerHomeSpaceShip().GetPositionX()` 会卡住）。

### 工具

| 文件 | 用途 |
|---|---|
| `tools/memscan/sfgps.cs` | 从游戏内存读降落区中心经纬度、半径、星球（只读，经 Address Library 解析地址） |
| `tools/arm-landing.ps1` | 设置下一次星图降落的目标 |
| `tools/make-guide.ps1` | 同一降落区内换算旧信标位置（不需要插件时使用） |
| `plugin/` | OSBLanding SFSE 插件（CommonLibSF / xmake） |
| `tools/memscan/memscan.cs`、`vecscan.cs`、`hwbp.cs`、`tools/xref/` | 逆向用的内存扫描、断点、静态分析工具 |

### 原型用法

文件：`papyrus/OSB.psc`、`tools/make-restore.ps1`。不需要 esp。

1. 编译 `OSB.psc`（`tools/compile.ps1`；若复制到 `Program Files` 失败，用管理员 PowerShell 或手动复制 `.pex` 到 `Data\Scripts`）。
2. 游戏内控制台：`cgf "OSB.Backup" <信标 refID 的有符号十进制>`。关闭控制台，等提示 `OSB: backed up N objects`（游戏切到后台会暂停脚本）。
3. PowerShell：`powershell -ExecutionPolicy Bypass -File tools\make-restore.ps1`
   生成 `<Starfield>\OSB_Restore.txt`，并在 `backups\` 留一份带时间戳的副本。
4. 站在目标哨站范围内（信标已放好），控制台：`bat OSB_Restore`。

refID 换算：`FF` 开头的 ID 超出 32 位有符号整数，要填负数，例如 `FF044D75` → `-16495243`：
`[BitConverter]::ToInt32([BitConverter]::GetBytes([uint32]'0xFF044D75'),0)`

### 已验证（测试哨站：信标 + 大气水提取器 + 皮椅 + 1 个 MOD 物件 + 炮塔）

- `WorkshopItemKeyword` = `Starfield.esm` KYWD `00054BA6`（与 FO4 相同）。原版 `.psc` 源码里没有出现，是扫描 ESM 找到的。
- `beacon.GetRefsLinkedToMe(kw)` 返回 14 个引用，全部属于该信标；旧方法最多找到 2 个。
- 复制物件并链接后：可见、`GetWorkshop()` 返回信标、在建造范围内、可在建造模式选中 / 移动 / 拆除，拆除返还材料。
- 完整流程：6 个物件全部恢复（提取器、提取器地基、椅子、MOD 物件的 2 个引用、炮塔），无重复。

### 踩过的坑

- **坐标单位是米**。最初偏移 `+300.0` 把复制品放到 300 米外，看不见、也不在建造范围内。
- **`Form.GetFormID()` 会让脚本永久卡住**（对 `MapMarker_LongRange 00254244`、`00000021` 等基础物件）。现在备份脚本不调用它，只把 `[Form < (xxxxxxxx)>]` 字符串写进日志，由 `make-restore.ps1` 解析十六进制 ID。
- `FF` 开头的引用 ID 每次读档都可能变化，备份只存“基础物件 + 坐标 + 角度 + 缩放”。
- 炮塔是 `ActorBase`（`002E181C`），不能按“NPC 一律跳过”处理。
- 信标自带的地基与信标坐标完全重合，按坐标跳过。
- MOD 物件可能只以 PackIn 引用出现，PackIn 不能跳过。
- 游戏切到后台会暂停，Papyrus 也会停；第三方脚本卡住（日志里大量 `Stack potentially stalled`）会让备份变慢。

### 备份时跳过的内容

- 信标本身、`OutpostBeacon_Packin`、`MapMarker_LongRange`、`NPCClipboard`（信标组成部分）
- 与信标坐标重合的物件
- FormID < `0x800` 的引擎保留物件（在 `make-restore.ps1` 中过滤）

### 已知限制 / 待办

1. 跨存档：坐标在另一存档中是否一致，未验证。
2. MOD 物件的 FormID 含加载顺序前缀，加载顺序变化后会找不到（日志里会报 base form missing）。可改为存“插件名 + 本地 ID”。
3. 同一份备份执行两次会产生重复物件；恢复前需手动拆光。
4. 不恢复：电线 / 货物链接、容器内物品、指派的船员。
5. 大型哨站的性能未测。
6. `OSB.psc` 里还留着排查用的 `Backup step` 日志，稳定后可删。

### 环境信息
- 游戏版本：**1.16.244.0**（Steam）
- 已安装：Creation Kit（含 `PapyrusCompiler.exe`）、SFSE、Address Library
- CommonLibSF 停在 1.14.74；当前方案不依赖它

### 测试残留清理
早期 `PlaceTest` 在存档中留下了不可见引用：`FF04A74C`、`FF04A751`、`FF0ABEF8`、`FF0ABEFB`。读取测试前存档，或在控制台执行 `prid <ID>` + `markfordelete`。

---

## 对话过程（整理）

1. **初始问题**：能否做一个 Starfield 备份与恢复哨站的 MOD（不改代码，先评估）。
   - 评估：可行性中到高。路线为 CK + Papyrus + SFSE 插件（JSON 文件读写）。难点：地形差异、电线/货物链接无 API、游戏更新导致 SFSE 失效、大哨站性能、存档安全。
2. **缩小范围：只做同一位置的原地恢复。**
   - 重新评估：可行性提高；地形、坐标换算问题消失。方案 A（纯 Papyrus，同一存档）与方案 B（SFSE 外部文件，可跨存档）。电线链接仍建议第一版不做。
3. **工作量**：Claude 无法运行游戏/CK，代码需用户编译和测试；预估方案 A 3–7 天，方案 B 1–3 周。
4. **用户选择方案 B、手持物品触发。**
5. **编译方式**：用户没有 Visual Studio 2022，只有 VS Code；讨论了安装 Build Tools 或用 GitHub Actions 云端编译，最终选择云端编译，并建立仓库 `Stevenzsc/starfield-outpost-backup`。
6. **环境检查**：确认 `CreationKit.exe`、`sfse_loader.exe`、游戏版本 1.16.244.0；原版脚本源码位于 `Data\scripts\Source`（4743 个 `.psc`）；查找到 workshop / outpost 相关接口。
7. **依赖检查**：SFSE 支持 1.16.244；CommonLibSF 停在 1.14.74；Address Library 已有 1.16.244。决定先用纯 Papyrus 验证两大风险点，再决定是否写 C++。
8. **验证脚本**：编写 `papyrus/OSB_Test.psc` 与 `tools/compile.ps1`，用户成功编译（需 `Unblock-File` + `-ExecutionPolicy Bypass`），在游戏中用 `cgf` 调用。
9. **测试 1（旧哨站，复制 PKIN）**：只找到 1 个物件，复制品不可见、不属于哨站。
10. **测试 2（新哨站，4 物件，复制 CONT 储物箱）**：各方法去重后只找到 2/4 个物件；复制品仍不可见、不属于哨站。
11. **初步结论**：纯 Papyrus 不可行；SFSE C++ 需要逆向游戏程序。
12. **复查**：发现测试只用了 `GetRefsLinkedToMe(None)`，而 FO4 的工坊物件是用 `WorkshopItemKeyword` 链接的。扫描 `Starfield.esm` 找到该关键字 `00054BA6`。
13. **测试 3**：`GetRefsLinkedToMe(kw)` 找到 14 个引用，全部属于信标。复制品链接后 `workshop` 从 `None` 变为信标，但偏移 300 米导致不可见。
14. **测试 4**：偏移改为 3 米，复制品可见、可选中 / 移动 / 拆除，返还材料。**两大风险点均解决。**
15. **原型**：编写 `OSB.psc`（Backup / R / Done）与 `make-restore.ps1`（日志 → `bat` 文件）。排查并修复：`GetFormID()` 卡死、信标地基重复、MOD 物件与炮塔被漏掉。
16. **测试 5**：完整流程（备份 → 拆光 → `bat OSB_Restore`），6 个物件全部恢复，无重复。
17. **跨存档定位**：内存扫描找到降落区中心经纬度（Address Library ID 945580 = `BGSPlanet::Manager`），写出独立读取工具 `sfgps`，不再依赖 GPS MOD。
18. **精确降落**：星图点选精度不够（最近只能落在 59 km 外）；用 Iced 静态分析找到 `SetLandingSite`（ID 51740）及其调用方，写 SFSE 插件 OSBLanding 替换星图降落坐标，GitHub Actions 编译。
19. **测试 6**：在建哨站之前的旧存档里，星图随便点一处，插件把降落中心精确替换为原坐标；`bat OSB_Guide` 到达原信标位置（高差 0.64 m），放信标后用绝对模式 `bat OSB_Restore`，哨站完整恢复，位置和高度正确。
