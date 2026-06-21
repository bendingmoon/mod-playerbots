/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "LfgGroupBotMgr.h"

#include "CharacterCache.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "LFGMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

using namespace lfg;

LfgGroupBotMgr& LfgGroupBotMgr::instance()
{
    static LfgGroupBotMgr instance;
    return instance;
}

void LfgGroupBotMgr::Update(uint32 /*diff*/)
{
    // Run cleanup check every 15 seconds
    if (m_lastCleanupTime + 15 > time(nullptr))
        return;

    m_lastCleanupTime = time(nullptr);
    CheckAndCleanup();
}

// ---- Public API ----

void LfgGroupBotMgr::OnPlayerQueueForLfg(Player* player, std::vector<uint32> const& /*dungeonIds*/)
{
    if (!player || !player->IsInWorld())
        return;

    // Skip if this player already has 4 active spawned bots
    uint32 existingBots = GetSpawnedBotCountForPlayer(player->GetGUID());
    if (existingBots >= 4)
        return;

    // Build queue info and calculate what roles are needed
    LfgPlayerQueueInfo info;
    info.playerGuid = player->GetGUID();
    info.playerLevel = player->GetLevel();
    info.teamId = player->GetTeamId();
    info.queueStartTime = time(nullptr);

    // Determine player's own role from LFG system
    // sLFGMgr->GetRoles() returns the role mask the player selected in the LFG UI
    info.playerRole = sLFGMgr->GetRoles(player->GetGUID());
    if (!info.playerRole)
        info.playerRole = PLAYER_ROLE_DAMAGE; // fallback: default to DPS if no roles selected

    CalculateNeededRoles(player, info);

    uint32 totalNeeded = info.neededTanks + info.neededHealers + info.neededDps;
    uint32 canSpawn = 4 - existingBots;

    if (totalNeeded == 0 || canSpawn == 0)
        return;

    // Trim to what we can spawn, priority: TANK > HEALER > DPS
    if (totalNeeded > canSpawn)
    {
        uint32 adjust = totalNeeded - canSpawn;
        while (adjust > 0 && info.neededDps > 0)  { info.neededDps--;  adjust--; }
        while (adjust > 0 && info.neededHealers > 0) { info.neededHealers--; adjust--; }
        while (adjust > 0 && info.neededTanks > 0)   { info.neededTanks--;   adjust--; }
    }

    LOG_INFO("playerbots", "LFG: Player {} needs T:{}/H:{}/D:{} bots ({} existing, {} can spawn)",
             player->GetName().c_str(),
             info.neededTanks, info.neededHealers, info.neededDps,
             existingBots, canSpawn);

    // Spawn bots by role priority: TANK -> HEALER -> DPS
    auto spawnRole = [&](uint8 role, uint32 count)
    {
        for (uint32 i = 0; i < count; i++)
        {
            uint32 botGuid = 0;
            uint8 botClass = 0, botLevel = 0;

            if (!SelectBotCharacter(info, role, botGuid, botClass, botLevel))
            {
                LOG_INFO("playerbots", "LFG: No suitable bot found for role: {}",
                         (role & PLAYER_ROLE_TANK) ? "TANK" : ((role & PLAYER_ROLE_HEALER) ? "HEAL" : "DPS"));
                continue;
            }

            if (!LoginBot(botGuid, player))
                continue;

            LfgSpawnedBotInfo spawned;
            spawned.botGuid = ObjectGuid::Create<HighGuid::Player>(botGuid);
            spawned.playerGuid = player->GetGUID();
            spawned.assignedRole = role;
            spawned.spawnTime = time(nullptr);
            spawned.state = LfgSpawnBotState::LOGGING_IN;

            m_spawnedBots.push_back(spawned);

            LOG_INFO("playerbots", "LFG: Spawned bot guid={} class={} level={} role={} for player {}",
                     botGuid, botClass, botLevel,
                     (role & PLAYER_ROLE_TANK) ? "TANK" : ((role & PLAYER_ROLE_HEALER) ? "HEAL" : "DPS"),
                     player->GetName().c_str());
        }
    };

    spawnRole(PLAYER_ROLE_TANK,   info.neededTanks);
    spawnRole(PLAYER_ROLE_HEALER, info.neededHealers);
    spawnRole(PLAYER_ROLE_DAMAGE, info.neededDps);
}

