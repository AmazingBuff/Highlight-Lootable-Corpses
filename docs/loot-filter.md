# HighlightLootableCorpses（原 CorpseESP）「有价值尸体筛选」功能方案与实现记录

> 日期：2026/08/18
> 状态：已实现，Release 构建通过（`/W4 /WX` 无告警）
> 涉及文件：`src/loot_filter.h/.cpp`（新增）、`corpse_finder`、`config`、`esp_renderer`、`ui_menu`、`src/CMakeLists.txt`

## 一、功能概述

新增"战利品筛选"：扫描时评估每个尸体的库存，命中任一"价值分类"的才显示边框；
未开启时完全保持现状。分类、阈值全部可配置（INI + 游戏内 MCP 菜单）。

## 二、分类体系：位掩码（`LootFilter::Category`）

| 分类 | 位 | 含义 |
|---|---|---|
| `e_quest` | 1<<0 | 任务物品 |
| `e_key` | 1<<1 | 钥匙 |
| `e_enchanted` | 1<<2 | 附魔装备 |
| `e_valuable` | 1<<3 | 单件价值 >= 阈值 |
| `e_book` | 1<<4 | 书籍（按模式限定） |
| `e_consumable` | 1<<5 | 消耗品（箭矢/炼金材料/灵魂石/药水/卷轴） |

位掩码的好处：尸体可同时命中多类；渲染端按"配置允许的掩码"做与运算，
**改配置立即生效**（无需等下一次扫描）。

## 三、物品判定规则（精确到 CommonLibSSE API）

判定原则：**用与搜刮界面（以及 QuickLoot IE）完全相同的路径取背包内容，再逐项
判断玩家能否拿取**。不手写模拟等级列表/装备来源，信任引擎。

**取库存**（`fetch_inventory_items`，fork 自 QuickLoot IE
`src/Items/Inventory.cpp` 的 `LoadContainerInventory`）：

1. `ref->GetInventoryChanges()`（无参 = noInit=false）：容器尚无库存档时触发
   引擎初始化/掷骰（与首次打开容器等效），outfit 装备、容器模板物品、等级列表
   由此全部 resolve 成实际物品写入 InventoryChanges——draugr 这类经 TPLT 模板
   继承装备（"Use Inventory" 标志）的 NPC 也在此路径覆盖；
2. actor 额外调用引擎函数 `RefreshEnchantedWeapons`（SE 50946 / AE 51823，
   fork 自 QuickLoot IE）把装备武器的附魔实例数据刷进库存档；
3. 三路合并到 `unordered_map<TESBoundObject*, InventoryEntryData>`：
   - **changes 条目**：引擎 resolve 后的实际物品（含 worn 装备、掷骰结果）；
   - **容器模板**（`GetContainer()->ForEachContainerObject`）：**跳过
     `TESLevItem`**（引擎尚未 resolve 的占位条目直接忽略，与搜刮界面一致）；
     与 changes 重叠的条目（非 leveled）count 叠加；
   - **掉落物**（`ExtraDroppedItemList`）：未删除/未禁用的掉落引用按其 count 计入；
4. 过滤 `countDelta <= 0`（被拿走的物品体现为负增量）。

**逐项判定**（evaluate）：

- form type 为 `LeveledItem`：引擎仍未 resolve 的占位条目 → 跳过（QuickLoot IE
  同款取舍：宁缺勿错）；
- `!GetPlayable()`：玩家不可拿取的残留物不算"还有货"；
- 其余按 `countDelta > 0` 计入 `has_items`，并累计分类/价值。

> **教训**：不要试图手写模拟引擎的库存语义——等级列表解析、outfit 物化
> （draugr 武器经 TPLT 模板链继承）都有引擎专属时序。手写模拟（noInit 只读合并、
> 静态展开 LVLI、沿模板链扫装备）每一版都在某类尸体上回归；最终以 QuickLoot IE
> 的"触发引擎初始化 + 只信引擎结果"方案收口。库存档缺失时
> `GetInventoryChanges()` 的兜底路径（`ForceInitInventoryChanges`）可能留下
> 未 resolve 的占位条目，此时跳过它们意味着漏报而非误报——可接受。

**分类/价值**从合并库存评估，供战利品筛选使用。

逐件分类规则（或关系，命中即置位）：

