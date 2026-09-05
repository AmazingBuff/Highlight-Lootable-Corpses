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

`LootFilter::evaluate(ref)` 读取引擎合并库存（基类容器条目 + 运行时 countDelta，
`ref->GetInventory(filter, a_noInit=true)`，只读、不创建 InventoryChanges），
`count > 0` 才算有货（被拿走的物品体现为 count <= 0）。

**LVLI 占位条目**（基类 CNTO 里的升级清单，如 `LootGoldChange`、`LootDraugrWeapon15`）按容器阶段区别对待——这是"搜空尸体仍显示"缺陷的根因修复：

- **已初始化**（`GetInventoryChanges(true) != nullptr`，即容器被打开过 / Actor 出生）：引擎已把 LVLI 解析成具体物品，运行时条目以解析后的具体物品为键，基类 LVLI 条目成为引擎 UI 永不显示、玩家拿不到的占位伪物品 → **跳过**（不计 `has_items`/分类/价值）；
- **未初始化**（从未打开的静态尸体）：LVLI 条目代表尚未生成的真实战利品 → **计入 `has_items`**，且不经过 `GetPlayable()`（本机虚表分发不可靠，与 `IsDead()` 同类问题）。

其余条目还须通过 `object->GetPlayable()`（不可拾取的残留物不算"还有货"）。

逐件分类规则（或关系，命中即置位）：

- **kQuest**：`entry->IsQuestObject()`（内部查 `HasQuestObjectAlias`，即任务别名"任务对象"标志，权威）
- **kKey**：`obj->GetFormType() == RE::FormType::KeyMaster`（`TESKey`）
- **kEnchanted**：有 entry 用 `entry->IsEnchanted()`（覆盖基底 `TESEnchantableForm::formEnchanting` + 实例附魔）；基础容器条目自己查 `obj->As<TESEnchantableForm>() && formEnchanting`
- **kValuable**：`value = item_value(...)`：金币堆（`obj->IsGold()`，FormID==0xF）按枚数计，其余 `entry ? entry->GetValue() : obj->GetGoldValue()`；`value >= high_value_threshold` 命中
- **kBook**：`obj->GetFormType() == RE::FormType::Book`，按 `book_filter_mode`：`TeachesSpell()`（法术书）/ `TeachesSkill()`（技能书）/ 全部；笔记/信件（无 teaches 标志）只在 mode=0 时算
- **kConsumable**：`formType ∈ {Ammo, Ingredient, AlchemyItem, SoulGem, Scroll}`；灵魂石按 `soul_gem_filled_only` 选项要求 `entry->GetSoulLevel() != SOUL_LEVEL::kNone`（已填充）

> 注意：`InventoryEntryData::GetObject()` 会被 windows.h 的 `GetObject` 宏展开成
> `GetObjectA`，实现中改用公开成员 `entry->object` 访问（曾踩坑，勿改回）。

## 四、数据流与集成点

```
scan()（游戏线程，500ms）
  └─ 对每个候选尸体（actor/灰烬堆/静态尸体）:
       actor      → LootFilter::evaluate(actor)
       灰烬堆     → evaluate(find_ash_pile_owner(pile))   // 战利品在关联 Actor 上
       静态尸体   → evaluate(ref)                          // 基础容器 + 运行时
  └─ 结果写入 CorpseEntry.loot_categories / best_item_value
       ↓ snapshot（互斥拷贝）
渲染 on_present（渲染线程）
  └─ if (cfg.loot_filter_enabled &&
         (corpse.loot_categories & LootFilter::enabled_category_mask()) == 0)
         continue;   // 不画该尸体
```

- **评估放扫描期**：游戏线程、每 0.5s 一次、线性遍历无分配，性能无虞；渲染线程零成本只做位与。
- **过滤放渲染期**：配置改动即时生效；MCP 状态行显示"可见 X / 总数 Y"。

## 五、配置项（INI `[LootFilter]` 段）