uint32 LfgGroupBotMgr::GetSpawnedBotCountForPlayer(ObjectGuid playerGuid) const
{
    uint32 count = 0;
    for (auto const& info : m_spawnedBots)
    {
        if (info.playerGuid == playerGuid &&
            info.state < LfgSpawnBotState::FINISHED)
            count++;
    }
    return count;
}

void LfgGroupBotMgr::CleanupBotsForPlayer(ObjectGuid playerGuid)
{
    LOG_INFO("playerbots", "LFG: Cleaning up all bots for player {}", playerGuid.ToString().c_str());

    for (auto& info : m_spawnedBots)
    {
        if (info.playerGuid == playerGuid &&
            info.state < LfgSpawnBotState::TO_LOGOUT)
        {
            CleanupBot(info);
        }
    }
}

void LfgGroupBotMgr::OnBotLeftGroup(Player* bot)
{
    if (!bot)
        return;

    // Look for this bot in the spawned bots list
    for (auto& info : m_spawnedBots)
    {
        if (info.botGuid == bot->GetGUID() &&
            info.state < LfgSpawnBotState::TO_LOGOUT)
        {
            LOG_INFO("playerbots", "LFG: Bot {} left group, cleaning up immediately",
                     bot->GetName().c_str());
            CleanupBot(info);
            return;
        }
    }
}

// ---- Private: Role Calculation ----

void LfgGroupBotMgr::CalculateNeededRoles(Player* player, LfgPlayerQueueInfo& info)
{
    uint8 existingTanks = 0, existingHealers = 0, existingDps = 0;

    // Count roles of ALL real (non-bot) party members, not just the triggering player.
    // At LFG_STATE_QUEUED all members have already confirmed their roles in LFG.
    Group* group = player->GetGroup();
    if (group)
    {
        for (auto const& slot : group->GetMemberSlots())
        {
            Player* member = ObjectAccessor::FindPlayer(slot.guid);
            if (!member)
                continue;

            // Skip AI-controlled bots — only count real human players
            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
            if (memberAI && !memberAI->IsRealPlayer())
                continue;

            uint8 memberRole = sLFGMgr->GetRoles(member->GetGUID());
            if (!memberRole)
                memberRole = PLAYER_ROLE_DAMAGE; // fallback if no role set

            if (memberRole & PLAYER_ROLE_TANK)
                existingTanks++;
            else if (memberRole & PLAYER_ROLE_HEALER)
                existingHealers++;
            else
                existingDps++;
        }
    }
    else
    {
        // Solo player — count just the player
        if (info.playerRole & PLAYER_ROLE_TANK)
            existingTanks++;
        else if (info.playerRole & PLAYER_ROLE_HEALER)
            existingHealers++;
        else
            existingDps++;
    }

    // Count already-spawned bots for this player (these are bots we spawned
    // in a previous check cycle that are still logging in or in queue)
    for (auto const& spawned : m_spawnedBots)
    {
        if (spawned.playerGuid == player->GetGUID() &&
            spawned.state < LfgSpawnBotState::FINISHED)
        {
            if (spawned.assignedRole & PLAYER_ROLE_TANK)
                existingTanks++;
            else if (spawned.assignedRole & PLAYER_ROLE_HEALER)
                existingHealers++;
            else
                existingDps++;
        }
    }

    // Target composition: 1 Tank, 1 Healer, 3 DPS = 5-man party
    info.neededTanks   = (existingTanks   < 1) ? (1 - existingTanks)   : 0;
    info.neededHealers = (existingHealers < 1) ? (1 - existingHealers) : 0;
    info.neededDps     = (existingDps     < 3) ? (3 - existingDps)     : 0;
}

