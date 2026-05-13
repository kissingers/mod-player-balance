# 高性职业平衡模块 - BY牛皮德来

本项目是对 `mod-playerclass-damage` 模块的全面升级和改名，旨在支持 **众多玩家并发** 环境下的 **高效的高频伤害计算**

- **分层键名支持**：`ModPlayerBalance.<Class>.Enable/Rate/Talent1/2/3/Talent1/2/3point`
- **10个职业完整覆盖**：Warrior, Paladin, Hunter, Rogue, Priest, DeathKnight, Shaman, Mage, Warlock, Druid
- **天赋倍率配置**：每个职业 3 系天赋独立配置,独立夹层系数和独立所需点数
- **全局技能配置**：`ModPlayerBalance.Spell = "spellId:rate,spellId:rate,..."`
- **一次性解析**：在 `OnStartup()` 中完成所有配置解析和动态玩家最大数,无后续运行时开销
- **访问**：将哈希表数据结构改为固定大小数组 `PlayerDamageRate[guidLow]` 时间复杂度O(1)
- **玩家生命周期管理**：4 个完整的钩子
- **倍率计算**：最终倍率 = 基础Rate × 每系列天赋累乘倍率 × 已学会技能倍率
