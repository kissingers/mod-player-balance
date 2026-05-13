/*
 * 玩家职业平衡 -- By牛皮德来
 * 职业3系列天赋树达到一定点数可调节伤害和治疗
 * 增加玩家用拥有特定技能或天赋技能可调节
 * 增加玩家佩戴特定装备时候可调节伤害和治疗
 * 去掉了对npcbots的判断的条件编译,因为不涉及调血量,不会产生bug
 * 为了提高效率,没有对宠物进行同步提升,因为需要对每个伤害增加大量判断,可考虑对应天赋树增加玩家更多伤害替代
 */

#if 0  // 条件编译开关.改为 1 支持调试,配置文件调试开关有效. 改为 0 禁用调试,配置文件调试开关无效,但是能大幅度优化性能. 切换模式需要重新编译
#define DEBUG_DAMAGE_CALCULATION
#endif

#include "ScriptMgr.h"
#include "Player.h"
#include "Config.h"
#include "Chat.h"
#include "SpellAuras.h"
#include "SpellAuraEffects.h"
#include "Unit.h"
#include <sstream>

// ============================================================================
// 全局配置和数据结构
// ============================================================================

static uint32 MaxCharactersGuid = 65536;    //初始化一个下标,启动后会被角色最大(一般为最后注册玩家的)guid加一定冗余替代,冗余量能满足重启前新增加玩家数量就可
static uint32 NeedTalents = 41;             //每天赋树最少多少点生效,考虑主天赋至少41点,可按需要在配置文件内修改.较低值会导致多个天赋都满足并叠加伤害调整

bool ModPlayerBalanceEnabled = false;
bool ModPlayerBalanceDebugEnabled = false;

// 职业配置结构体
struct BalanceConfig
{
    bool Enable;
    float Rate;
    float Talent1, Talent2, Talent3;
    uint32 Talent1Point, Talent2Point, Talent3Point;

    BalanceConfig() : Enable(false), Rate(1.0f), Talent1(1.0f), Talent2(1.0f), Talent3(1.0f), Talent1Point(NeedTalents), Talent2Point(NeedTalents) , Talent3Point(NeedTalents) {}
};

// 按职业ID存储配置（1-11，对应WoW职业）
static BalanceConfig BalanceConfigs[12];

// 全局技能倍率查表（SpellId -> Rate）
static std::map<uint32, float> SpellDamageRates;

// 全局装备倍率查表（ItemId -> Rate）
static std::map<uint32, float> EquipDamageRates;

// 玩家伤害倍率缓存（GUIDLow -> 平衡后输出倍率）
static std::vector<float> PlayerDamageRate(MaxCharactersGuid, 0.0f);

// ============================================================================
// 工具函数
// ============================================================================

// 解析配置字符串的通用函数,解析格式如: "2231:1.2,33313:1.1"
template<typename MapType>
static void ParseConfigString(const std::string& configStr, MapType& ratesMap)
{
    ratesMap.clear();

    if (configStr.empty())
        return;

    std::istringstream ss(configStr);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        // 移除前后空格
        size_t start = item.find_first_not_of(" \t\r\n");
        size_t end = item.find_last_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        item = item.substr(start, end - start + 1);

        // 分离 Id:Rate
        size_t colonPos = item.find(':');
        if (colonPos == std::string::npos) continue;

        try
        {
            uint32 id = std::stoul(item.substr(0, colonPos));
            float rate = std::stof(item.substr(colonPos + 1));
            ratesMap[id] = rate;
        }
        catch (...) { }
    }
}