// ---- Private: Bot Selection ----

bool LfgGroupBotMgr::SelectBotCharacter(LfgPlayerQueueInfo const& playerInfo,
                                         uint8 neededRole,
                                         uint32& outBotGuid,
                                         uint8& outClass,
                                         uint8& outLevel)
{
    std::vector<uint8> preferredClasses = GetPreferredClasses(neededRole);

    // Build class filter string
    std::string classFilter;
    for (size_t i = 0; i < preferredClasses.size(); i++)
    {
        if (i > 0) classFilter += ",";
        classFilter += std::to_string(preferredClasses[i]);
    }

    // Build race filter string based on faction
    // Alliance races: 1(Human),3(Dwarf),4(Night Elf),7(Gnome),11(Draenei)
    // Horde races: 2(Orc),5(Undead),6(Tauren),8(Blood Elf),10(Troll)
    std::string raceFilter = (playerInfo.teamId == TEAM_ALLIANCE)
        ? "1,3,4,7,11"
        : "2,5,6,8,10";

    // Build exclusion list: bots already spawned + pending logins
    std::string excludeFilter;
    for (auto const& spawned : m_spawnedBots)
    {
        if (!excludeFilter.empty()) excludeFilter += ",";
        excludeFilter += std::to_string(spawned.botGuid.GetCounter());
    }
    for (uint32 pending : m_pendingLogins)
    {
        if (!excludeFilter.empty()) excludeFilter += ",";
        excludeFilter += std::to_string(pending);
    }

    // Query a suitable random bot character:
    // - Account must be in the random bot account list (via randomBotAccounts)
    // - Matching class and race (faction)
    // - Not already online or pending login
    // - No level filter: bot level will be synced to match the player after login
    std::string excludeClause = excludeFilter.empty()
        ? ""
        : " AND c.guid NOT IN (" + excludeFilter + ")";

    // Build account filter from randomBotAccounts
    std::string accountFilter;
    for (size_t i = 0; i < sPlayerbotAIConfig.randomBotAccounts.size(); i++)
    {
        if (i > 0) accountFilter += ",";
        accountFilter += std::to_string(sPlayerbotAIConfig.randomBotAccounts[i]);
    }

    if (accountFilter.empty())
    {
        LOG_ERROR("playerbots", "LFG: No random bot accounts configured!");
        return false;
    }

    std::string query = std::string(
        "SELECT c.guid, c.class, c.race, c.level "
        "FROM characters c "
        "WHERE c.account IN (") + accountFilter + ")"
        "  AND c.class IN (" + classFilter + ")"
        "  AND c.race IN (" + raceFilter + ")"
        "  AND c.online = 0"
        + excludeClause
        + " ORDER BY RAND()"
        + " LIMIT 1";

    QueryResult result = CharacterDatabase.Query(query.c_str());
    if (!result)
        return false;

    Field* fields = result->Fetch();
    outBotGuid  = fields[0].Get<uint32>();
    outClass    = fields[1].Get<uint8>();
    // fields[2] is race, not used directly
    outLevel    = fields[3].Get<uint8>();

    return true;
}

std::vector<uint8> LfgGroupBotMgr::GetPreferredClasses(uint8 role)
{
    if (role & PLAYER_ROLE_TANK)
        return { CLASS_WARRIOR, CLASS_PALADIN, CLASS_DEATH_KNIGHT, CLASS_DRUID };
    else if (role & PLAYER_ROLE_HEALER)
        return { CLASS_PRIEST, CLASS_PALADIN, CLASS_SHAMAN, CLASS_DRUID };
    else // DPS
        return { CLASS_MAGE, CLASS_WARLOCK, CLASS_HUNTER, CLASS_ROGUE,
                 CLASS_SHAMAN, CLASS_DRUID, CLASS_PRIEST,
                 CLASS_WARRIOR, CLASS_PALADIN, CLASS_DEATH_KNIGHT };
}

// ---- Private: Bot Login ----

