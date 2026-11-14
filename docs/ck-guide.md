# 用 Creation Kit 制作 OutpostBackup.esp

这个 esp 只包含 3 样东西：菜单消息、数据板道具、发放道具的任务。脚本已经编译好并放在 `Data\Scripts`（源码在 `Data\Scripts\Source`），CK 能直接看到。

做一次即可，做完把 `OutpostBackup.esp` 交给我检查。

## 0. 打开 CK

1. 运行 `CreationKit.exe`。
2. `File > Data...`，只勾选 **Starfield.esm**（不要勾选其他 MOD），**不要**设置 Active File，点 **OK**。等加载完成（几分钟）。

## 1. 菜单消息 `OSB_MenuMsg`

1. Object Window 左侧树：**Miscellaneous > Message**。
2. 右侧列表空白处右键 **New**。
3. 填写：
   - **ID**：`OSB_MenuMsg`
   - **Title**：`Outpost Backup`
   - **Message Text**：`What do you want to do?`
   - 勾选 **Message Box**
4. 在 **Menu Buttons** 区域，按顺序添加 4 个按钮（右键 New 或 Add），按钮文字：
   1. `Back up this outpost`
   2. `Land at the saved outpost`
   3. `Restore the saved outpost here`
   4. `Cancel`
5. **OK** 保存。

> 按钮顺序必须一致：脚本按 0、1、2、3 判断。

## 2. 数据板道具 `OSB_Slate`

1. Object Window：**Items > Book**。
2. 在过滤框输入 `Slate`，找一个普通的数据板（名字里有 Slate/Note、没有任务相关字样的都可以）。
3. 右键它 **Duplicate**，会生成一个 `...DUPLICATE001` 的副本。打开**副本**：
   - **ID**：`OSB_Slate`
   - **Name**：`Outpost Backup Slate`
   - **Book Text**（如果有）：`Read this slate to back up, land at, or restore an outpost.`
   - 如果有 **Teaches** / 任务相关的选项，设为无。
4. 在 **Scripts** 区域：
   1. 如果副本已经带有原物品的脚本，选中后 **Remove**。
   2. **Add**，选择 `OSB_Slate`，OK。
   3. 双击 `OSB_Slate`（或点 **Properties**），选中属性 `OSB_MenuMsg`，**Edit Value** 选 `OSB_MenuMsg`，OK。
5. **OK** 保存。

## 3. 发放道具的任务 `OSB_GiveSlateQuest`

1. Object Window：**Character > Quest**。
2. 右键 **New**：
   - **ID**：`OSB_GiveSlateQuest`
   - **Quest Name**：`Outpost Backup Setup`
   - 勾选 **Start Game Enabled**
   - 勾选 **Run Once**
3. **OK** 保存一次（第一次保存后才能挂脚本）。
4. 重新打开这个任务，切到 **Scripts** 标签：
   1. **Add**，选择 `OSB_GiveSlate`。
   2. 打开它的 **Properties**，`OSB_SlateItem` 设为 `OSB_Slate`。
5. **OK** 保存。

## 4. 保存

1. `File > Save`，文件名填 **`OutpostBackup.esp`**。
2. 如果弹出关于 ESL / 小型插件的选项，选 **是/标记为 Small（ESL）**；没有弹出也没关系。
3. 关闭 CK。

完成后告诉我，我会检查 esp 里的记录，再一起在游戏里测试。

## 出问题时

- 找不到某个菜单或字段：截图发给我。
- 脚本列表里找不到 `OSB_Slate`、`OSB_GiveSlate`：确认 `Data\Scripts\OSB_Slate.pex` 存在，重启 CK。
