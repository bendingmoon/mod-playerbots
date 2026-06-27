#include "NewRpgAction.h"

#include <cmath>
#include <cstdlib>

#include "BroadcastHelper.h"
#include "CellImpl.h"
#include "ChatHelper.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "G3D/Vector2.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "LootMgr.h"
#include "NearestGameObjects.h"
#include "GossipDef.h"
#include "IVMapMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Object.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "QuestDef.h"
#include "Random.h"
#include "SharedDefines.h"
#include "Timer.h"
#include "TravelMgr.h"

bool TellRpgStatusAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;
    std::string out = botAI->rpgInfo.ToString();
    bot->Whisper(out.c_str(), LANG_UNIVERSAL, owner);
    return true;
}

bool StartRpgDoQuestAction::Execute(Event event)
{
    Player* owner = event.getOwner();
    if (!owner)
        return false;

    std::string const text = event.getParam();
    PlayerbotChatHandler ch(owner);
    uint32 questId = ch.extractQuestId(text);
    const Quest* quest = sObjectMgr->GetQuestTemplate(questId);
    if (quest)
    {
        botAI->rpgInfo.ChangeToDoQuest(questId, quest);
        bot->Whisper("Start to do quest " + std::to_string(questId), LANG_UNIVERSAL, owner);
        return true;
    }
    bot->Whisper("Invalid quest " + text, LANG_UNIVERSAL, owner);
    return false;
}

bool NewRpgStatusUpdateAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    NewRpgStatus status = info.GetStatus();
    switch (status)
    {
        case RPG_IDLE:
            return RandomChangeStatus({RPG_GO_CAMP, RPG_GO_GRIND, RPG_WANDER_RANDOM, RPG_WANDER_NPC, RPG_DO_QUEST,
                                       RPG_TRAVEL_FLIGHT, RPG_REST, RPG_OUTDOOR_PVP});

        case RPG_GO_GRIND:
        {
            auto& data = std::get<NewRpgInfo::GoGrind>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_GRIND -> WANDER_RANDOM
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderRandom();
                return true;
            }
            break;
        }
        case RPG_GO_CAMP:
        {
            auto& data = std::get<NewRpgInfo::GoCamp>(info.data);
            WorldPosition& originalPos = data.pos;
            assert(data.pos != WorldPosition());
            // GO_CAMP -> WANDER_NPC
            if (bot->GetExactDist(originalPos) < 10.0f)
            {
                info.ChangeToWanderNpc();
                return true;
            }
            break;
        }
        case RPG_WANDER_RANDOM:
        {
            // WANDER_RANDOM -> IDLE
            if (info.HasStatusPersisted(statusWanderRandomDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_WANDER_NPC:
        {
            if (info.HasStatusPersisted(statusWanderNpcDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_DO_QUEST:
        {
            // DO_QUEST -> IDLE
            if (info.HasStatusPersisted(statusDoQuestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_TRAVEL_FLIGHT:
        {
            auto& data = std::get<NewRpgInfo::TravelFlight>(info.data);
            if (data.inFlight && !bot->IsInFlight())
            {
                // flight arrival
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_REST:
        {
            // REST -> IDLE
            if (info.HasStatusPersisted(statusRestDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        case RPG_OUTDOOR_PVP:
        {
            if (info.HasStatusPersisted(statusOutDoorPvPDuration))
            {
                info.ChangeToIdle();
                return true;
            }
            break;
        }
        default:
            break;
    }
    return false;
}

bool NewRpgGoGrindAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;
    if (auto* data = std::get_if<NewRpgInfo::GoGrind>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        // Small nudge so the next tick's MoveFarTo starts from a
        // slightly different position. Kept small so it doesn't look
        // like the bot is abandoning its destination.
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgGoCampAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    if (auto* data = std::get_if<NewRpgInfo::GoCamp>(&botAI->rpgInfo.data))
    {
        if (MoveFarTo(data->pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    return false;
}

bool NewRpgWanderRandomAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    return MoveRandomNear();
}

bool NewRpgWanderNpcAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::WanderNpc>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    if (!data.npcOrGo)
    {
        // No npc can be found, switch to IDLE
        ObjectGuid npcOrGo = ChooseNpcOrGameObjectToInteract();
        if (npcOrGo.IsEmpty())
        {
            info.ChangeToIdle();
            return true;
        }
        data.npcOrGo = npcOrGo;
        data.lastReach = 0;
        return true;
    }

    WorldObject* object = ObjectAccessor::GetWorldObject(*bot, data.npcOrGo);
    if (object && IsWithinInteractionDist(object))
    {
        if (!data.lastReach)
        {
            data.lastReach = getMSTime();
            if (bot->CanInteractWithQuestGiver(object))
                InteractWithNpcOrGameObjectForQuest(data.npcOrGo);
            return true;
        }

        if (data.lastReach && GetMSTimeDiffToNow(data.lastReach) < npcStayTime)
            return false;

        // has reached the npc for more than `npcStayTime`, select the next target
        data.npcOrGo = ObjectGuid();
        data.lastReach = 0;
    }
    else
    {
        if (MoveWorldObjectTo(data.npcOrGo))
            return true;
        // NPC pathing failed (random offset in a wall, mmap hiccup, etc).
        // Take a small random step so the next tick retries from a
        // different spot instead of staring at the NPC from afar.
        return MoveRandomNear(15.0f);
    }

    return true;
}

bool NewRpgDoQuestAction::Execute(Event /*event*/)
{
    if (SearchQuestGiverAndAcceptOrReward())
        return true;

    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::DoQuest>(&info.data);
    if (!dataPtr)
        return false;
    auto& data = *dataPtr;
    uint32 questId = data.questId;
    uint8 questStatus = bot->GetQuestStatus(questId);
    switch (questStatus)
    {
        case QUEST_STATUS_INCOMPLETE:
            return DoIncompleteQuest(data);
        case QUEST_STATUS_COMPLETE:
            return DoCompletedQuest(data);
        default:
            break;
    }
    info.ChangeToIdle();
    return true;
}

bool NewRpgDoQuestAction::DoIncompleteQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    if (data.pos != WorldPosition())
    {
        /// @TODO: extract to a new function
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has completed
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const QuestStatusData& q_status = bot->getQuestStatusMap().at(questId);
        bool completed = true;
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] < quest->RequiredNpcOrGoCount[currentObjective])
                completed = false;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] <
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                completed = false;
        }
        // the current objective is completed, clear and find a new objective later
        if (completed)
        {
            data.lastReachPOI = 0;
            data.pos = WorldPosition();
            data.objectiveIdx = 0;
            data.lastInteractGO.Clear();
        }
    }
    if (data.pos == WorldPosition())
    {
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo))
        {
            // can't find a poi pos to go, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        uint32 rndIdx = urand(0, poiInfo.size() - 1);
        G3D::Vector2 nearestPoi = poiInfo[rndIdx].pos;
        int32 objectiveIdx = poiInfo[rndIdx].objectiveIdx;

        float dx = nearestPoi.x, dy = nearestPoi.y;

        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = objectiveIdx;
        data.lastInteractGO.Clear();
    }

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        // Long-range sampler couldn't land a candidate — nudge the
        // bot a short distance so the next tick retries from a
        // different position instead of sitting idle.
        return MoveRandomNear(10.0f);
    }
    // Now we are near the quest objective
    // Creature-killing and loot should be handled automatically by grind strategy.
    // Gameobject interaction (gathering, mining, etc.) is handled below.

    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        bool hasProgression = false;
        int32 currentObjective = data.objectiveIdx;
        // check if the objective has progression
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        const QuestStatusData& q_status = bot->getQuestStatusMap().at(questId);
        if (currentObjective < QUEST_OBJECTIVES_COUNT)
        {
            if (q_status.CreatureOrGOCount[currentObjective] != 0 && quest->RequiredNpcOrGoCount[currentObjective])
                hasProgression = true;
        }
        else if (currentObjective < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT)
        {
            if (q_status.ItemCount[currentObjective - QUEST_OBJECTIVES_COUNT] != 0 &&
                quest->RequiredItemCount[currentObjective - QUEST_OBJECTIVES_COUNT])
                hasProgression = true;
        }
        if (!hasProgression)
        {
            // we has reach the poi for more than 5 mins but no progession
            // may not be able to complete this quest, marked as abandoned
            /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
            botAI->lowPriorityQuest.insert(questId);
            botAI->rpgStatistic.questAbandoned++;
            LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
            botAI->rpgInfo.ChangeToIdle();
            return true;
        }
        // clear and select another poi later
        data.lastReachPOI = 0;
        data.pos = WorldPosition();
        data.objectiveIdx = 0;
        return true;
    }

    // At the POI: check if the current objective requires interacting with
    // a gameobject. Two patterns exist in quest design:
    //
    // 1) NPC/GO objective (indices 0-3 in RequiredNpcOrGo):
    //    - RequiredNpcOrGo[i] < 0 → gameobject template entry (negate it)
    //    Search GOs by entry ID.
    //
    // 2) Item objective (indices 4-9 in RequiredItemId):
    //    - The quest item drops from gameobject loot (e.g. "Cactus Apple"
    //      from "Cactus" plants).
    //    Search GOs whose loot table contains quest items the player needs.
    //
    // Both paths feed into the same interaction logic below.
    int32 goObjectiveIdx = data.objectiveIdx;
    Quest const* goQuest = sObjectMgr->GetQuestTemplate(questId);
    GameObject* go = nullptr;
    if (goObjectiveIdx < QUEST_OBJECTIVES_COUNT && goQuest)
    {
        int32 npcOrGoEntry = goQuest->RequiredNpcOrGo[goObjectiveIdx];
        LOG_DEBUG("playerbots", "[AutoGather] {} quest {} objectiveIdx={} npcOrGoEntry={}",
                  bot->GetName(), questId, goObjectiveIdx, npcOrGoEntry);
        if (npcOrGoEntry < 0)  // GO objective
        {
            go = FindNearestQuestGameObject(-npcOrGoEntry, 80.0f);
            LOG_DEBUG("playerbots", "[AutoGather] {} FindNearestQuestGameObject(entry={}) -> {}",
                      bot->GetName(), -npcOrGoEntry, go ? "found" : "NOT FOUND");
        }
    }
    else if (goObjectiveIdx >= QUEST_OBJECTIVES_COUNT &&
             goObjectiveIdx < QUEST_OBJECTIVES_COUNT + QUEST_ITEM_OBJECTIVES_COUNT && goQuest)
    {
        uint32 itemIdx = goObjectiveIdx - QUEST_OBJECTIVES_COUNT;
        uint32 questItemId = goQuest->RequiredItemId[itemIdx];
        LOG_DEBUG("playerbots", "[AutoGather] {} quest {} objectiveIdx={} (item) questItemId={}",
                  bot->GetName(), questId, goObjectiveIdx, questItemId);
        if (questItemId)
        {
            go = FindNearestQuestItemGameObject(questItemId, 80.0f);
            LOG_DEBUG("playerbots", "[AutoGather] {} FindNearestQuestItemGameObject(itemId={}) -> {}",
                      bot->GetName(), questItemId, go ? "found" : "NOT FOUND");
        }
    }

    if (go)
    {
        float dist = bot->GetDistance(go);
        // Use the GO's actual interaction distance (varies per GO type),
        // not the generic INTERACTION_DISTANCE. This matches what the
        // server checks in HandleGameObjectUseOpcode and spell range validation.
        float goInteractDist = go->GetInteractionDistance();
        LOG_DEBUG("playerbots", "[AutoGather] {} GO entry={} type={} dist={:.2f} goInteractDist={:.2f} isMoving={} lastGO={}",
                  bot->GetName(), go->GetEntry(), go->GetGoType(), dist, goInteractDist,
                  bot->isMoving() ? 1 : 0,
                  (data.lastInteractGO == go->GetGUID()) ? "same" : "new");
        if (dist <= goInteractDist)
        {
            if (bot->isMoving())
            {
                LOG_DEBUG("playerbots", "[AutoGather] {} still moving, waiting for stop", bot->GetName());
                return false;
            }
            if (bot->IsNonMeleeSpellCast(false))
            {
                LOG_DEBUG("playerbots", "[AutoGather] {} already casting, waiting", bot->GetName());
                return false;
            }
            // Don't re-interact with the same GO — the spell or loot from
            // the previous tick may still be processing. If blocked, search
            // for the next unblocked GO instead of returning (otherwise we'd
            // be stuck forever since the nearest is always the blocked one).
            if (data.lastInteractGO == go->GetGUID())
            {
                LOG_DEBUG("playerbots", "[AutoGather] {} nearest GO already-interacted, searching next candidate",
                          bot->GetName());
                go = nullptr;
                std::list<GameObject*> candidates;
                AnyGameObjectInObjectRangeCheck u_check(bot, 80.0f);
                Acore::GameObjectListSearcher<AnyGameObjectInObjectRangeCheck> searcher(bot, candidates, u_check);
                Cell::VisitObjects(bot, searcher, 80.0f);
                float bestDist = 80.0f;
                for (GameObject* candidate : candidates)
                {
                    if (!candidate || !candidate->isSpawned() || !candidate->IsInWorld())
                        continue;
                    uint32 ct = candidate->GetGoType();
                    if (ct != GAMEOBJECT_TYPE_CHEST && ct != GAMEOBJECT_TYPE_GOOBER)
                        continue;
                    uint32 lid = candidate->GetGOInfo()->GetLootId();
                    if (!lid || !LootTemplates_Gameobject.HaveQuestLootForPlayer(lid, bot))
                        continue;
                    if (candidate->GetGUID() == data.lastInteractGO)
                        continue;
                    float d = bot->GetDistance(candidate);
                    if (d < bestDist) { go = candidate; bestDist = d; }
                }
                LOG_DEBUG("playerbots", "[AutoGather] {} next unblocked GO -> {} (dist={:.1f})",
                          bot->GetName(), go ? std::to_string(go->GetEntry()) : "NONE", bestDist);
                if (!go)
                    return false; // no other GO nearby, wait for respawn or POI timeout

                // Always move to the new candidate GO first, even if it's
                // technically within range. The bot is positioned near the
                // previous GO and the spell's actual range may be shorter
                // than goInteractDist, causing "out of range" on the cast.
                float newDist = bot->GetDistance(go);
                float newInteractDist = go->GetInteractionDistance();
                LOG_DEBUG("playerbots", "[AutoGather] {} unblocked GO at {:.1f}yd, moving to it first",
                          bot->GetName(), newDist);
                return MoveWorldObjectTo(go->GetGUID(), newInteractDist);
            }

            if (!go)
                return false;

            GameObjectTemplate const* goInfo = go->GetGOInfo();
            bool interacted = false;
            switch (go->GetGoType())
            {
                case GAMEOBJECT_TYPE_GOOBER:
                    LOG_DEBUG("playerbots", "[AutoGather] {} GOOBER -> go->Use()", bot->GetName());
                    bot->SetFacingToObject(go);
                    go->Use(bot);
                    interacted = true;
                    break;
                case GAMEOBJECT_TYPE_CHEST:
                {
                    uint32 lockId = goInfo->GetLockId();
                    uint32 gatherSpellId = 0;
                    LOG_DEBUG("playerbots", "[AutoGather] {} CHEST lockId={}", bot->GetName(), lockId);
                    if (lockId)
                    {
                        LockEntry const* lockInfo = sLockStore.LookupEntry(lockId);
                        if (lockInfo)
                        {
                            // Pass 1: LOCK_KEY_SKILL with required profession
                            for (uint8 i = 0; i < MAX_LOCK_CASE && !gatherSpellId; ++i)
                            {
                                if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                                {
                                    uint32 skillId = SkillByLockType(LockType(lockInfo->Index[i]));
                                    uint32 reqSkill = std::max(2u, lockInfo->Skill[i]);
                                    LOG_DEBUG("playerbots", "[AutoGather] {} LOCK_KEY_SKILL skillId={} reqSkill={} hasSkill={} skillVal={}",
                                              bot->GetName(), skillId, reqSkill,
                                              bot->HasSkill((SkillType)skillId) ? 1 : 0,
                                              bot->GetSkillValue(skillId));
                                    if ((skillId == SKILL_MINING || skillId == SKILL_HERBALISM ||
                                         skillId == SKILL_LOCKPICKING) &&
                                        bot->HasSkill((SkillType)skillId) &&
                                        uint32(bot->GetSkillValue(skillId)) >= reqSkill)
                                        gatherSpellId = botAI->GetGatheringSpellId(skillId);
                                }
                            }
                            // Pass 2: LOCK_KEY_SKILL → try generic opening spell by EffectMiscValue
                            // (e.g. spell 22810 "Opening"). Matches the client's
                            // SpellEffectMiscCache[properties2] lookup.
                            for (uint8 i = 0; i < MAX_LOCK_CASE && !gatherSpellId; ++i)
                            {
                                if (lockInfo->Type[i] == LOCK_KEY_SKILL)
                                {
                                    gatherSpellId = botAI->GetOpeningSpellIdByMiscValue(lockInfo->Index[i]);
                                    if (gatherSpellId)
                                        LOG_DEBUG("playerbots", "[AutoGather] {} LOCK_KEY_SKILL misc={} -> spellId={}",
                                                  bot->GetName(), lockInfo->Index[i], gatherSpellId);
                                }
                            }
                            // Pass 3: LOCK_KEY_SPELL → explicit spell ID
                            for (uint8 i = 0; i < MAX_LOCK_CASE && !gatherSpellId; ++i)
                            {
                                if (lockInfo->Type[i] == LOCK_KEY_SPELL)
                                {
                                    gatherSpellId = lockInfo->Index[i];
                                    LOG_DEBUG("playerbots", "[AutoGather] {} LOCK_KEY_SPELL -> spellId={}",
                                              bot->GetName(), gatherSpellId);
                                }
                            }
                            // Pass 4: LOCK_KEY_ITEM → key in inventory
                            for (uint8 i = 0; i < MAX_LOCK_CASE && !gatherSpellId && !interacted; ++i)
                            {
                                if (lockInfo->Type[i] == LOCK_KEY_ITEM &&
                                    bot->GetItemCount(lockInfo->Index[i]) > 0)
                                {
                                    LOG_DEBUG("playerbots", "[AutoGather] {} LOCK_KEY_ITEM key={} -> Use",
                                              bot->GetName(), lockInfo->Index[i]);
                                    go->Use(bot);
                                    interacted = true;
                                }
                            }
                        }
                        else
                            LOG_DEBUG("playerbots", "[AutoGather] {} lockInfo NOT FOUND for lockId={}",
                                      bot->GetName(), lockId);
                    }
                    if (gatherSpellId)
                    {
                        LOG_DEBUG("playerbots", "[AutoGather] {} CastSpell(spellId={})", bot->GetName(), gatherSpellId);
                        bot->SetFacingToObject(go);
                        bot->CastSpell(go, gatherSpellId, false);
                        interacted = true;
                    }
                    // Only SendLoot if no spell was found AND we haven't already
                    // interacted with this GO (anti-spam, since SendLoot bypasses
                    // the normal GO despawn/respawn cycle).
                    if (!interacted && data.lastInteractGO != go->GetGUID())
                    {
                        LOG_DEBUG("playerbots", "[AutoGather] {} fallback -> SendLoot (no spell found)", bot->GetName());
                        bot->SetFacingToObject(go);
                        bot->SendLoot(go->GetGUID(), LOOT_SKINNING);
                        interacted = true;
                        data.lastInteractGO = go->GetGUID();
                    }
                    else if (!interacted)
                    {
                        LOG_DEBUG("playerbots", "[AutoGather] {} skipping same GO (already interacted)", bot->GetName());
                    }
                    // Send CMSG_GAMEOBJ_REPORT_USE — the client always sends this
                    // after interacting with a GO. It triggers go->AI()->GossipHello()
                    // which may be required for quest credit on some GOs.
                    {
                        WorldPacket reportPacket(CMSG_GAMEOBJ_REPORT_USE, 8);
                        reportPacket << go->GetGUID();
                        bot->GetSession()->HandleGameobjectReportUse(reportPacket);
                    }
                    break;
                }
                default:
                    LOG_DEBUG("playerbots", "[AutoGather] {} unhandled GO type={} -> go->Use()",
                              bot->GetName(), go->GetGoType());
                    break;
            }
            if (!interacted)
            {
                bot->SetFacingToObject(go);
                go->Use(bot);
            }
            // Record which GO we interacted with to prevent re-casting
            // on the same object next tick (anti-spam for instant spells).
            data.lastInteractGO = go->GetGUID();
            return true;
        }
        else
        {
            LOG_DEBUG("playerbots", "[AutoGather] {} moving to GO (dist={:.2f} targetDist={:.2f})",
                      bot->GetName(), dist, goInteractDist);
            return MoveWorldObjectTo(go->GetGUID(), goInteractDist);
        }
    }

    // At the POI: keep the bot actively placed but avoid large
    // random 20yd hops that look like pacing back and forth. A small
    // ~8yd wander reads as the bot looking around while grind/loot
    // strategies do their work.
    return MoveRandomNear(8.0f);
}

bool NewRpgDoQuestAction::DoCompletedQuest(NewRpgInfo::DoQuest& data)
{
    uint32 questId = data.questId;
    const Quest* quest = data.quest;

    if (data.objectiveIdx != -1)
    {
        // if quest is completed, back to poi with -1 idx to reward
        BroadcastHelper::BroadcastQuestUpdateComplete(botAI, bot, quest);
        botAI->rpgStatistic.questCompleted++;
        std::vector<POIInfo> poiInfo;
        if (!GetQuestPOIPosAndObjectiveIdx(questId, poiInfo, true))
        {
            // can't find a poi pos to reward, stop doing quest for now
            botAI->rpgInfo.ChangeToIdle();
            return false;
        }
        assert(poiInfo.size() > 0);
        // now we get the place to get rewarded
        float dx = poiInfo[0].pos.x, dy = poiInfo[0].pos.y;
        // z = MAX_HEIGHT as we do not know accurate z
        float dz = std::max(bot->GetMap()->GetHeight(dx, dy, MAX_HEIGHT), bot->GetMap()->GetWaterLevel(dx, dy));

        // double check for GetQuestPOIPosAndObjectiveIdx
        if (dz == INVALID_HEIGHT || dz == VMAP_INVALID_HEIGHT_VALUE)
            return false;

        WorldPosition pos(bot->GetMapId(), dx, dy, dz);
        data.lastReachPOI = 0;
        data.pos = pos;
        data.objectiveIdx = -1;
    }

    if (data.pos == WorldPosition())
        return false;

    if (bot->GetDistance(data.pos) > 10.0f && !data.lastReachPOI)
    {
        if (MoveFarTo(data.pos))
            return true;
        return MoveRandomNear(10.0f);
    }

    // Now we are near the qoi of reward
    // the quest should be rewarded by SearchQuestGiverAndAcceptOrReward
    if (!data.lastReachPOI)
    {
        data.lastReachPOI = getMSTime();
        return true;
    }
    // stayed at this POI for more than 5 minutes
    if (GetMSTimeDiffToNow(data.lastReachPOI) >= poiStayTime)
    {
        // e.g. Can not reward quest to gameobjects
        /// @TODO: It may be better to make lowPriorityQuest a global set shared by all bots (or saved in db)
        botAI->lowPriorityQuest.insert(questId);
        botAI->rpgStatistic.questAbandoned++;
        LOG_DEBUG("playerbots", "[New RPG] {} marked as abandoned quest {}", bot->GetName(), questId);
        botAI->rpgInfo.ChangeToIdle();
        return true;
    }
    return false;
}

bool NewRpgTravelFlightAction::Execute(Event /*event*/)
{
    NewRpgInfo& info = botAI->rpgInfo;
    auto* dataPtr = std::get_if<NewRpgInfo::TravelFlight>(&info.data);
    if (!dataPtr)
        return false;

    auto& data = *dataPtr;
    if (bot->IsInFlight())
    {
        data.inFlight = true;
        return false;
    }

    if (bot->GetDistance(data.flightMasterPos) > INTERACTION_DISTANCE)
        return MoveFarTo(data.flightMasterPos);

    Creature* flightMaster = bot->FindNearestCreature(data.flightMasterEntry, INTERACTION_DISTANCE * 3);
    if (!flightMaster || !flightMaster->IsAlive())
    {
        info.ChangeToIdle();
        return true;
    }
    if (bot->GetDistance(flightMaster) > INTERACTION_DISTANCE)
        return MoveFarTo(flightMaster);

    std::vector<uint32> nodes = data.path;

    botAI->RemoveShapeshift();
    if (bot->IsMounted())
        bot->Dismount();

    if (!bot->ActivateTaxiPathTo(nodes, flightMaster, 0))
    {
        LOG_DEBUG("playerbots", "[New RPG] {} active taxi path {} (from {} to {}) failed", bot->GetName(),
                  flightMaster->GetEntry(), nodes[0], nodes[nodes.size() - 1]);
        info.ChangeToIdle();
        return true;
    }
    return true;
}