//计算玩家最终伤害倍率
static float CalculatePlayerDamageRate(Player* player)
{
    if (!player)
        return 0.0f;

    uint8 classId = player->getClass();
    if (classId < 1 || classId > 11)
        return 0.0f;

    BalanceConfig& config = BalanceConfigs[classId];

    // 职业未启用
    if (!config.Enable)
        return 0.0f;

    float finalRate = config.Rate;

    //获取三系天赋点数,分别为角色从左到右的三系
    uint8 specPoints[3] = {};
    player->GetTalentTreePoints(specPoints);

    if (specPoints[0] >= config.Talent1Point)
        finalRate *= config.Talent1;

    if (specPoints[1] >= config.Talent2Point)
        finalRate *= config.Talent2;

    if (specPoints[2] >= config.Talent3Point)
        finalRate *= config.Talent3;

    // 应用已学会的技能倍率
    for (const auto& pair : SpellDamageRates)
    {
        if (player->HasSpell(pair.first) || player->HasTalent(pair.first, player->GetActiveSpec()))
        {
            finalRate *= pair.second;
        }
    }

    // 应用当前穿戴的装备倍率
    for (uint8 i = EQUIPMENT_SLOT_START; i < EQUIPMENT_SLOT_END; ++i)
    {
        if (Item* pItem = player->GetItemByPos(INVENTORY_SLOT_BAG_0, i))
        {
            uint32 itemId = pItem->GetEntry();
            auto equipIter = EquipDamageRates.find(itemId);
            if (equipIter != EquipDamageRates.end())
            {
                finalRate *= equipIter->second;
            }
        }
    }

    // 如果最终结果为 1.0f，则不需要调整，保持为 0.0f
    if (finalRate == 1.0f)
        finalRate = 0.0f;

    return finalRate;
}

// ============================================================================
// 玩家脚本钩子
// ============================================================================

class Mod_PlayerBalance_PlayerScript : public PlayerScript
{
public:
    Mod_PlayerBalance_PlayerScript() : PlayerScript("Mod_PlayerBalance_PlayerScript") {}

