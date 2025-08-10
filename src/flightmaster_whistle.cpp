/*
 * Credits: silviu20092
 */

#include <chrono>
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "Chat.h"
#include "MapMgr.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "flightmaster_whistle.h"

FlightmasterWhistle::FlightmasterWhistle()
{
    enabled = true;
    timer = 900;
    preserveZone = true;
    linkMainCities = false;
    minPlayerLevel = 1;

    CreateLinkedZones();
}

FlightmasterWhistle::~FlightmasterWhistle()
{
}

FlightmasterWhistle* FlightmasterWhistle::instance()
{
    static FlightmasterWhistle instance;
    return &instance;
}

void FlightmasterWhistle::LoadFlightmasters()
{
    flightmasters.clear();

    uint32 oldMSTime = getMSTime();

    QueryResult result = WorldDatabase.Query("select c.guid, c.map, c.position_x, c.position_y, c.position_z, c.orientation from creature_template ct "
        "inner join creature c on ct.entry = c.id1 "
        "where ct.npcflag & 8192 = 8192");
    if (!result)
    {
        LOG_INFO("server.loading", ">> Loaded 0 flightmasters.");
        LOG_INFO("server.loading", " ");
        return;
    }

    do
    {
        Field* fields = result->Fetch();
        ObjectGuid::LowType guid = fields[0].Get<ObjectGuid::LowType>();
        uint32 map = fields[1].Get<uint32>();
        float x = fields[2].Get<float>();
        float y = fields[3].Get<float>();
        float z = fields[4].Get<float>();
        float o = fields[5].Get<float>();

        CreatureSpawnInfo spawnInfo;
        spawnInfo.guid = guid;
        spawnInfo.pos.WorldRelocate(map, x, y, z, o);

        flightmasters.push_back(spawnInfo);
    } while (result->NextRow());

    LOG_INFO("server.loading", ">> Loaded {} flightmasters in {} ms", flightmasters.size(), GetMSTimeDiffToNow(oldMSTime));

    if (!sWorld->getBoolConfig(CONFIG_PRELOAD_ALL_NON_INSTANCED_MAP_GRIDS))
    {
        LOG_INFO("server.loading", ">> Preloading grids near flightmasters...");
        PreloadGrids();
    }
}

void FlightmasterWhistle::TeleportToNearestFlightmaster(Player* player) const
{
    uint32 currentTime = getMSTime();
    uint32 lastTime = timerMap[player->GetGUID().GetCounter()];
    uint32 diff = getMSTimeDiff(lastTime, currentTime);

    if (lastTime > 0 && diff < GetTimer() && !player->IsGameMaster())
    {
        SendPlayerMessage(player, "请在 " + FlightmasterWhistle::FormatTimer(GetTimer() - diff) + " 后再次尝试.");
        return;
    }

    SendPlayerMessage(player, "正在传送到最近的飞行管理员");

    if (HandleTeleport(player))
        timerMap[player->GetGUID().GetCounter()] = currentTime;
}

/*static*/ std::unordered_map<uint32, uint32> FlightmasterWhistle::timerMap;

/*static*/ void FlightmasterWhistle::SendPlayerMessage(const Player* player, const std::string& message)
{
    ChatHandler handler(player->GetSession());
    handler.SendSysMessage(message);
}

/*static*/ std::string FlightmasterWhistle::FormatTimer(const uint32 ms)
{
    std::chrono::hh_mm_ss time{ std::chrono::milliseconds(ms) };
    return Acore::ToString(time.minutes().count()) + " 分 " + Acore::ToString(time.seconds().count()) + " 秒";
}

bool FlightmasterWhistle::HandleTeleport(Player* player) const
{
    if (player == nullptr || !player->IsInWorld())
        return false;

    if (!GetEnabled())
    {
        SendPlayerMessage(player, "还不能这么做.");
        return false;
    }

    if (player->GetLevel() < GetMinPlayerLevel())
    {
        SendPlayerMessage(player, "你需要达到至少 " + Acore::ToString(GetMinPlayerLevel()) + " 级才能使用这个功能.");
        return false;
    }

    if (!player->IsAlive())
    {
        SendPlayerMessage(player, "死亡状态下无法执行此操作.");
        return false;
    }

    if (player->IsInCombat())
    {
        SendPlayerMessage(player, "战斗中无法执行此操作.");
        return false;
    }

    if (EnemiesNearby(player))
    {
        SendPlayerMessage(player, "附近有敌方玩家时无法执行此操作.");
        return false;
    }

    if (player->InArena())
    {
        SendPlayerMessage(player, "在竞技场中无法执行此操作.");
        return false;
    }

    Map* map = player->GetMap();
    ASSERT(map != nullptr);
    if (map->Instanceable())
    {
        SendPlayerMessage(player, "在副本中无法执行此操作.");
        return false;
    }

    const CreatureSpawnInfo* nearestFm = ChooseNearestSpawnInfo(player);
    if (nearestFm == nullptr)
    {
        SendPlayerMessage(player, "当前区域未找到飞行管理员.");
        return false;
    }

    player->TeleportTo(nearestFm->pos);
    return true;
}