| INI 键 | 字段 | 默认 | 说明 |
|---|---|---|---|
| LootFilterEnabled | `loot_filter_enabled` | false | 总开关（默认关 = 现状行为） |
| ValueQuestItems | `value_quest_items` | true | 任务物品 |
| ValueKeys | `value_keys` | true | 钥匙 |
| ValueEnchanted | `value_enchanted` | true | 附魔装备 |
| ValueHighValue | `value_high_value` | true | 高价值 |
| HighValueThreshold | `high_value_threshold` | 100.0 | 单件金币价值阈值（load 时 clamp >= 0） |
| ValueBooks | `value_books` | true | 书籍 |
| BookFilterMode | `book_filter_mode` | 1 | 0=全部书籍 1=法术+技能书 2=仅法术书（load 时 clamp 0..2） |
| ValueConsumables | `value_consumables` | true | 消耗品 |
| SoulGemFilledOnly | `soul_gem_filled_only` | true | 灵魂石仅算已填充 |

配套 `LootFilter::enabled_category_mask()`：由各开关合成掩码（渲染线程无锁读取）。

## 六、MCP 菜单（ui_menu.cpp "Loot Filter" 小节）

- `Checkbox("Filter Valuable Corpses Only")` — 总开关
- `Checkbox` × 6：Quest Items / Keys / Enchanted Gear / High-Value Items / Books / Consumables
- `SliderFloat("High Value Threshold", 10~10000)`
- `Combo("Book Mode")`：全部书籍 / 法术+技能书 / 仅法术书
- `Checkbox("Filled Soul Gems Only")`
- 状态行：`Visible: X / Y corpses`（总开关开启时显示）

## 七、文件改动清单

| 文件 | 改动 |
|---|---|
| `src/loot_filter.h` / `.cpp`（新增） | `Category` 枚举、`Result`、`evaluate()`、`enabled_category_mask()`、`category_summary()`；匿名命名空间内 `item_value()` / `classify_item()` 纯函数 |
| `src/corpse_finder.h` | `CorpseEntry` 增加 `loot_categories`、`best_item_value` |
| `src/corpse_finder.cpp` | 三处候选（actor/ash/static）接入 evaluate；`added to list` 日志追加 `[cats=... best=...]` |
| `src/config.h` / `.cpp` | 新字段、INI `[LootFilter]` 读写、save/reset 覆盖、load 日志追加 lootFilter 状态 |
| `src/esp_renderer.cpp` | 尸体循环首行过滤 continue（约 3 行） |
| `src/ui_menu.cpp` | Loot Filter 控件组 + 可见/总数状态行 |
| `src/CMakeLists.txt` | 登记 `loot_filter.cpp/h`（显式 sources） |

依赖方向：`loot_filter`（功能层）→ `config`（核心层）；`corpse_finder`/`esp_renderer`/`ui_menu` 调用 `LootFilter`，无循环依赖，`config.h` 未引入新依赖。

## 八、边界情况

1. **灰烬堆**：库存挂在关联 Actor 上，必须走 `find_ash_pile_owner`（找不到时回退评估堆自身）
2. **静态尸体**：基础容器 + 运行时库存双来源都要评估
3. **不可拿取的任务物品**（`kCantTake`/任务别名）：仍算有价值——玩家需要知道它在哪
4. **已装备物品**（`IsWorn`）：不排除，可搜刮
5. **玩家自身**：已排除，不受影响
6. **空灵魂石**：默认排除（filled_only 可关）
7. **基础容器里的灵魂石**：无实例信息，filled_only 开启时无法验证填充态，会被排除（静态尸体场景极少见，可接受）
8. **LVLI 占位条目**：见第三节——已初始化容器跳过（搜空即消失的关键），未初始化容器视为潜在战利品
9. **打开后升级清单解析为空的容器**：已初始化 + 无具体条目 → 判空，box 消失（与引擎 UI 一致）

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