bool LfgGroupBotMgr::LoginBot(uint32 botGuid, Player* player)
{
    ObjectGuid guid = ObjectGuid::Create<HighGuid::Player>(botGuid);

    // Avoid duplicate logins
    if (m_pendingLogins.find(botGuid) != m_pendingLogins.end())
        return false;

    // Check if already online
    if (ObjectAccessor::FindConnectedPlayer(guid))
        return false;

    // Mark as pending
    m_pendingLogins.insert(botGuid);

    // Set the "add" event immediately to prevent AddRandomBots() from
    // selecting this character while the async login is still in progress
    sRandomPlayerbotMgr.SetEventValue(botGuid, "add", 1,
                                      sPlayerbotAIConfig.permanentlyInWorldTime);

    // Login as random bot (masterAccountId = 0 means random bot)
    sRandomPlayerbotMgr.AddPlayerBot(guid, 0);

    return true;
}

// ---- Private: Cleanup ----

void LfgGroupBotMgr::CleanupBot(LfgSpawnedBotInfo& info)
{
    Player* bot = ObjectAccessor::FindConnectedPlayer(info.botGuid);
    if (!bot)
    {
        // Bot is already offline. We can't access bot->GetGroup() since the
        // player object is gone, but we can look up the LFG group through
        // sLFGMgr and remove the offline bot from the group.
        ObjectGuid groupGuid = sLFGMgr->GetGroup(info.botGuid);
        if (groupGuid)
        {
            if (Group* group = sGroupMgr->GetGroupByGUID(groupGuid.GetCounter()))
            {
                LOG_INFO("playerbots", "LFG: Removing offline bot {} from group {}",
                         info.botGuid.ToString().c_str(), groupGuid.ToString().c_str());
                group->RemoveMember(info.botGuid, GROUP_REMOVEMETHOD_LEAVE);
            }
        }

        // Clear the "add" event so ProcessBot won't keep trying to re-add
        // this bot indefinitely, then mark for removal.
        sRandomPlayerbotMgr.SetEventValue(info.botGuid.GetCounter(), "add", 0, 0);
        m_pendingLogins.erase(info.botGuid.GetCounter());
        info.state = LfgSpawnBotState::TO_LOGOUT;
        return;
    }

    LOG_INFO("playerbots", "LFG: Cleaning up bot {} ({})",
             bot->GetName().c_str(), info.botGuid.ToString().c_str());

    // 1. Leave LFG queue if still queued
    lfg::LfgState state = sLFGMgr->GetState(bot->GetGUID());
    if (state != lfg::LFG_STATE_NONE && state <= lfg::LFG_STATE_QUEUED)
    {
        WorldPacket* packet = new WorldPacket(CMSG_LFG_LEAVE);
        bot->GetSession()->QueuePacket(packet);
    }

    // 2. Leave group (synchronous, before logout)
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (botAI)
    {
        Group* group = bot->GetGroup();
        if (group)
        {
            LOG_INFO("playerbots", "LFG: Bot {} leaving group before cleanup",
                     bot->GetName().c_str());
            group->RemoveMember(bot->GetGUID(), GROUP_REMOVEMETHOD_LEAVE);
        }
    }

    // 3. Immediately expire the "add" event so ProcessBot won't re-add the bot
    //    and log the bot out right now (no delay).
    //    Previously a 30s delay was used, during which the bot's AI kept running
    //    and could join another group, preventing logout indefinitely.
    sRandomPlayerbotMgr.SetEventValue(info.botGuid.GetCounter(), "add", 0, 0);

    // 4. Immediately logout the bot
    sRandomPlayerbotMgr.LogoutPlayerBot(info.botGuid);

    // 5. Clean up tracking
    m_pendingLogins.erase(info.botGuid.GetCounter());
    info.state = LfgSpawnBotState::TO_LOGOUT;
}