- **e_quest**：`entry->IsQuestObject()`（内部查 `HasQuestObjectAlias`，即任务别名"任务对象"标志，权威）
- **e_key**：`obj->GetFormType() == RE::FormType::KeyMaster`（`TESKey`）
- **e_enchanted**：`entry->IsEnchanted()`（覆盖基底 `TESEnchantableForm::formEnchanting` + 实例附魔）
- **e_valuable**：`value = item_value(...)`：金币堆（`obj->IsGold()`，FormID==0xF）按枚数计，其余 `entry->GetValue()`；`value >= high_value_threshold` 命中
- **e_book**：`obj->GetFormType() == RE::FormType::Book`，按 `book_filter_mode`：`TeachesSpell()`（法术书）/ `TeachesSkill()`（技能书）/ 全部；笔记/信件（无 teaches 标志）只在 mode=0 时算
- **e_consumable**：`formType ∈ {Ammo, Ingredient, AlchemyItem, SoulGem, Scroll}`（灵魂石无条件计入）

> 注意：`InventoryEntryData::GetObject()` 会被 windows.h 的 `GetObject` 宏展开成
> `GetObjectA`，实现中改用公开成员 `entry->object` 访问（曾踩坑，勿改回）。

## 四、数据流与集成点

```
scan()（游戏线程，500ms）
  └─ TES::ForEachReferenceInRange(玩家, max_distance)
       actor      → cached_evaluate(actor)      // 命中缓存则跳过评估
       灰烬堆     → cached_evaluate(关联 Actor)  // 战利品在关联 Actor 上
       静态尸体   → cached_evaluate(ref)
  └─ 结果写入 CorpseEntry.loot_categories / best_item_value
       ↓ snapshot（互斥拷贝）
渲染 on_present（渲染线程）
  └─ if (cfg.value_filter_enabled &&
         (corpse.loot_categories & LootFilter::enabled_category_mask()) == 0)
         continue;   // 不画该尸体
```

- **评估放扫描期**：游戏线程、每 0.5s 一次、线性遍历无分配，性能无虞；渲染线程零成本只做位与。
- **评估结果缓存**：FormID → Result（`g_loot_cache`，附评估时的配置快照戳），
  两条失效路径——`TESContainerChangedEvent` 失效对应尸体（玩家拿/放物品、脚本
  AddItem/RemoveItem、respawn 填充库存都走引擎容器变化路径）；loot filter 参数
  修改（阈值/分类开关等）使快照戳失配，下轮扫描全部重评（分类/价值依赖配置，
  改参数后必须重新归类）；读档/新游戏时整体清空。范围内遍历每轮照跑（发现新
  尸体、刷新 ragdoll 包围盒），但已评估尸体的库存合并与分类不再重复执行。scan
  与事件分发同在游戏线程，无需加锁。
- **过滤放渲染期**：配置改动即时生效；MCP 状态行显示"可见 X / 总数 Y"。

## 五、配置项（INI `[LootFilter]` 段）

| INI 键 | 字段 | 默认 | 说明 |
|---|---|---|---|
| ValueFilterEnabled | `value_filter_enabled` | false | 总开关（默认关 = 现状行为） |
| ValueQuestItems | `value_quest_items` | false | 任务物品 |
| ValueKeys | `value_keys` | false | 钥匙 |
| ValueEnchanted | `value_enchanted` | false | 附魔装备 |
| ValueHighValue | `value_high_value` | false | 高价值 |
| HighValueThreshold | `high_value_threshold` | 100.0 | 单件金币价值阈值（load 时 clamp >= 0） |
| ValueBooks | `value_books` | false | 书籍 |
| BookFilterMode | `book_filter_mode` | 1 | 0=全部书籍 1=法术+技能书 2=仅法术书（load 时 clamp 0..2） |
| ValueConsumables | `value_consumables` | false | 消耗品 |

配套 `LootFilter::enabled_category_mask()`：由各开关合成掩码（渲染线程无锁读取）。

## 六、MCP 菜单（ui_menu.cpp "Value Filter" 小节）

- `Checkbox("Filter Valuable Corpses Only")` — 总开关
- `Checkbox` × 6：Quest Items / Keys / Enchanted Gear / High-Value Items / Books / Consumables
  （总开关关闭时由 `BeginDisabled`/`EndDisabled` 灰显禁用，仅在总开关开启时可改）
- `SliderFloat("High Value Threshold", 10~10000)`
- `Combo("Book Mode")`：全部书籍 / 法术+技能书 / 仅法术书
- 状态行：`Visible: X / Y corpses`（总开关开启时显示）

## 七、文件改动清单