    void OnPlayerLogin(Player* player) override
    {
        if (!ModPlayerBalanceEnabled || !player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
    }

    void OnPlayerLogout(Player* player) override
    {
        if (!player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = 0.0f;
    }

    void OnPlayerTalentsReset(Player* player, bool /*noCost*/) override
    {
        if (!ModPlayerBalanceEnabled || !player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
    }

    void OnPlayerLearnTalents(Player* player, uint32 /*talentId*/, uint32 /*talentRank*/, uint32 /*spellid*/) override
    {
        if (!ModPlayerBalanceEnabled || !player)
            return;

        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
    }

    void OnPlayerEquip(Player* player, Item* it, uint8 /*bag*/, uint8 /*slot*/, bool /*update*/)  override
    {
        if (!ModPlayerBalanceEnabled || !player || !it)
            return;
    
        uint32 itemId = it->GetEntry();
        if (EquipDamageRates.find(itemId) == EquipDamageRates.end())
            return;
    
        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
    }
    
    void OnPlayerUnequip(Player* player, Item* it) override
    {
        if (!ModPlayerBalanceEnabled || !player || !it)
            return;
    
        uint32 itemId = it->GetEntry();
        if (EquipDamageRates.find(itemId) == EquipDamageRates.end())
            return;
    
        uint32 guidLow = player->GetGUID().GetCounter();
        if (guidLow < MaxCharactersGuid)
            PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
    }
};

// ============================================================================
// Unit脚本钩子 - 伤害注入点
// ============================================================================

class Mod_PlayerBalance_UnitScript : public UnitScript
{
public:
    Mod_PlayerBalance_UnitScript() : UnitScript("Mod_PlayerBalance_UnitScript", true, { UNITHOOK_MODIFY_HEAL_RECEIVED, UNITHOOK_ON_AURA_APPLY, UNITHOOK_MODIFY_MELEE_DAMAGE, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN, UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK }) {}

    //直接治疗调整
    void ModifyHealReceived(Unit* /*target*/, Unit* healer, uint32& heal, SpellInfo const* spellInfo) override
    {
		//不开启模块或治疗者不是玩家直接退出
        if (!ModPlayerBalanceEnabled || !healer || !healer->ToPlayer())
            return;

        // 当前玩家没有启用伤害调节或等效没有启用调节效果直接退出
        uint32 guidLow = healer->GetGUID().GetCounter();
        //if (guidLow >= MaxCharactersGuid || PlayerDamageRate[guidLow] == 0.0f)  //有冗余量,且这里只读,每次重启会自动适配冗余,所以去掉这行的下标溢出判断,能坚持一年不重启的服务器可考虑改回来,下同
        if (PlayerDamageRate[guidLow] == 0.0f)
            return;

        if (spellInfo && !spellInfo->HasAttribute(SPELL_ATTR0_NO_IMMUNITIES) && spellInfo->Mechanic != MECHANIC_BANDAGE)
        {
            #ifdef DEBUG_DAMAGE_CALCULATION
            if (ModPlayerBalanceDebugEnabled && healer->ToPlayer()->GetSession())
            {
                uint32 originHeal = heal;
                heal *= PlayerDamageRate[guidLow];
                ChatHandler(healer->ToPlayer()->GetSession()).PSendSysMessage("治疗调整: {} {} 从 {} -> {}", 
                    spellInfo->SpellName[healer->ToPlayer()->GetSession()->GetSessionDbcLocale()], spellInfo->Id, originHeal, heal);
            }
            else
            #endif
            {
                heal *= PlayerDamageRate[guidLow];
            }
        }
    }

    //光环吸收调整,主要为套盾吸收类治疗
    void OnAuraApply(Unit* target, Aura* aura) override
    {
        if (!ModPlayerBalanceEnabled || !target || !aura)
            return;

        Unit* caster = aura->GetCaster();
        if (!caster || !caster->ToPlayer())
            return;

        uint32 guidLow = caster->GetGUID().GetCounter();
        //if (guidLow >= MaxCharactersGuid || PlayerDamageRate[guidLow] == 0.0f)
        if (PlayerDamageRate[guidLow] == 0.0f)
            return;

        if (SpellInfo const* spellInfo = aura->GetSpellInfo())
        {
            if (spellInfo->HasAttribute(SPELL_ATTR0_NO_IMMUNITIES))
                return;

            if (spellInfo->HasAura(SPELL_AURA_SCHOOL_ABSORB))
            {
                Unit::AuraEffectList const& effectList = target->GetAuraEffectsByType(SPELL_AURA_SCHOOL_ABSORB);
                for (AuraEffect* eff : effectList)
                {
                    if (eff && eff->GetAuraType() == SPELL_AURA_SCHOOL_ABSORB && eff->GetSpellInfo()->Id == spellInfo->Id)
                    {
                        #ifdef DEBUG_DAMAGE_CALCULATION
                        if (ModPlayerBalanceDebugEnabled && caster->ToPlayer()->GetSession())
                        {
                            int32 absorb = eff->GetAmount();
                            eff->SetAmount(absorb * PlayerDamageRate[guidLow]);
                            ChatHandler(caster->ToPlayer()->GetSession()).PSendSysMessage("光环吸收调整: {} {} 从 {} -> {}", 
                                spellInfo->SpellName[caster->ToPlayer()->GetSession()->GetSessionDbcLocale()], spellInfo->Id, absorb, eff->GetAmount());
                        }
                        else
                        #endif
                        {
                            eff->SetAmount(eff->GetAmount() * PlayerDamageRate[guidLow]);
                        }
                    }
                }
            }
        }
    }

    //肉搏伤害调整
    void ModifyMeleeDamage(Unit* /*target*/, Unit* attacker, uint32& damage) override
    {
        if (!ModPlayerBalanceEnabled || !attacker || !attacker->ToPlayer())
            return;

        uint32 guidLow = attacker->GetGUID().GetCounter();
        //if (damage == 0.0f || guidLow >= MaxCharactersGuid || PlayerDamageRate[guidLow] == 0.0f)
        if (damage == 0.0f || PlayerDamageRate[guidLow] == 0.0f)    //和其他不一样,肉搏为0占比很高,所以优化下逻辑
            return;

        #ifdef DEBUG_DAMAGE_CALCULATION
        if (ModPlayerBalanceDebugEnabled && attacker->ToPlayer()->GetSession())
        {
            uint32 originDamage = damage;
            damage *= PlayerDamageRate[guidLow];
            ChatHandler(attacker->ToPlayer()->GetSession()).PSendSysMessage("近战伤害调整: 从 {} -> {}", originDamage, damage);
        }
        else
        #endif
        {
            damage *= PlayerDamageRate[guidLow];
        }
    }

    //技能伤害调整
    void ModifySpellDamageTaken(Unit* /*target*/, Unit* attacker, int32& damage, SpellInfo const* spellInfo) override
    {
        if (!ModPlayerBalanceEnabled || !attacker || !attacker->ToPlayer())
            return;

        uint32 guidLow = attacker->GetGUID().GetCounter();
        //if (guidLow >= MaxCharactersGuid || PlayerDamageRate[guidLow] == 0.0f)
        if (PlayerDamageRate[guidLow] == 0.0f)
            return;

        if (spellInfo)
        {
            #ifdef DEBUG_DAMAGE_CALCULATION
            if (ModPlayerBalanceDebugEnabled && attacker->ToPlayer()->GetSession())
            {
                int32 originDamage = damage;
                damage *= PlayerDamageRate[guidLow];
                ChatHandler(attacker->ToPlayer()->GetSession()).PSendSysMessage("法术伤害调整: {} {} 从 {} -> {}", 
                    spellInfo->SpellName[attacker->ToPlayer()->GetSession()->GetSessionDbcLocale()], spellInfo->Id, originDamage, damage);
            }
            else
            #endif
            {
                damage *= PlayerDamageRate[guidLow];
            }
        }
    }

    //持续伤害调整
    void ModifyPeriodicDamageAurasTick(Unit* /*target*/, Unit* attacker, uint32& damage, SpellInfo const* spellInfo) override
    {
        if (!ModPlayerBalanceEnabled || !attacker || !attacker->ToPlayer())
            return;

        uint32 guidLow = attacker->GetGUID().GetCounter();
        //if (guidLow >= MaxCharactersGuid || PlayerDamageRate[guidLow] == 0.0f)
        if (PlayerDamageRate[guidLow] == 0.0f)
            return;

        if (spellInfo)
        {
            #ifdef DEBUG_DAMAGE_CALCULATION
            if (ModPlayerBalanceDebugEnabled && attacker->ToPlayer()->GetSession())
            {
                uint32 originDamage = damage;
                damage *= PlayerDamageRate[guidLow];
                ChatHandler(attacker->ToPlayer()->GetSession()).PSendSysMessage("持续伤害调整: {} {} 从 {} -> {}", 
                    spellInfo->SpellName[attacker->ToPlayer()->GetSession()->GetSessionDbcLocale()], spellInfo->Id, originDamage, damage);
            }
            else
            #endif
            {
                damage *= PlayerDamageRate[guidLow]; 
            }
        }
    }
};

// ============================================================================
// 配置加载
// ============================================================================

class Mod_PlayerBalance_ConfigScript : public WorldScript
{
public:
    Mod_PlayerBalance_ConfigScript() : WorldScript("Mod_PlayerBalance_ConfigScript") {}

    void OnStartup() override  //按需修改角色下标以提高性能,只在启动时候执行一次,先于OnAfterConfigLoad,以后reload config不会执行到
    {
        QueryResult result = CharacterDatabase.Query("SELECT MAX(guid) FROM characters");
        if (result)  //用角色lowguid数量加冗余值重新初始化,sql自增量正常下一次重启前不可能增加5千玩家.如你的服务器从来不重启且增加玩家较多,可以加大这个附加数字
        {
            MaxCharactersGuid = (*result)[0].Get<uint32>() + 5000;
            PlayerDamageRate.assign(MaxCharactersGuid, 0.0f);        
        }
    }

    void OnAfterConfigLoad(bool /*reload*/) override  //影响初始化和reload,确认第一次执行在OnStartup后面,所以可以正确初始化
    {
        ModPlayerBalanceEnabled = sConfigMgr->GetOption<bool>("ModPlayerBalance.Enable", false);
        ModPlayerBalanceDebugEnabled = sConfigMgr->GetOption<bool>("ModPlayerBalance.DebugInfo", false);
        NeedTalents = sConfigMgr->GetOption<uint32>("ModPlayerBalance.DefaultTalentPoint", 41);

        if (!ModPlayerBalanceEnabled)
            return;

        // 加载职业配置（从1到11）
        const char* classNames[] = {
            "",              // 0: 无效
            "Warrior",       // 1: 战士
            "Paladin",       // 2: 圣骑士
            "Hunter",        // 3: 猎人
            "Rogue",         // 4: 盗贼
            "Priest",        // 5: 牧师
            "DeathKnight",   // 6: 死亡骑士
            "Shaman",        // 7: 萨满
            "Mage",          // 8: 法师
            "Warlock",       // 9: 术士
            "",              // 10: 无效
            "Druid"          // 11: 德鲁伊
        };

        for (int classId = 1; classId <= 11; ++classId)
        {
            if (classId == 10)
                continue;

            const char* className = classNames[classId];
            std::string prefix = std::string("ModPlayerBalance.") + className;

            BalanceConfigs[classId].Enable = sConfigMgr->GetOption<bool>(prefix + ".Enable", false);
            BalanceConfigs[classId].Rate = sConfigMgr->GetOption<float>(prefix + ".Rate", 1.0f);
            BalanceConfigs[classId].Talent1 = sConfigMgr->GetOption<float>(prefix + ".Talent1", 1.0f);
            BalanceConfigs[classId].Talent2 = sConfigMgr->GetOption<float>(prefix + ".Talent2", 1.0f);
            BalanceConfigs[classId].Talent3 = sConfigMgr->GetOption<float>(prefix + ".Talent3", 1.0f);
            BalanceConfigs[classId].Talent1Point = sConfigMgr->GetOption<uint32>(prefix + ".Talent1Point", NeedTalents);
            BalanceConfigs[classId].Talent2Point = sConfigMgr->GetOption<uint32>(prefix + ".Talent2Point", NeedTalents);
            BalanceConfigs[classId].Talent3Point = sConfigMgr->GetOption<uint32>(prefix + ".Talent3Point", NeedTalents);
        }

        // 一次性解析全局技能配置
        std::string spellConfig = sConfigMgr->GetOption<std::string>("ModPlayerBalance.Spell", "");
        ParseConfigString(spellConfig, SpellDamageRates);

        // 一次性解析全局装备配置
        std::string equipConfig = sConfigMgr->GetOption<std::string>("ModPlayerBalance.Equip", "");
        ParseConfigString(equipConfig, EquipDamageRates);

        //重载时候进行一次所有玩家的伤害系数重新计算,依赖其他参数,故需要放最后
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& itr : players)
        {
            Player* player = itr.second;
            if (!player || !player->IsInWorld())
                continue;
            
            uint32 guidLow = player->GetGUID().GetCounter();
            if (guidLow < MaxCharactersGuid)
                PlayerDamageRate[guidLow] = CalculatePlayerDamageRate(player);
        }
    }
};

// ============================================================================
// 脚本注册
// ============================================================================

void AddModPlayerBalanceScripts()
{
    new Mod_PlayerBalance_ConfigScript();
    new Mod_PlayerBalance_PlayerScript();
    new Mod_PlayerBalance_UnitScript();
}