void LfgGroupBotMgr::CheckAndCleanup()
{
    // Remove TO_LOGOUT entries from the tracking list
    m_spawnedBots.erase(
        std::remove_if(m_spawnedBots.begin(), m_spawnedBots.end(),
                       [](LfgSpawnedBotInfo const& info) {
                           return info.state == LfgSpawnBotState::TO_LOGOUT;
                       }),
        m_spawnedBots.end());

    time_t now = time(nullptr);

    for (auto& info : m_spawnedBots)
    {
        // Already marked for removal, skip
        if (info.state == LfgSpawnBotState::TO_LOGOUT)
            continue;

        Player* bot = ObjectAccessor::FindConnectedPlayer(info.botGuid);

        // ---- State: LOGGING_IN ----
        if (info.state == LfgSpawnBotState::LOGGING_IN)
        {
            if (bot && bot->IsInWorld())
            {
                // Bot has logged in successfully
                m_pendingLogins.erase(info.botGuid.GetCounter());

                PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
                Player* player = ObjectAccessor::FindConnectedPlayer(info.playerGuid);

                // Sync bot's level to match the queueing player
                if (botAI && player)
                {
                    uint32 diff = sPlayerbotAIConfig.lfgSpawnBotMaxLevelDiff;
                    uint32 minLvl = player->GetLevel() >= diff ? player->GetLevel() - diff : 15;
                    if (minLvl < 15) minLvl = 15;
                    uint32 maxLvl = std::min(player->GetLevel() + diff, 80u);
                    if (maxLvl < 15) maxLvl = 15;
                    uint32 targetLevel = urand(minLvl, maxLvl);

                    sRandomPlayerbotMgr.SetValue(bot, "level", targetLevel);
                    PlayerbotFactory factory(bot, targetLevel);
                    factory.Randomize(false);

                    LOG_INFO("playerbots", "LFG: Bot {} synced to level {} (player {})",
                             bot->GetName().c_str(), targetLevel, player->GetName().c_str());
                }

                // Set player as master and join LFG directly
                if (botAI && player)
                {
                    botAI->SetMaster(player);
                    botAI->ResetStrategies();

                    // Directly send LFG join — bot won't pass the IsRandomBot()
                    // check needed for LfgStrategy auto-join.
                    std::vector<uint32> dungeons =
                        sRandomPlayerbotMgr.LfgDungeons[bot->GetTeamId()];

                    std::set<uint32> list;
                    for (uint32 dId : dungeons)
                    {
                        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dId);
                        if (!dungeon || (dungeon->TypeID != LFG_TYPE_RANDOM
                                      && dungeon->TypeID != LFG_TYPE_DUNGEON
                                      && dungeon->TypeID != LFG_TYPE_HEROIC
                                      && dungeon->TypeID != LFG_TYPE_RAID))
                            continue;
                        uint8 botLevel = bot->GetLevel();
                        // Only check MinLevel/MaxLevel bounds when they are actually set.
                        // MaxLevel=0 means "no maximum" (common for LFG_TYPE_RANDOM entries),
                        // so we must not compare against it or all bots get filtered out.
                        if ((dungeon->MinLevel && botLevel < dungeon->MinLevel) ||
                            (dungeon->MaxLevel && botLevel > dungeon->MaxLevel))
                            continue;
                        if (botLevel > dungeon->MinLevel + 10
                            && dungeon->TypeID == LFG_TYPE_DUNGEON)
                            continue;
                        list.insert(dId);
                    }

                    if (!list.empty())
                    {
                        WorldPacket* pkt = new WorldPacket(CMSG_LFG_JOIN);
                        *pkt << (uint32)info.assignedRole;
                        *pkt << (bool)false;
                        *pkt << (bool)false;
                        *pkt << (uint8)(list.size());
                        for (uint32 dId : list)
                            *pkt << (uint32)dId;
                        *pkt << (uint8)3 << (uint8)0 << (uint8)0 << (uint8)0;
                        *pkt << std::to_string(botAI->GetEquipGearScore(bot));
                        bot->GetSession()->QueuePacket(pkt);

                        LOG_INFO("playerbots", "LFG: Bot {} joined LFG role={} dungeons={}",
                                 bot->GetName().c_str(), info.assignedRole, list.size());

                        info.state = LfgSpawnBotState::IN_QUEUE;
                        LOG_INFO("playerbots", "LFG: Bot {} now in world, queued for LFG with player {}",
                                 bot->GetName().c_str(), player ? player->GetName().c_str() : "?");
                    }
                    else
                    {
                        // No matching dungeons — clean up immediately instead of
                        // transitioning to IN_QUEUE and blocking the slot for 300s.
                        LOG_INFO("playerbots", "LFG: Bot {} — no matching dungeons, cleaning up immediately",
                                 bot->GetName().c_str());
                        CleanupBot(info);
                    }
                }
            }
            else if (now - info.spawnTime > 60)
            {
                // Login timeout (1 minute) — bot failed to log in.
                // Clean up properly (including the "add" event) so the slot
                // is freed for a replacement bot.
                LOG_INFO("playerbots", "LFG: Bot login timeout for guid {}, cleaning up",
                         info.botGuid.ToString().c_str());
                CleanupBot(info);
            }
            continue;
        }

        // ---- State: IN_QUEUE ----
        if (info.state == LfgSpawnBotState::IN_QUEUE)
        {
            if (!bot || !bot->IsInWorld())
            {
                // Bot went offline unexpectedly — clean up properly so
                // a replacement can be spawned on the next CheckLfgQueue.
                LOG_INFO("playerbots", "LFG: Bot {} went offline while in queue, cleaning up",
                         info.botGuid.ToString().c_str());
                CleanupBot(info);
                continue;
            }

            // Check if bot is now in an LFG dungeon group
            Group* group = bot->GetGroup();
            if (group && group->isLFGGroup())
            {
                lfg::LfgState gState = sLFGMgr->GetState(group->GetGUID());
                if (gState == lfg::LFG_STATE_DUNGEON ||
                    gState == lfg::LFG_STATE_FINISHED_DUNGEON)
                {
                    info.state = LfgSpawnBotState::IN_DUNGEON;

                    // If the original player is still in this group, keep the bot
                    // assigned to them — no need to re-assign.
                    if (group->IsMember(info.playerGuid))
                    {
                        LOG_DEBUG("playerbots", "LFG: Bot {} — original player {} still in group, keeping",
                                  bot->GetName().c_str(), info.playerGuid.ToString().c_str());
                    }
                    else
                    {
                        // Re-assign bot to the real human player in this group.
                        // Priority: group leader if human, otherwise first human found.
                        ObjectGuid newPlayerGuid;
                    for (auto const& slot : group->GetMemberSlots())
                    {
                        if (Player* member = ObjectAccessor::FindPlayer(slot.guid))
                        {
                            // Real human = no bot AI OR bot AI with IsRealPlayer()
                            PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
                            if (!memberAI || memberAI->IsRealPlayer())
                            {
                                if (group->IsLeader(member->GetGUID()))
                                {
                                    // Leader is a real human — priority
                                    newPlayerGuid = member->GetGUID();
                                    break;
                                }
                                if (!newPlayerGuid)
                                    newPlayerGuid = member->GetGUID(); // first human found
                            }
                        }
                    }

                    if (!newPlayerGuid)
                    {
                        LOG_INFO("playerbots", "LFG: Bot {} entered dungeon but no human found (player offline?), keeping original player {}",
                                 bot->GetName().c_str(), info.playerGuid.ToString().c_str());
                    }
                    else if (newPlayerGuid != info.playerGuid)
                    {
                        if (!group->IsLeader(newPlayerGuid))
                            group->ChangeLeader(newPlayerGuid);

                        info.playerGuid = newPlayerGuid;
                        LOG_INFO("playerbots", "LFG: Bot {} re-assigned to human player {}",
                                 bot->GetName().c_str(), newPlayerGuid.ToString().c_str());
                    }
                    } // else: original player not in group

                    continue;
                }
            }

            // Check if player is still online and still queueing
            Player* player = ObjectAccessor::FindConnectedPlayer(info.playerGuid);
            if (!player)
            {
                // Player logged out, cleanup this bot
                CleanupBot(info);
                continue;
            }

            // If player left the LFG queue (cancelled), clean up immediately
            lfg::LfgState playerState = sLFGMgr->GetState(player->GetGUID());
            if (playerState == lfg::LFG_STATE_NONE)
            {
                LOG_INFO("playerbots", "LFG: Player {} left queue, cleaning up bot {}",
                         player->GetName().c_str(), bot->GetName().c_str());
                CleanupBot(info);
                continue;
            }

            // Also clean up if player is now in a dungeon (already matched without this bot)
            if (playerState >= lfg::LFG_STATE_DUNGEON)
            {
                LOG_INFO("playerbots", "LFG: Player {} already in dungeon, cleaning up bot {}",
                         player->GetName().c_str(), bot->GetName().c_str());
                CleanupBot(info);
                continue;
            }

            // Timeout: if queued too long (5+ minutes) without getting a match
            if (now - info.spawnTime > 300)
            {
                LOG_INFO("playerbots", "LFG: Bot {} queue timeout, cleaning up",
                         bot->GetName().c_str());
                CleanupBot(info);
                continue;
            }
        }

        // ---- State: IN_DUNGEON ----
        if (info.state == LfgSpawnBotState::IN_DUNGEON)
        {
            if (!bot || !bot->IsInWorld())
            {
                // Bot went offline during dungeon — proper cleanup so the slot
                // is freed and a replacement can be spawned.
                LOG_INFO("playerbots", "LFG: Bot {} went offline while in dungeon, cleaning up",
                         info.botGuid.ToString().c_str());
                CleanupBot(info);
                continue;
            }

            Group* group = bot->GetGroup();

            // Dungeon finished (LFG state = FINISHED_DUNGEON)
            if (group && group->isLFGGroup())
            {
                lfg::LfgState gState = sLFGMgr->GetState(group->GetGUID());
                if (gState == lfg::LFG_STATE_FINISHED_DUNGEON)
                {
                    info.state = LfgSpawnBotState::FINISHED;
                    LOG_INFO("playerbots", "LFG: Bot {} finished dungeon, scheduling cleanup",
                             bot->GetName().c_str());
                    CleanupBot(info);
                    continue;
                }
            }

            // Group disbanded or bot left/kicked from group
            if (!group || !group->isLFGGroup())
            {
                Map* map = bot->GetMap();
                if (!map || !map->Instanceable())
                {
                    // Bot is no longer in an instance and not in a LFG group
                    info.state = LfgSpawnBotState::FINISHED;
                    LOG_INFO("playerbots", "LFG: Bot {} left dungeon/group, cleaning up",
                             bot->GetName().c_str());
                    CleanupBot(info);
                    continue;
                }
                else
                {
                    // Still in instance but no longer in LFG group → kicked from group
                    LOG_INFO("playerbots", "LFG: Bot {} was kicked from group, cleaning up",
                             bot->GetName().c_str());
                    CleanupBot(info);
                    continue;
                }
            }

            // If the assigned player has been offline > 5 min, don't wait forever
            Player* assignedPlayer = ObjectAccessor::FindConnectedPlayer(info.playerGuid);
            if (!assignedPlayer && now - info.spawnTime > 300)
            {
                LOG_INFO("playerbots", "LFG: Bot {} — assigned player offline > 5min during dungeon, cleaning up",
                         bot->GetName().c_str());
                CleanupBot(info);
                continue;
            }

            // Safety net: max 60 minutes in a dungeon, cleanup regardless
            if (now - info.spawnTime > 3600)
            {
                LOG_INFO("playerbots", "LFG: Bot {} dungeon timeout (60min), cleaning up",
                         bot->GetName().c_str());
                CleanupBot(info);
                continue;
            }
        }

        // ---- State: FINISHED (should have been cleaned up already) ----
        if (info.state == LfgSpawnBotState::FINISHED)
        {
            // Safety: force cleanup if somehow still tracked
            CleanupBot(info);
        }
    }
}
