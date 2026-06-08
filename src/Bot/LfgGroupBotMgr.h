/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_LFGGROUPBOTMGR_H
#define _PLAYERBOT_LFGGROUPBOTMGR_H

#include "Common.h"
#include "LFG.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"

#include <map>
#include <vector>

class Player;
class PlayerbotAI;

// Tracks the lifecycle of a spawned LFG bot
enum class LfgSpawnBotState
{
    LOGGING_IN,    // Bot is being logged into the world
    IN_QUEUE,      // Bot is in the LFG queue, waiting for match
    IN_DUNGEON,    // Bot is inside the LFG dungeon
    FINISHED,      // Dungeon finished, pending cleanup
    TO_LOGOUT      // Marked for logout / removal from tracking
};

// Info for a single spawned bot
struct LfgSpawnedBotInfo
{
    ObjectGuid botGuid;         // Bot character GUID
    ObjectGuid playerGuid;      // The real player this bot was spawned for
    uint8      assignedRole;    // Assigned LFG role (PLAYER_ROLE_TANK/HEALER/DAMAGE)
    time_t     spawnTime;       // When the bot was spawned
    LfgSpawnBotState state;     // Current lifecycle state
};

// Info about a queueing player that needs bots
struct LfgPlayerQueueInfo
{
    ObjectGuid playerGuid;
    uint8      playerLevel;
    uint32     teamId;          // TEAM_ALLIANCE or TEAM_HORDE
    uint8      playerRole;      // Player's own LFG role
    uint8      neededTanks;     // How many tanks still needed (target: 1)
    uint8      neededHealers;   // How many healers still needed (target: 1)
    uint8      neededDps;       // How many DPS still needed (target: 3)
    time_t     queueStartTime;  // When the player started queueing
};

class LfgGroupBotMgr
{
public:
    static LfgGroupBotMgr& instance();

    // Called every tick from the world update loop
    void Update(uint32 diff);

    // Called when a real player is detected queueing for LFG
    // @param player     The real player
    // @param dungeonIds The dungeon IDs the player selected
    void OnPlayerQueueForLfg(Player* player, std::vector<uint32> const& dungeonIds);

    // Get the number of active spawned bots for a given player
    uint32 GetSpawnedBotCountForPlayer(ObjectGuid playerGuid) const;

    // Force cleanup of all bots spawned for a specific player (e.g. player logs out)
    void CleanupBotsForPlayer(ObjectGuid playerGuid);

private:
    LfgGroupBotMgr() = default;
    ~LfgGroupBotMgr() = default;
    LfgGroupBotMgr(LfgGroupBotMgr const&) = delete;
    LfgGroupBotMgr& operator=(LfgGroupBotMgr const&) = delete;

    // Calculate what roles are needed to fill a 5-man party (1T/1H/3DPS)
    void CalculateNeededRoles(Player* player, LfgPlayerQueueInfo& info);

    // Select a suitable bot character from the database matching level, faction, and role
    // Returns true and fills outBotGuid/outClass/outLevel on success
    bool SelectBotCharacter(LfgPlayerQueueInfo const& playerInfo,
                            uint8 neededRole,
                            uint32& outBotGuid,
                            uint8& outClass,
                            uint8& outLevel);

    // Login a specific bot character and set its master to the player
    bool LoginBot(uint32 botGuid, Player* player);

    // Cleanup a single bot: leave LFG queue, leave group, schedule logout
    void CleanupBot(LfgSpawnedBotInfo& info);

    // Periodic state check and cleanup
    void CheckAndCleanup();

    // Return preferred classes for a given role (TANK/HEALER/DAMAGE)
    static std::vector<uint8> GetPreferredClasses(uint8 role);

    // ---- members ----
    std::vector<LfgSpawnedBotInfo> m_spawnedBots;
    time_t m_lastCleanupTime = 0;

    // Set of bot GUIDs currently in the process of being logged in (to avoid duplicates)
    std::set<uint32> m_pendingLogins;
};

#define sLfgGroupBotMgr LfgGroupBotMgr::instance()

#endif
