# Starfield 哨站备份与恢复 MOD：可行性探索记录

> 本文记录了与 Claude Code 的讨论与测试过程，以及最终结论。

## 总结

| 项目 | 结论 |
|---|---|
| 目标 | 备份哨站，并在**同一位置**原地恢复；支持跨存档（方案 B：SFSE 外部文件） |
| 纯 Papyrus | ❌ 不可行：列不全哨站物件；`PlaceAtMe` 生成的物件不可见、也不属于哨站，且没有函数能把物件挂回哨站 |
| SFSE C++ | ⚠️ 理论可行，但需要逆向 `Starfield.exe` 1.16.244 的哨站系统（物件归属判断、注册到哨站的内部函数）；CommonLibSF 只支持到 1.14.74，且很可能没有覆盖哨站系统 |
| 推荐下一步 | 在 Nexus Mods 搜索 “outpost blueprint / outpost transfer / outpost save”，优先使用或参考现成 MOD |

### 环境信息
- 游戏版本：**1.16.244.0**（Steam）
- 已安装：Creation Kit、SFSE（`sfse_loader.exe`）、Address Library（含 `versionlib-1-16-244-0.bin`）
- SFSE 官方已支持 1.16.244；SFSE 本身不提供注册 Papyrus 原生函数的接口，需要 CommonLibSF
- CommonLibSF 最后更新 2025-09，`RUNTIME_LATEST = 1.14.74`；其 Address Library 加载逻辑会读取 Steam 版的 `versionlib-1-16-244-0.bin`，所以地址库本身不是问题，风险在内部结构是否变化

### 关键测试结果（`papyrus/OSB_Test.psc`）
测试哨站：信标 + 4 个建造物。

| 方法 | 找到 | 说明 |
|---|---|---|
| `GetWorkshopResourceObjects(None, 0)` | 1 | 仅资源类物件（储物箱 `FF0AB60D`） |
| `GetWorkshopOwnedObjects(Player)` | 2 | 储物箱 `FF0AB60D` + 一个 ActorBase 物件 `FF0AB985` |
| `GetRefsLinkedToMe(None)` | 0 | `None` |
| `GetDestructibleOutpostObjects()` | 1 | 同一个储物箱 |
| **去重合计** | **2 / 4** | 有 2 个物件任何方法都找不到 |

- 信标的 `GetWorkshop()` 返回它自己，说明信标就是哨站对象。
- 控制台里点到的 `PKIN`（Pack-In）是预组装外壳，实际物件是紧随其后的另一个引用。
- `PlaceTest`（复制 CONT 储物箱或 PKIN）：生成了新引用，但 `workshop=None`、`inBuildableArea=false`，游戏中**不可见**。

### 可用的 Papyrus 接口（来自原版源码）
- `ObjectReference.GetWorkshop()`、`GetWorkshopOwnedObjects(Actor)`、`GetWorkshopResourceObjects(ActorValue, int)`、`GetDestructibleOutpostObjects()`、`IsWithinBuildableArea(ObjectReference)`
- `FindAllReferencesOfType(Form/FormList, radius)`、`FindAllReferencesWithKeyword(...)`
- `Game.GetForm(int)`、`Game.GetFormFromFile(int, string)`
- 事件：`OnWorkshopObjectPlaced` / `OnWorkshopObjectRemoved` / `OnWorkshopObjectMoved`
- 信标脚本：`OutpostBeaconScript`（不存在 FO4 式的 `WorkshopScript`）

### 测试残留清理
`PlaceTest` 在存档中留下了不可见空引用：`FF04A74C`、`FF04A751`、`FF0ABEF8`、`FF0ABEFB`。读取测试前存档，或在控制台执行 `prid <ID>` + `markfordelete`。不再需要时可删除 `Data\Scripts\OSB_Test.pex`。

### 若将来继续
1. 列全物件：CK 中建立包含所有可建造物件的 FormList，用 `FindAllReferencesOfType` 在信标周围搜索，再用 `GetWorkshop()` 过滤；或用 SFSE C++ 遍历单元格引用。
2. 挂回哨站：需要逆向游戏内部的建造/注册函数（SFSE C++），这是最难、最不确定的部分。
3. 关注 CommonLibSF 更新，或公开的哨站系统逆向成果。

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
11. **结论**：纯 Papyrus 不可行；SFSE C++ 需要逆向游戏程序，Claude 在云端无法完成，结果不确定。建议先寻找现成 MOD，或暂停，等待 CommonLibSF / 社区逆向成果。