const FlightmasterWhistle::CreatureSpawnInfo* FlightmasterWhistle::ChooseNearestSpawnInfo(const Player* player) const
{
    Map* map = player->GetMap();
    const CreatureSpawnInfo* nearest = nullptr;
    float minDist = std::numeric_limits<float>::max();

    CreatureSpawnInfoContainer::const_iterator citer = flightmasters.begin();
    while (citer != flightmasters.end())
    {
        const CreatureSpawnInfo* current = &*citer;
        if (current->pos.GetMapId() == map->GetId())
        {
            bool search = !GetPreserveZone();
            if (!search)
            {
                uint32 fmZone = sMapMgr->GetZoneId(PHASEMASK_NORMAL, current->pos);
                search = fmZone == player->GetZoneId() || IsInLinkedZone(fmZone, player);
            }

            if (search)
            {
                float dist = player->GetWorldLocation().GetExactDist(current->pos);
                if (dist < minDist)
                {
                    Creature* creature = ObjectAccessor::GetSpawnedCreatureByDBGUID(map->GetId(), current->guid);
                    if (creature != nullptr && creature->GetReactionTo(player) > REP_UNFRIENDLY && player->CanSeeOrDetect(creature))
                    {
                        minDist = dist;
                        nearest = current;
                    }
                }
            }
        }

        citer++;
    }

    return nearest;
}

bool FlightmasterWhistle::EnemiesNearby(const Player* player, float range) const
{
    std::list<Player*> targets;
    Acore::AnyUnfriendlyUnitInObjectRangeCheck u_check(player, player, range);
    Acore::PlayerListSearcher<Acore::AnyUnfriendlyUnitInObjectRangeCheck> searcher(player, targets, u_check);
    Cell::VisitObjects(player, searcher, range);

    return !targets.empty();
}

void FlightmasterWhistle::CreateLinkedZones()
{
    linkedZones[1637] = 14;         // Orgrimmar -> Durotar
    linkedZones[1638] = 215;        // Thunderbluff -> Mulgore
    linkedZones[1497] = 85;         // Undercity -> Tirisfal Glades
    linkedZones[3487] = 3430;       // Silvermoon City -> Eversong Woods (FM is in Eversong Woods anyway, just a placeholder)
    linkedZones[1519] = 12;         // Stormwind City -> Elwynn Forest
    linkedZones[1537] = 1;          // Ironforge -> Dun Morogh
    linkedZones[1657] = 141;        // Darnassus -> Teldrassil
    linkedZones[3557] = 3524;       // Exodar -> Azuremyst Isle
    linkedZones[4395] = 2817;       // Dalaran -> Crystalsong Forest
}

bool FlightmasterWhistle::IsInLinkedZone(uint32 zone, const Player* player) const
{
    return GetLinkMainCities() && linkedZones.find(zone) != linkedZones.end() && linkedZones.at(zone) == player->GetZoneId();
}

void FlightmasterWhistle::PreloadGrids()
{
    CreatureSpawnInfoContainer::const_iterator citer = flightmasters.begin();
    while (citer != flightmasters.end())
    {
        const CreatureSpawnInfo* current = &*citer;
        Map* map = sMapMgr->CreateBaseMap(current->pos.GetMapId());
        map->LoadGridsInRange(current->pos, GRID_RADIUS);
        citer++;
    }
}

void FlightmasterWhistle::SetEnabled(bool enabled)
{
    this->enabled = enabled;
}

bool FlightmasterWhistle::GetEnabled() const
{
    return enabled;
}

void FlightmasterWhistle::SetTimer(int32 timer)
{
    if (timer < 0)
        this->timer = 0;
    else
        this->timer = (uint32)timer * 1000;
}

uint32 FlightmasterWhistle::GetTimer() const
{
    return timer;
}

void FlightmasterWhistle::SetPreserveZone(bool preserveZone)
{
    this->preserveZone = preserveZone;
}

bool FlightmasterWhistle::GetPreserveZone() const
{
    return preserveZone;
}

void FlightmasterWhistle::SetLinkMainCities(bool linkMainCities)
{
    this->linkMainCities = linkMainCities;
}

bool FlightmasterWhistle::GetLinkMainCities() const
{
    return linkMainCities;
}

void FlightmasterWhistle::SetMinPlayerLevel(int32 level)
{
    if (level < 1 || level > STRONG_MAX_LEVEL)
        level = 1;

    this->minPlayerLevel = (uint8)level;
}

uint8 FlightmasterWhistle::GetMinPlayerLevel() const
{
    return minPlayerLevel;
}