| 文件 | 改动 |
|---|---|
| `src/loot_filter.h` / `.cpp`（新增） | `Category` 枚举、`Result`、`evaluate()`、`enabled_category_mask()`、`category_summary()`；匿名命名空间内 `item_value()` / `classify_item()` 纯函数、`fetch_inventory_items()`（fork 自 QuickLoot IE `LoadContainerInventory`）、`refresh_enchanted_weapons()`（引擎函数重定位 SE 50946 / AE 51823） |
| `src/corpse_finder.h` | `CorpseEntry` 增加 `loot_categories`、`best_item_value` |
| `src/corpse_finder.cpp` | 三处候选（actor/ash/static）接入 evaluate；`added to list` 日志追加 `[cats=... best=...]` |
| `src/config.h` / `.cpp` | 新字段、INI `[LootFilter]` 读写、save/reset 覆盖、load 日志追加 lootFilter 状态 |
| `src/esp_renderer.cpp` | 尸体循环首行过滤 continue（约 3 行） |
| `src/ui_menu.cpp` | Value Filter 控件组 + 可见/总数状态行 |
| `src/CMakeLists.txt` | 登记 `loot_filter.cpp/h`（显式 sources） |

依赖方向：`loot_filter`（功能层）→ `config`（核心层）；`corpse_finder`/`esp_renderer`/`ui_menu` 调用 `LootFilter`，无循环依赖，`config.h` 未引入新依赖。

## 八、边界情况

1. **灰烬堆**：库存挂在关联 Actor 上，必须走 `find_ash_pile_owner`（找不到时回退评估堆自身）
2. **静态尸体**：基础容器 + 运行时库存双来源都要评估
3. **不可拿取的任务物品**（`kCantTake`/任务别名）：仍算有价值——玩家需要知道它在哪
4. **已装备物品**（`IsWorn`）：不排除，可搜刮
5. **玩家自身**：已排除，不受影响
6. **LVLI 战利品**：引擎库存初始化后以具体物品入档（含 worn 装备、掷骰结果），
   随合并库存正常计入；容器模板中仍未 resolve 的 `TESLevItem` 占位条目跳过
   （见第三节）——占位条目 count 恒为基类定义值、永不随拿取递减，计入会导致
   拿空的尸体永远判有货
7. **兜底路径的残留占位**：`GetInventoryChanges()` 的兜底初始化
   （`ForceInitInventoryChanges`）可能留下未 resolve 的占位条目，此时跳过它们
   意味着漏报而非误报（宁缺勿错，QuickLoot IE 同款取舍）
8. **Actor 尸体的装备**：手上武器/身上装备在引擎库存初始化后以带 Worn 标记的
   changes 条目出现，随合并库存正常计入；附魔武器的实例数据由
   `RefreshEnchantedWeapons` 补齐
9. **扫描期主动初始化**：进入搜索半径且尚无库存档的尸体会被
   `GetInventoryChanges()` 初始化（提前掷骰，与首次打开等效，玩家无感知）；
   未打开过即存档再读，战利品不再重掷（与原版微差，可接受）
10. **评估缓存**：评估结果按 FormID 缓存，容器变化事件 + 配置快照戳双重失效
    （见第四节）；缓存条目只增不减，单条仅几十字节、量级为"评估过的尸体数"，
    内存可忽略
11. **已搜索尸体标记**（`searched_corpses` 模块）：玩家搜索过的尸体 FormID 集合，
    灰烬堆双向关联（堆/Actor 任一命中即算）。标记忠实于搜索事实——拿空/塞回
    物品都不改写，box 显示由 `has_items && !已搜索` 共同决定；标记只随"引用不
    存在"清除（引擎 FormDelete 回调 + 读档 ResolveFormID 失败丢弃），并经 SKSE
    co-save（record `HLCS`）按存档持久化。搜索始终记录，`HideSearchedEnabled`
    开关只控制 scan() 是否应用。搜索来源两路：原版激活（TESActivateEvent，
    仅玩家）与 QuickLoot IE 打开战利品菜单（`quickloot_compat` 经其公开 API
    的 OpeningLootMenuEvent，QLIE 拿取走 RemoveItem 不发激活事件；QLIE 未安装
    时自动降级为纯激活路径，可选依赖零影响）

## 九、验证清单

- [x] `cmake --preset Release` 配置成功（AE）
- [x] `cmake --build build --config Release` 成功，`/W4 /WX` 无告警；产物 `build/src/Release/HighlightLootableCorpses.dll`
- [ ] 游戏内开过滤：只有破烂（骨头/布匹/低值杂物）的尸体消失；带附魔武器、钥匙、法术书、金币堆的尸体保留
- [ ] 日志核对 `cats=` 判定（灰烬堆、干尸各看一次）
- [ ] 阈值、书籍模式、灵魂石开关各调一次验证即时生效
- [ ] 关掉总开关回归：行为与现状完全一致

## 十、可选扩展（本期未做，架构已预留）

- 按分类染色（`loot_categories` 已存，渲染端取色即可扩展）
- 消耗品数量下限（如箭矢 >= 10 根才算）
- 毒药武器（`IsPoisoned`）单独判定
