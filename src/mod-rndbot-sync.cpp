/*
 * This file is part of mod-rndbot-sync.
 * Copyright (C) 2026 Yuof
 *
 * Derived from mod-player-bot-level-brackets (AGPL-3.0).
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Affero General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Affero General Public License
 * for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

// Core AzerothCore headers
#include "mod-rndbot-sync.h"
#include "ScriptMgr.h"
#include "Log.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Player.h"
#include "WorldSession.h"
#include "ObjectAccessor.h"
#include "Map.h"
#include "MapMgr.h"
#include "GameTime.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "ArenaTeamMgr.h"
#include "World.h"
#include "Random.h"

// mod-playerbots headers
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotMgr.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "TravelMgr.h"
#include "Group.h"
#include "SocialMgr.h"
#include "Item.h"
#include "Bag.h"
#include "ItemTemplate.h"
#include "ObjectMgr.h"
#include "CreatureData.h"
#include "DBCStores.h"

// Standard library
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <unordered_map>
#include <utility>
#include <cctype>
#include <cmath>

using namespace Acore::ChatCommands;

// Defined further down; the target/eligibility helpers above need them.
static bool IsHumanPlayer(Player* player);
static bool IsPlayerRandomBot(Player* player);
static bool IsBotExcluded(Player* bot);
static bool BotSuitsItsZone(Player* bot);
static void ResolveGatherBand(Player* bot, uint32 zoneId, int playerBandLower, int playerCeiling,
                              int& bandLowerOut, int& ceilingOut, bool& spreadOut);

// ---- Configuration -----------------------------------------------------------
static bool   g_Enabled = true;
static uint32 g_CheckFrequency = 300;              // seconds; 0 = events only
static uint32 g_TargetPercent = 30;                // % of eligible bots in band
static uint32 g_TargetBand = 3;                    // +/- levels around target
static uint32 g_ProcessLimit = 5;                  // max adjustments per pass; 0 = all
static bool   g_AllowDownleveling = true;          // pull bots above the ceiling down
static bool   g_IgnoreGuildBotsWithRealPlayers = true;
static bool   g_IgnoreFriendListed = true;
static bool   g_IgnoreArenaTeamBots = true;
static bool   g_IgnoreGroupedBots = true;          // bots in any party/raid
static std::unordered_set<std::string> g_ExcludeBotNames;
static uint32 g_GuildCacheRefreshFrequency = 600;  // seconds
static bool   g_DkProgressionEnabled = false;
static uint32 g_DkRequiredState = 13;              // PROGRESSION_TBC_TIER_5
static bool   g_FullDebug = false;
static bool   g_LiteDebug = false;
static bool  g_IlvlSyncEnabled = true;      // cap bot gear at the player's ilvl
static int32 g_OriginalGearScoreLimit = 0;  // configured RandomGearScoreLimit
static bool   g_ConfiguredPlayerZoneTeleport = false; // configured Playerbots.RandomBotConcentrateInPlayerZone
static bool   g_AreaSyncEnabled = true;     // gather bots into the player's zone
static uint32 g_AreaSyncZone = 0;           // fixed zone id; 0 = follow the player's zone
static bool   g_AreaSyncUsePlayerZone = true; // true = use the player's current zone id
static uint32 g_ZoneSpreadPercent = 30;     // % of a zone's bots matched to the zone's own level
static bool   g_FactionSyncEnabled = true;  // force every bot onto the real players' faction
// playerbots' configured ratios, kept so switching FactionSync off restores them.
static uint32 g_ConfiguredAllianceRatio = 50;
static uint32 g_ConfiguredHordeRatio = 50;

// mod-playerbots' own baseline, captured on the first EvaluateDeathKnightGate() call so the
// module can restore it when DeathKnightProgression is switched off (instead of leaving its
// override in place). Captured there, not in LoadConfig(), because mod-playerbots reads its
// own config in its own OnInit and this module must not race that.
static bool g_OriginalDisableDkLogin = false;
static bool g_OriginalDkCaptured = false;

// Zone the pass should gather bots into: a pinned zone, or the target player's zone.
// The player's zone is resolved once per pass and reused, so a bot is never moved into
// a different zone than the one the target player was standing in when the pass ran.
static uint32 ResolveAreaSyncZone(Player* targetPlayer)
{
    if (!g_AreaSyncUsePlayerZone)
    {
        return g_AreaSyncZone;
    }
    return targetPlayer ? targetPlayer->GetZoneId() : 0;
}

// Pulled from mod-playerbots config.
static uint8  g_RandomBotMinLevel = 1;
static uint8  g_RandomBotMaxLevel = 80;
static std::string g_RandomBotAccountPrefix = "rndbot";

// Converts a field to the value mod-playerbots stores in `character_social.friend`: the
// low GUID (character counter). Used for the friend cache; also the form to fall back to
// should a bot ever carry a non-player high GUID.
static inline uint32 LowGuidValue(Player const* player)
{
    return player->GetGUID().GetCounter();
}

// The highest-level real player online, or nullptr when none is. Shared by the adjustment
// pass and the status command so the two can never disagree about who the target is.
static Player* FindTargetPlayer(int& targetLevelOut)
{
    Player* best = nullptr;
    targetLevelOut = -1;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld() || !IsHumanPlayer(player))
        {
            continue;
        }
        if (static_cast<int>(player->GetLevel()) > targetLevelOut)
        {
            targetLevelOut = static_cast<int>(player->GetLevel());
            best = player;
        }
    }
    return best;
}

// Bots eligible for a level or zone change: in world, random bots, not excluded. Passed
// over once per pass and shared by the level and zone steps.
static std::vector<Player*> CollectEligibleBots()
{
    std::vector<Player*> eligible;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* bot = itr.second;
        if (!bot || !bot->IsInWorld() || IsHumanPlayer(bot))
        {
            continue;
        }
        if (!IsPlayerRandomBot(bot) || IsBotExcluded(bot))
        {
            continue;
        }
        eligible.push_back(bot);
    }
    return eligible;
}

// The level band for a target: [bandLower, ceiling]. `ceiling` is the target's level plus
// the band, clamped to both the world player cap and mod-playerbots' own RandomBotMaxLevel —
// otherwise a module-configured bot cap below the world cap would be exceeded.
static void ComputeLevelBand(int target, int& bandLowerOut, int& ceilingOut)
{
    uint32 const worldMax = sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL);
    int const maxLevel = std::min(static_cast<int>(worldMax), static_cast<int>(g_RandomBotMaxLevel));

    bandLowerOut = std::max(static_cast<int>(g_RandomBotMinLevel), target - static_cast<int>(g_TargetBand));
    ceilingOut = std::min(maxLevel, target + static_cast<int>(g_TargetBand));
    if (bandLowerOut > ceilingOut)
    {
        bandLowerOut = ceilingOut;
    }
}

static void LoadConfig()
{
    g_Enabled = sConfigMgr->GetOption<bool>("RndBotSync.Enabled", true);
    // Clamp the user-configured periods so the millisecond math below cannot overflow and
    // a stray 0 in one of the "every N seconds" keys cannot turn into a query-per-tick.
    g_CheckFrequency = std::min<uint32>(sConfigMgr->GetOption<uint32>("RndBotSync.CheckFrequency", 300), 86400);
    g_TargetPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("RndBotSync.TargetPercent", 30), 100);
    g_TargetBand = std::min<uint32>(sConfigMgr->GetOption<uint32>("RndBotSync.TargetBand", 3), 80);
    g_ProcessLimit = sConfigMgr->GetOption<uint32>("RndBotSync.ProcessLimit", 5);
    g_AllowDownleveling = sConfigMgr->GetOption<bool>("RndBotSync.AllowDownleveling", true);
    g_IgnoreGuildBotsWithRealPlayers = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreGuildBotsWithRealPlayers", true);
    g_IgnoreFriendListed = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreFriendListed", true);
    g_IgnoreArenaTeamBots = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreArenaTeamBots", true);
    g_IgnoreGroupedBots = sConfigMgr->GetOption<bool>("RndBotSync.IgnoreGroupedBots", true);
    g_GuildCacheRefreshFrequency = std::max<uint32>(60, std::min<uint32>(
        sConfigMgr->GetOption<uint32>("RndBotSync.GuildCacheRefreshFrequency", 600), 86400));
    g_DkProgressionEnabled = sConfigMgr->GetOption<bool>("RndBotSync.DeathKnightProgression.Enabled", false);
    g_DkRequiredState = sConfigMgr->GetOption<uint32>("RndBotSync.DeathKnightProgression.RequiredState", 13);
    g_IlvlSyncEnabled = sConfigMgr->GetOption<bool>("RndBotSync.IlvlSync.Enabled", true);
    g_AreaSyncEnabled = sConfigMgr->GetOption<bool>("RndBotSync.AreaSync.Enabled", true);
    uint32 const areaSyncZone = sConfigMgr->GetOption<uint32>("RndBotSync.AreaSync.ZoneId", 0);
    g_AreaSyncZone = areaSyncZone;
    g_AreaSyncUsePlayerZone = (areaSyncZone == 0);
    g_ZoneSpreadPercent = std::min<uint32>(sConfigMgr->GetOption<uint32>("RndBotSync.ZoneSpreadPercent", 30), 100);
    g_FactionSyncEnabled = sConfigMgr->GetOption<bool>("RndBotSync.FactionSync.Enabled", true);
    g_ConfiguredAllianceRatio = sConfigMgr->GetOption<uint32>("Playerbots.RandomBotAllianceRatio",
        sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotAllianceRatio", 50));
    g_ConfiguredHordeRatio = sConfigMgr->GetOption<uint32>("Playerbots.RandomBotHordeRatio",
        sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotHordeRatio", 50));
    // Read directly from config so it is order-independent of mod-playerbots and
    // reflects the user's configured value (the cap we restore to when off).
    g_OriginalGearScoreLimit = sConfigMgr->GetOption<int32>("Playerbots.RandomGearScoreLimit",
        sConfigMgr->GetOption<int32>("AiPlayerbot.RandomGearScoreLimit", 0));
    // playerbots' own "teleport random bots into a real player's zone" filter; see
    // UpdatePlayerZoneTeleport(). Read from config so switching AreaSync off restores the
    // user's value rather than a stale override. Key is the old spelling; playerbots'
    // camelCase rename moved the C++ member, so accept that conf spelling too.
    g_ConfiguredPlayerZoneTeleport = sConfigMgr->GetOption<bool>("Playerbots.RandomBotConcentrateInPlayerZone",
        sConfigMgr->GetOption<bool>("Playerbots.randomBotConcentrateInPlayerZone", false));
    g_FullDebug = sConfigMgr->GetOption<bool>("RndBotSync.FullDebugMode", false);
    g_LiteDebug = sConfigMgr->GetOption<bool>("RndBotSync.LiteDebugMode", false);

    g_RandomBotMinLevel = static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.RandomBotMinLevel",
        sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotMinLevel", 1)));
    g_RandomBotMaxLevel = static_cast<uint8>(sConfigMgr->GetOption<uint32>("Playerbots.RandomBotMaxLevel",
        sConfigMgr->GetOption<uint32>("AiPlayerbot.RandomBotMaxLevel", 80)));
    g_RandomBotAccountPrefix = sConfigMgr->GetOption<std::string>("Playerbots.RandomBotAccountPrefix",
        sConfigMgr->GetOption<std::string>("AiPlayerbot.RandomBotAccountPrefix", "rndbot"));

    // mod-playerbots ships its own random-bot re-leveling (level brackets, level reset). Running
    // either alongside this module means two systems fighting over every bot's level, so stand
    // down. Order-independent of mod-playerbots' own init. A missing option falls back to the
    // default without a warning, so both the current spelling (Playerbots.*) and the old one
    // (AiPlayerbot.*) are listed.
    for (char const* option : { "Playerbots.LevelBrackets.Enabled", "Playerbots.ResetBotLevel.Enabled",
                                "AiPlayerbot.LevelBrackets.Enabled", "AiPlayerbot.ResetBotLevel.Enabled" })
    {
        if (g_Enabled && sConfigMgr->GetOption<bool>(option, false))
        {
            LOG_ERROR("server.loading",
                      "[RndBotSync] Disabling: {} is enabled. mod-playerbots re-levels random bots itself; "
                      "running both fights over every bot's level. Set one of the two to 0.", option);
            g_Enabled = false;
        }
    }

    std::string excludeNames = sConfigMgr->GetOption<std::string>("RndBotSync.ExcludeNames", "");
    g_ExcludeBotNames.clear();
    std::istringstream stream(excludeNames);
    std::string name;
    while (std::getline(stream, name, ','))
    {
        name.erase(std::remove_if(name.begin(), name.end(),
                                  [](unsigned char c) { return ::isspace(c) != 0; }), name.end());
        if (!name.empty())
        {
            std::transform(name.begin(), name.end(), name.begin(),
                           [](unsigned char c) { return static_cast<char>(::tolower(c)); });
            g_ExcludeBotNames.insert(name);
        }
    }
}

// ---- Identity ----------------------------------------------------------------
static std::unordered_set<uint32> g_SocialFriendsList;

// Zone -> [minLevel, maxLevel] over the loaded creature spawns, built once at startup. Backs the
// "match the zone's level" half of area sync.
static std::unordered_map<uint32, std::pair<int32, int32>> CreatureLevelsInZone;

// Fills CreatureLevelsInZone from the object manager's already-loaded creature spawns, so a bot sent to
// a low-level zone can be releveled to match it. Read from memory, not the DB, so it is a one-off pass
// with no query storm. Zones with no spawns stay absent and keep the whole level range.
static void BuildCreatureLevelsInZone()
{
    CreatureLevelsInZone.clear();

    for (auto const& itr : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& spawn = itr.second;
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(spawn.id);
        if (!proto)
        {
            continue;
        }
        // Skip non-combat spawns: a city full of level 1 critters and vendors is not the zone's level.
        if ((proto->npcflag & (UNIT_NPC_FLAG_VENDOR | UNIT_NPC_FLAG_FLIGHTMASTER | UNIT_NPC_FLAG_INNKEEPER)) ||
            proto->faction == 35)
        {
            continue;
        }
        if (proto->minlevel == 0 || proto->minlevel > proto->maxlevel)
        {
            continue;
        }

        uint32 const zoneId = sMapMgr->GetZoneId(PHASEMASK_NORMAL, spawn.mapid, spawn.posX, spawn.posY, spawn.posZ);
        if (!zoneId)
        {
            continue;
        }

        auto& bounds = CreatureLevelsInZone[zoneId];
        if (bounds.second == 0)
        {
            bounds = { proto->minlevel, proto->maxlevel };
            continue;
        }
        bounds.first = std::min(bounds.first, static_cast<int32>(proto->minlevel));
        bounds.second = std::max(bounds.second, static_cast<int32>(proto->maxlevel));
    }
}

static bool IsPlayerRandomBot(Player* player)
{
    return player && sRandomPlayerbotMgr.IsRandomBot(player);
}

// A real (human) player, as opposed to any mod-playerbots bot. Reads the bot
// flag set on the session at creation, so it is reliable even before a bot's
// PlayerbotAI is registered (which only happens after the login hook fires).
//
// Deliberately not mod-playerbots' own ::IsRealPlayer (PlayerbotAI.h): that one
// tests !GET_PLAYERBOT_AI(player), which reports bots as human during
// OnPlayerLogin and would let a bot become the level target. Named differently
// so this never collides with that declaration again.
static bool IsHumanPlayer(Player* player)
{
    return player && player->GetSession() && !player->GetSession()->IsHeadless();
}

// ---- Exclusions --------------------------------------------------------------
static void LoadSocialFriendList()
{
    g_SocialFriendsList.clear();
    // character_social.friend is the low GUID (SocialMgr writes friendGuid.GetCounter(),
    // SocialMgr.cpp). Test the friend bit instead of the sign/width of the flags column.
    QueryResult result = CharacterDatabase.Query(
        "SELECT friend FROM character_social WHERE (flags & {}) <> 0", uint32(SOCIAL_FLAG_FRIEND));
    if (!result)
    {
        return;
    }
    do
    {
        g_SocialFriendsList.insert(result->Fetch()->Get<uint32>());
    } while (result->NextRow());
}

static bool BotInFriendList(Player* bot)
{
    return bot && g_SocialFriendsList.count(LowGuidValue(bot)) > 0;
}

static bool BotInArenaTeam(Player* bot)
{
    if (!bot)
    {
        return false;
    }
    for (uint32 slot = 0; slot < MAX_ARENA_SLOT; ++slot)
    {
        if (sArenaTeamMgr->GetArenaTeamById(bot->GetArenaTeamId(slot)))
        {
            return true;
        }
    }
    return false;
}

static bool BotNameExcluded(Player* bot)
{
    if (g_ExcludeBotNames.empty())
    {
        return false;
    }
    std::string name = bot->GetName();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(::tolower(c)); });
    return g_ExcludeBotNames.count(name) > 0;
}

// ---- Real-player accounts / guild cache --------------------------------------
static std::unordered_set<uint32> g_RealPlayerGuildIds;

// Builds a comma-separated list of account ids that are NOT random-bot accounts
// (username does not start with the configured prefix). Returns false if the
// prefix is empty or there are no such accounts.
static bool GetRealPlayerAccountIds(std::vector<uint32>& accountIdsOut)
{
    accountIdsOut.clear();
    if (g_RandomBotAccountPrefix.empty())
    {
        LOG_WARN("server.loading", "[RndBotSync] RandomBotAccountPrefix is empty; cannot identify real accounts.");
        return false;
    }

    // LEFT() counts characters of the stored username, so the length is the raw prefix
    // length; the escaped form is only used for the literal comparison below.
    uint32 const prefixLength = static_cast<uint32>(g_RandomBotAccountPrefix.size());
    std::string prefix = g_RandomBotAccountPrefix;
    LoginDatabase.EscapeString(prefix); // in-place, returns void

    QueryResult result = LoginDatabase.Query("SELECT id FROM account WHERE LEFT(username, {}) <> '{}'", prefixLength, prefix);
    if (!result)
    {
        return false;
    }
    do
    {
        accountIdsOut.push_back(result->Fetch()->Get<uint32>());
    } while (result->NextRow());

    return !accountIdsOut.empty();
}

// Runs `body` with a parameter list of at most 1000 account ids per query, so a large realm
// cannot build one IN(...) string big enough to hit max_allowed_packet or the query length cap.
// The ids do not repeat across chunks, so callers can accumulate results without de-duping.
template <typename Body>
static void ForEachAccountIdChunk(std::vector<uint32> const& accountIds, Body&& body)
{
    constexpr size_t chunkSize = 1000;
    for (size_t start = 0; start < accountIds.size(); start += chunkSize)
    {
        size_t const stop = std::min(start + chunkSize, accountIds.size());
        std::string chunk;
        for (size_t i = start; i < stop; ++i)
        {
            if (!chunk.empty())
            {
                chunk += ',';
            }
            chunk += std::to_string(accountIds[i]);
        }
        body(chunk);
    }
}

// Rebuilds the set of guild ids that have at least one real-player member
// (online or offline). Membership-based, so offline guildmates still count.
static void RefreshRealPlayerGuildCache()
{
    g_RealPlayerGuildIds.clear();
    std::vector<uint32> accountIds;
    if (!GetRealPlayerAccountIds(accountIds))
    {
        return;
    }

    ForEachAccountIdChunk(accountIds, [](std::string const& chunk)
    {
        QueryResult result = CharacterDatabase.Query(
            "SELECT DISTINCT gm.guildid FROM guild_member gm "
            "JOIN characters c ON c.guid = gm.guid WHERE c.account IN ({})",
            chunk);
        if (!result)
        {
            return;
        }
        do
        {
            g_RealPlayerGuildIds.insert(result->Fetch()->Get<uint32>());
        } while (result->NextRow());
    });

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Real-player guild cache: {} guild(s).", g_RealPlayerGuildIds.size());
    }
}

static bool BotInRealPlayerGuild(Player* bot)
{
    if (!bot)
    {
        return false;
    }
    uint32 guildId = bot->GetGuildId();
    return guildId != 0 && g_RealPlayerGuildIds.count(guildId) > 0;
}

// ---- Death Knight progression gate ------------------------------------------
// Sticky for the session: once unlocked, never re-locks.
static bool g_DkUnlocked = false;

// Whether the gate has been applied with the current g_DkProgressionEnabled value, so the
// per-login scan is skipped while the gate is already known to be satisfied.
static bool g_DkApplied = false;

static void EvaluateDeathKnightGate()
{
    // Capture mod-playerbots' own setting on the first call, before this module ever writes
    // to it, so "off" restores the configured value rather than a stale override.
    if (!g_OriginalDkCaptured)
    {
        g_OriginalDisableDkLogin = sPlayerbotAIConfig.disableDeathKnightLogin;
        g_OriginalDkCaptured = true;
    }

    // Switching the feature off (e.g. `.rndbotsync reload`) must give mod-playerbots its own
    // setting back, not leave this module's override latched in for the rest of the session.
    // The first capture happens above, where the module has not written the field yet, so the
    // restored value is the configured one.
    if (!g_DkProgressionEnabled)
    {
        g_DkUnlocked = false;
        sPlayerbotAIConfig.disableDeathKnightLogin = g_OriginalDisableDkLogin;
        return;
    }

    if (g_DkUnlocked)
    {
        sPlayerbotAIConfig.disableDeathKnightLogin = false;
        return;
    }

    std::vector<uint32> accountIds;
    if (!GetRealPlayerAccountIds(accountIds))
    {
        sPlayerbotAIConfig.disableDeathKnightLogin = true;
        return;
    }

    uint32 const requiredQuest = 66000 + g_DkRequiredState;
    for (uint32 const accountId : accountIds)
    {
        // One account per query: this gate runs on a single login, so it costs at most one
        // row fetch per real account and cannot build a huge IN(...) list.
        if (CharacterDatabase.Query(
                "SELECT 1 FROM character_queststatus_rewarded qr "
                "JOIN characters c ON c.guid = qr.guid "
                "WHERE qr.quest = {} AND c.account = {} LIMIT 1", requiredQuest, accountId))
        {
            g_DkUnlocked = true;
            break;
        }
    }

    sPlayerbotAIConfig.disableDeathKnightLogin = !g_DkUnlocked;

    if (g_DkUnlocked)
    {
        if (g_FullDebug || g_LiteDebug)
        {
            LOG_INFO("server.loading", "[RndBotSync] DK gate met (state {}); Death Knight bots unlocked.", g_DkRequiredState);
        }
    }
    else if (g_FullDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] DK gate not met (state {}); Death Knight bots blocked.", g_DkRequiredState);
    }
}

// True if this bot must NOT be adjusted.
static bool IsBotExcluded(Player* bot)
{
    if (g_IgnoreGuildBotsWithRealPlayers && BotInRealPlayerGuild(bot))
    {
        return true;
    }
    if (g_IgnoreFriendListed && BotInFriendList(bot))
    {
        return true;
    }
    if (g_IgnoreArenaTeamBots && BotInArenaTeam(bot))
    {
        return true;
    }
    if (g_IgnoreGroupedBots && bot && bot->GetGroup())
    {
        return true;
    }
    if (BotNameExcluded(bot))
    {
        return true;
    }
    return false;
}

// Average of the best owned item level per equipment slot (equipped + carried
// bags; bank excluded). Robust to transient situational equips: a real weapon
// sitting in a bag still wins the main-hand slot over an equipped fishing pole.
// Returns 0 if the player owns nothing equippable.
static uint32 GetBestOwnedItemLevel(Player* player)
{
    if (!player)
    {
        return 0;
    }

    uint32 bestPerSlot[EQUIPMENT_SLOT_END] = { 0 };

    auto consider = [&](Item* item)
    {
        if (!item)
        {
            return;
        }
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
        {
            return;
        }
        // Small isEquipable check: only weapons/armor that this player can slot.
        if (proto->Class != ITEM_CLASS_WEAPON && proto->Class != ITEM_CLASS_ARMOR)
        {
            return;
        }
        uint8 slot = player->FindEquipSlot(proto, NULL_SLOT, true);
        if (slot >= EQUIPMENT_SLOT_END)
        {
            return; // NULL_SLOT / not equippable by this player
        }
        if (proto->ItemLevel > bestPerSlot[slot])
        {
            bestPerSlot[slot] = proto->ItemLevel;
        }
    };

    // Equipped slots.
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    }
    // Backpack.
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        consider(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    }
    // Equipped bags.
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag* pBag = player->GetBagByPos(bag);
        if (!pBag)
        {
            continue;
        }
        for (uint32 j = 0; j < pBag->GetBagSize(); ++j)
        {
            consider(player->GetItemByPos(bag, static_cast<uint8>(j)));
        }
    }

    uint32 sum = 0;
    uint32 count = 0;
    for (uint8 slot = 0; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (bestPerSlot[slot] > 0)
        {
            sum += bestPerSlot[slot];
            ++count;
        }
    }
    return count ? sum / count : 0;
}

// Sets or restores the global gear-score cap so mod-playerbots caps all bot gear
// (default bot init and re-randomize) at the target player's item level. Same
// runtime-override pattern as the DK feature's disableDeathKnightLogin.
static void UpdateGlobalGearCap(Player* targetPlayer)
{
    if (!g_IlvlSyncEnabled || !targetPlayer)
    {
        sPlayerbotAIConfig.randomGearScoreLimit = g_OriginalGearScoreLimit;
        return;
    }

    uint32 ilvl = GetBestOwnedItemLevel(targetPlayer);
    if (ilvl == 0)
    {
        // Player owns nothing equippable: don't set 0 (mod-playerbots treats 0 as
        // "unlimited"); leave the configured cap in place.
        sPlayerbotAIConfig.randomGearScoreLimit = g_OriginalGearScoreLimit;
        return;
    }

    // mod-playerbots only applies the cap to green+ candidates; whites bypass it.
    // Leveling gear of every quality sits at ilvl = required level + 5, so a cap
    // at a leveling player's average ilvl starves the bots kept at/above their
    // level of greens and blues and they come out in whites. Floor the cap at
    // what a bot at the level ceiling needs; once the player outgears the
    // leveling curve (dungeons, raids) their own ilvl takes over.
    int32 const capFloor = static_cast<int32>(targetPlayer->GetLevel() + g_TargetBand) + 5;
    sPlayerbotAIConfig.randomGearScoreLimit = std::max<int32>(capFloor, static_cast<int32>(ilvl));

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Gear cap set to item level {} (player '{}').",
                 sPlayerbotAIConfig.randomGearScoreLimit, targetPlayer->GetName());
    }
}

// Turns playerbots' own teleport filter on while area sync is enabled. This module only moves
// a few bots per pass, and playerbots re-teleports every random bot on its own timer anyway
// (including the 1800s push after a move below), which previously scattered the gathered
// population straight back out. With the filter on, each of those teleports lands in a zone a
// real player is standing in, so the zone fills at teleport rate instead of pass rate.
static void UpdatePlayerZoneTeleport()
{
    bool const wanted = g_ConfiguredPlayerZoneTeleport || g_AreaSyncEnabled;
    if (sPlayerbotAIConfig.randomBotConcentrateInPlayerZone == wanted)
    {
        return;
    }

    sPlayerbotAIConfig.randomBotConcentrateInPlayerZone = wanted;

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Playerbots zone teleport filter set to {}.", wanted);
    }
}

// The set of teams the online real players are on. Empty when nobody real is online.
static std::pair<bool, bool> CollectRealPlayerTeams()
{
    bool alliance = false;
    bool horde = false;
    for (auto const& itr : ObjectAccessor::GetPlayers())
    {
        Player* player = itr.second;
        if (!player || !player->IsInWorld() || !IsHumanPlayer(player))
        {
            continue;
        }
        if (player->GetTeamId() == TEAM_ALLIANCE)
        {
            alliance = true;
        }
        else if (player->GetTeamId() == TEAM_HORDE)
        {
            horde = true;
        }
    }
    return { alliance, horde };
}

// Drives playerbots' random-bot faction ratios so every bot is on a team a real player is
// playing. On a one-faction realm this is what stops half the bot population from logging in
// as the empty faction, and it also pins the race a newly created bot character rolls
// (RandomPlayerbotFactory::CreateRandomBot picks the team by coin flip). Both teams online,
// or FactionSync switched off, restores the configured ratio.
static void UpdateFactionRatio()
{
    auto const [allianceOnline, hordeOnline] = CollectRealPlayerTeams();
    bool const mirror = g_FactionSyncEnabled && (allianceOnline != hordeOnline);

    uint32 const allianceWanted = mirror ? (allianceOnline ? 1u : 0u) : g_ConfiguredAllianceRatio;
    uint32 const hordeWanted = mirror ? (hordeOnline ? 1u : 0u) : g_ConfiguredHordeRatio;

    if (sPlayerbotAIConfig.randomBotAllianceRatio == allianceWanted &&
        sPlayerbotAIConfig.randomBotHordeRatio == hordeWanted)
    {
        return;
    }

    sPlayerbotAIConfig.randomBotAllianceRatio = allianceWanted;
    sPlayerbotAIConfig.randomBotHordeRatio = hordeWanted;

    if (g_FullDebug || g_LiteDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Bot faction ratio set to {}A/{}H (alliance online {}, horde online {}).",
                 allianceWanted, hordeWanted, +allianceOnline, +hordeOnline);
    }
}

// ---- Adjustment pass ---------------------------------------------------------

// True if the bot's transient state allows a level change right now. Persistent
// exclusions are handled separately by IsBotExcluded. Caller guarantees a
// non-null, in-world bot.
static bool IsBotSafeToAdjust(Player* bot)
{
    if (!bot->IsAlive())
    {
        return false;
    }
    if (bot->IsInCombat())
    {
        return false;
    }
    if (bot->InBattleground() || bot->InArena() || bot->inRandomLfgDungeon() || bot->InBattlegroundQueue())
    {
        return false;
    }
    if (bot->IsInFlight())
    {
        return false;
    }
    return true;
}

// Same guards mod-playerbots applies to its own bot teleports. A map id that is
// not in Playerbots.RandomBotMaps is refused so area sync cannot undo the admin's
// own "bots do not go there" list.
static bool IsBotSafeToTeleport(Player* bot)
{
    if (!bot->IsAlive() || bot->IsInCombat() || bot->IsBeingTeleported() || bot->IsRooted())
    {
        return false;
    }
    if (bot->InBattleground() || bot->InArena() || bot->InBattlegroundQueue() || bot->IsInFlight())
    {
        return false;
    }
    // In a group, only a bot leading its own group may be moved; otherwise it would
    // be yanked away from the group it follows.
    if (bot->GetGroup() && !bot->GetGroup()->IsLeader(bot->GetGUID()))
    {
        return false;
    }
    return true;
}

// True if the map is one mod-playerbots itself allows bots to be teleported to.
static bool BotMapAllowed(uint32 mapId)
{
    auto const& maps = sPlayerbotAIConfig.randomBotMaps;
    return std::find(maps.begin(), maps.end(), mapId) != maps.end();
}

// Picks a place in `zoneId` for a bot to land: mod-playerbots' own teleport locations for
// the bot's level that sit in that zone on the player's map, shuffled so the bots spread
// out instead of stacking on one spot.
//
// shortcut: the level-keyed location cache only covers maps listed in Playerbots.RandomBotMaps;
// a zone outside that list (a dungeon, a battleground, a map the admin excluded) finds no
// location and the bot is skipped. Extend with map grid generation if that ever matters.
static bool FindZoneTeleportPosition(Player* bot, Player* targetPlayer, uint32 zoneId, Position& out)
{
    Map* map = targetPlayer->GetMap();
    uint32 const mapId = map->GetId();

    std::vector<Position> candidates;
    for (WorldLocation const& loc : sTravelMgr.GetTeleportLocations(bot))
    {
        if (loc.GetMapId() != mapId)
        {
            continue;
        }
        if (map->GetZoneId(PHASEMASK_NORMAL, loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ()) != zoneId)
        {
            continue;
        }
        candidates.emplace_back(loc.GetPositionX(), loc.GetPositionY(), loc.GetPositionZ());
    }

    if (candidates.empty())
    {
        return false;
    }

    std::shuffle(candidates.begin(), candidates.end(), RandomEngine::Instance());

    // Ground and water checks use the bot's own phase mask, because the bot is the one
    // landing; a candidate the bot cannot stand on is skipped, not moved to a fallback.
    for (Position const& pos : candidates)
    {
        float const z = map->GetHeight(bot->GetPhaseMask(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ() + 0.5f);
        if (z <= INVALID_HEIGHT ||
            map->IsInWater(bot->GetPhaseMask(), pos.GetPositionX(), pos.GetPositionY(), z, bot->GetCollisionHeight()))
        {
            continue;
        }
        out.Relocate(pos.GetPositionX(), pos.GetPositionY(), z + 0.05f);
        return true;
    }

    return false;
}

// Teleports a bot into the target player's zone (same zone id, same map) onto one of
// mod-playerbots' own teleport locations for that zone. Instances are skipped: a
// WorldLocation carries no instance id, so a bot would land in another instance of the
// same map. Returns false when the bot is already in the zone or cannot be moved.
static bool MoveBotToPlayerZone(Player* bot, Player* targetPlayer, uint32 zoneId)
{
    if (!zoneId || !bot || !targetPlayer || !bot->IsInWorld() || !bot->GetSession() ||
        bot->GetSession()->IsLoggingOut() || bot->IsDuringRemoveFromWorld())
    {
        return false;
    }

    Map* map = targetPlayer->GetMap();
    if (!map || map->Instanceable() || !BotMapAllowed(map->GetId()))
    {
        return false;
    }
    if (bot->GetMapId() == map->GetId() && bot->GetZoneId() == zoneId)
    {
        return false;
    }
    if (!IsBotSafeToTeleport(bot))
    {
        return false;
    }

    Position pos;
    if (!FindZoneTeleportPosition(bot, targetPlayer, zoneId, pos))
    {
        return false;
    }
    // Teleport first: TeleportTo can refuse (disabled map, expansion, invalid coords), and
    // clearing the motion master / resetting the AI before a refusal would leave an idle bot.
    if (!bot->TeleportTo(map->GetId(), pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
                         targetPlayer->GetOrientation()))
    {
        return false;
    }
    // shortcut: a bot in a phase the player is not in lands invisibly in the same zone;
    // open-world random bots are unphased, so only fix this if phased zones become common.
    bot->GetMotionMaster()->Clear();
    if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(bot))
    {
        botAI->Reset(true);
    }
    bot->RemoveAurasWithInterruptFlags(AURA_INTERRUPT_FLAG_TELEPORTED | AURA_INTERRUPT_FLAG_CHANGE_MAP);
    bot->SendMovementFlagUpdate();

    // mod-playerbots re-teleports every random bot on its own long timer; push that back so
    // the bot does not immediately wander off again.
    sRandomPlayerbotMgr.ScheduleTeleport(bot->GetGUID().GetCounter(), 1800);

    if (g_FullDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Moved bot '{}' into zone {} of '{}' (map {}).",
                 bot->GetName(), zoneId, targetPlayer->GetName(), map->GetId());
    }
    return true;
}

// Sets a single bot's level to a random value in [lower, upper] and re-randomizes
// gear/stats. Used both to raise (promote) and lower (downlevel) a bot. Death
// Knights are floored at the heroic start level; if the range is entirely below
// it the bot is skipped. Returns false (no change) for bots that are logging out or not safe
// to change right now (dead, in combat, in an instance/queue, in flight).
static bool SetBotLevelInRange(Player* bot, int lower, int upper)
{
    if (!bot || !bot->IsInWorld() || !bot->GetSession() ||
        bot->GetSession()->IsLoggingOut() || bot->IsDuringRemoveFromWorld())
    {
        return false;
    }

    if (!IsBotSafeToAdjust(bot))
    {
        return false;
    }

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        int const dkMinLevel = static_cast<int>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        if (upper < dkMinLevel)
        {
            return false;
        }
        if (lower < dkMinLevel)
        {
            lower = dkMinLevel;
        }
    }
    if (lower > upper)
    {
        return false;
    }

    uint8 newLevel = static_cast<uint8>(urand(lower, upper));

    if (bot->IsMounted())
    {
        bot->Dismount();
    }

    PlayerbotFactory factory(bot, newLevel);
    factory.Randomize(false);

    // Known mod-playerbots quirk: re-init talents when rolling to max level with
    // equip/spec persistence enabled.
    if (newLevel == g_RandomBotMaxLevel && sPlayerbotAIConfig.equipAndSpecPersistence)
    {
        PlayerbotFactory talentFactory(bot, newLevel);
        talentFactory.InitTalentsTree(false, true, true);
    }

    if (g_FullDebug)
    {
        LOG_INFO("server.loading", "[RndBotSync] Set bot '{}' to level {}.", bot->GetName(), newLevel);
    }
    return true;
}

// ---- Adjustment pass ---------------------------------------------------------

// ---- Adjustment pass ---------------------------------------------------------

// Enforces the ceiling: pulls bots above it down. Returns the number changed.
static uint32 DownlevelAboveCeiling(std::vector<Player*> const& eligible, int bandLower, int ceiling,
                                    bool unlimited, uint32& budget)
{
    std::vector<Player*> aboveCeiling;
    for (Player* bot : eligible)
    {
        if (static_cast<int>(bot->GetLevel()) > ceiling)
        {
            aboveCeiling.push_back(bot);
        }
    }

    // Highest first: bring the most out-of-place bots down first.
    std::sort(aboveCeiling.begin(), aboveCeiling.end(), [](Player* a, Player* b)
    {
        return a->GetLevel() > b->GetLevel();
    });

    uint32 changed = 0;
    for (Player* bot : aboveCeiling)
    {
        if (!unlimited && budget == 0)
        {
            break;
        }
        // A bot sitting at the level of the zone it is in is where area sync wants it: leave it alone.
        if (BotSuitsItsZone(bot))
        {
            continue;
        }
        // Scatter across [RandomBotMinLevel, ceiling] to keep a level spread. A bot the
        // player deliberately guilded is a companion, not filler: keep it in the band instead
        // (only reachable with IgnoreGuildBotsWithRealPlayers = 0). Unsafe bots (combat,
        // instance, flight, ...) are skipped without spending budget.
        int const lower = BotInRealPlayerGuild(bot) ? bandLower : static_cast<int>(g_RandomBotMinLevel);
        if (SetBotLevelInRange(bot, lower, ceiling))
        {
            ++changed;
            if (!unlimited)
            {
                --budget;
            }
        }
    }
    return changed;
}

// Concentrates about TargetPercent% of the population inside the band by raising the
// lowest-level bots into it. Returns the number changed.
static uint32 PromoteIntoBand(std::vector<Player*> const& eligible, int bandLower, int ceiling,
                              uint32 desiredNear, bool unlimited, uint32& budget)
{
    uint32 currentNear = 0;
    std::vector<Player*> belowBand;
    for (Player* bot : eligible)
    {
        int const level = static_cast<int>(bot->GetLevel());
        if (level >= bandLower && level <= ceiling)
        {
            ++currentNear;
        }
        else if (level < bandLower)
        {
            belowBand.push_back(bot);
        }
    }

    if (currentNear >= desiredNear)
    {
        return 0;
    }

    uint32 need = desiredNear - currentNear;

    // Lowest-level first, so the population moves upward into the band.
    std::sort(belowBand.begin(), belowBand.end(), [](Player* a, Player* b)
    {
        return a->GetLevel() < b->GetLevel();
    });

    uint32 changed = 0;
    for (Player* bot : belowBand)
    {
        if (need == 0 || (!unlimited && budget == 0))
        {
            break;
        }
        // Same rule as the downlevel step: a bot at its zone's level is not a straggler to pull up.
        if (BotSuitsItsZone(bot))
        {
            continue;
        }
        // Unsafe bots are skipped without spending budget or the deficit.
        if (SetBotLevelInRange(bot, bandLower, ceiling))
        {
            ++changed;
            --need;
            if (!unlimited)
            {
                --budget;
            }
        }
    }
    return changed;
}

// Moves the nearest bots standing outside the target zone, or inside it above the level band, onto a
// teleport location in the zone at a level inside the band. "Near" is the combined quota: in the zone
// AND playable with the target player, so a gathered bot is one the player can actually group with. A
// moved bot is levelled into the band first, because the teleport can refuse and a refused level change
// is harmless while a refused teleport after a level change would leave the deficit unpaid.
// Returns the number moved.
static uint32 GatherIntoZone(std::vector<Player*> const& eligible, Player* targetPlayer, uint32 zoneId,
                             int bandLower, int ceiling, uint32 desiredNear, bool unlimited, uint32& areaBudget)
{
    auto inZone = [&](Player* bot)
    {
        return bot->GetMapId() == targetPlayer->GetMapId() && bot->GetZoneId() == zoneId;
    };
    auto inBand = [&](Player* bot)
    {
        int const level = static_cast<int>(bot->GetLevel());
        return level >= bandLower && level <= ceiling;
    };

    uint32 currentNear = 0;
    std::vector<Player*> outsideZone;
    for (Player* bot : eligible)
    {
        // A bot standing in the zone at a level that suits the zone is local colour the player can see:
        // it counts towards the quota, so the pass does not summon a second bot on top of it. Only bots
        // that cannot be counted this way need to be brought up to the player's band instead.
        if (inZone(bot) && (inBand(bot) || BotSuitsItsZone(bot)))
        {
            ++currentNear;
        }
        else
        {
            outsideZone.push_back(bot);
        }
    }

    if (currentNear >= desiredNear)
    {
        return 0;
    }

    uint32 need = desiredNear - currentNear;

    // Nearest first: the bots already in the general region converge before the ones on the
    // other side of the world, so the local population grows visibly.
    std::sort(outsideZone.begin(), outsideZone.end(), [&](Player* a, Player* b)
    {
        bool const sameMapA = a->GetMapId() == targetPlayer->GetMapId();
        bool const sameMapB = b->GetMapId() == targetPlayer->GetMapId();
        if (sameMapA != sameMapB)
        {
            return sameMapA;
        }
        if (!sameMapA)
        {
            return false;
        }
        return a->GetExactDist(targetPlayer) < b->GetExactDist(targetPlayer);
    });

    uint32 changed = 0;
    for (Player* bot : outsideZone)
    {
        if (need == 0 || (!unlimited && areaBudget == 0))
        {
            break;
        }
        // In-zone bots the town keeps: their level already suits the zone they stand in.
        if (inZone(bot) && BotSuitsItsZone(bot))
        {
            continue;
        }

        // In the zone but out of step with it: no teleport, just bring it to the player's level, which
        // is what makes it one of the bots the player can group with.
        if (inZone(bot))
        {
            if (SetBotLevelInRange(bot, bandLower, ceiling))
            {
                ++changed;
                --need;
                if (!unlimited)
                {
                    --areaBudget;
                }
            }
            continue;
        }

        // Out of the zone: relevel it, then move it in. A spread bot is levelled to the zone's own band
        // first - it has to be, because the teleport location is picked from the bot's own level pool,
        // so a level 80 bot has no location inside a level 10 zone to be sent to at all.
        int botBandLower = 0;
        int botCeiling = 0;
        bool botSpread = false;
        ResolveGatherBand(bot, zoneId, bandLower, ceiling, botBandLower, botCeiling, botSpread);
        int const botLevel = static_cast<int>(bot->GetLevel());
        bool const botInBand = botLevel >= botBandLower && botLevel <= botCeiling;
        // A bot that cannot be releveled (a low Death Knight, a clamped one) is left where it is rather
        // than dropped into a zone where it would be out of place.
        if (!botInBand && !SetBotLevelInRange(bot, botBandLower, botCeiling))
        {
            continue;
        }
        // Unsafe, unreachable or zone-less bots are skipped without spending budget or the deficit.
        if (MoveBotToPlayerZone(bot, targetPlayer, zoneId))
        {
            ++changed;
            --need;
            if (!unlimited)
            {
                --areaBudget;
            }
        }
    }
    return changed;
}

// True for the ZoneSpreadPercent share of the population. The roll is recomputed every time a bot is
// looked at, so the same bot drifts between "the player's level" and "the zone's level" instead of the
// two groups being fixed for the session.
static bool RollZoneSpread()
{
    return g_ZoneSpreadPercent > 0 && urand(1, 100) <= g_ZoneSpreadPercent;
}

// A bot's level range when nothing about the zone is known: the world cap and playerbots' own limits.
static void WorldLevelRange(int& lowOut, int& highOut)
{
    int const worldMax = static_cast<int>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL));
    lowOut = static_cast<int>(g_RandomBotMinLevel);
    highOut = std::min(worldMax, static_cast<int>(g_RandomBotMaxLevel));
}

// The band a spread bot is releveled to: the level of the zone it lands in, +/- TargetBand, clamped
// to the zone's own [minLevel, maxLevel] from the creature spawns and then to the world range. An
// unknown zone (no spawn map, no believable bracket) falls back to the whole world range.
static void ComputeZoneLevelBand(uint32 zoneId, int& bandLowerOut, int& ceilingOut)
{
    int low = 0;
    int high = 0;
    WorldLevelRange(low, high);

    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId))
    {
        if (int const areaLevel = static_cast<int>(area->area_level))
        {
            low = areaLevel - static_cast<int>(g_TargetBand);
            high = areaLevel + static_cast<int>(g_TargetBand);
        }
    }

    auto spawns = CreatureLevelsInZone.find(zoneId);
    if (spawns != CreatureLevelsInZone.end())
    {
        low = std::max(low, spawns->second.first);
        high = std::min(high, spawns->second.second);
    }

    int worldLow = 0;
    int worldHigh = 0;
    WorldLevelRange(worldLow, worldHigh);
    bandLowerOut = std::max(std::min(low, high), worldLow);
    ceilingOut = std::min(std::max(low, high), worldHigh);
    if (bandLowerOut > ceilingOut)
    {
        bandLowerOut = worldLow;
        ceilingOut = worldHigh;
    }
}

// Resolves the band for one bot about to be gathered: the player's band, or the zone's own band for
// the spread share. Rolling only when the bot is actually worked on keeps the player's band accurate
// for the rest of the pass.
static void ResolveGatherBand(Player* bot, uint32 zoneId, int playerBandLower, int playerCeiling,
                              int& bandLowerOut, int& ceilingOut, bool& spreadOut)
{
    spreadOut = RollZoneSpread();
    if (spreadOut)
    {
        ComputeZoneLevelBand(zoneId, bandLowerOut, ceilingOut);
    }
    else
    {
        bandLowerOut = playerBandLower;
        ceilingOut = playerCeiling;
    }

    if (bot->getClass() == CLASS_DEATH_KNIGHT)
    {
        int const dkMinLevel = static_cast<int>(sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL));
        bandLowerOut = std::max(bandLowerOut, dkMinLevel);
        ceilingOut = std::max(ceilingOut, dkMinLevel);
    }
}

// True when this bot's level already suits the zone it is standing in: it is a spread bot (the current
// roll says so) and its level is inside that zone's band. The level steps skip such a bot, which is what
// keeps a bot spread down to a low-level zone from being promoted straight back up on the next pass and
// bounced between the two bands on every timer tick.
static bool BotSuitsItsZone(Player* bot)
{
    if (g_ZoneSpreadPercent == 0 || !bot->IsInWorld())
    {
        return false;
    }

    // Only a zone with real creature levels can be "suit"ed; for any other zone the band above is the
    // whole world range, which every bot fits and which would exempt the entire population.
    uint32 const zoneId = bot->GetZoneId();
    if (!CreatureLevelsInZone.count(zoneId))
    {
        return false;
    }

    int bandLower = 0;
    int ceiling = 0;
    ComputeZoneLevelBand(zoneId, bandLower, ceiling);
    int const level = static_cast<int>(bot->GetLevel());
    return level >= bandLower && level <= ceiling;
}

// Which part of the pass a call performs. The world timer alternates Level and Area across
// the check interval; a login, a level-up or the resync command runs the full pass.
enum class PassPart
{
    Full,   // level steps + area step
    Level,  // downlevel + promote only
    Area    // zone gathering only
};

// One adjustment pass. `part` selects which steps run; `unlimitedBudget` ignores ProcessLimit.
static void RunAdjustmentPass(bool unlimitedBudget = false, PassPart part = PassPart::Full)
{
    if (!g_Enabled)
    {
        return;
    }

    int target = -1;
    Player* const targetPlayer = FindTargetPlayer(target);

    // Item level sync: set the gear cap from the target player (or restore the original when
    // none online). Independent of the level-adjustment steps, so it also runs for an area-only
    // pass.
    UpdateGlobalGearCap(targetPlayer);
    UpdatePlayerZoneTeleport();
    UpdateFactionRatio();

    if (target < 0)
    {
        return; // No real player online: freeze.
    }

    // The band's upper edge is the ceiling: the highest real player's level plus the band,
    // treated as the effective maximum for the whole bot population.
    int bandLower = 0;
    int ceiling = 0;
    ComputeLevelBand(target, bandLower, ceiling);

    // Zone the pass gathers into: resolved once so the whole pass targets one zone.
    uint32 const areaZone = g_AreaSyncEnabled ? ResolveAreaSyncZone(targetPlayer) : 0;

    bool const levelsWanted = (part != PassPart::Area) &&
                              (g_TargetPercent > 0 || g_AllowDownleveling);
    bool const areaWanted = (part != PassPart::Level) && g_AreaSyncEnabled && areaZone != 0;
    if (!levelsWanted && !areaWanted)
    {
        return;
    }

    // One scan: the level steps and the zone step need the same population.
    std::vector<Player*> const eligible = CollectEligibleBots();
    if (eligible.empty())
    {
        return;
    }

    bool const unlimited = unlimitedBudget || (g_ProcessLimit == 0);
    uint32 desiredNear = static_cast<uint32>(std::lround(g_TargetPercent / 100.0 * eligible.size()));

    // Split the finite pass so neither the level steps nor the zone step can starve the other:
    // while bots below the band are plentiful, promotion alone would spend every slot. A pass
    // that runs only one kind of step gets the whole budget for it. When ProcessLimit is 1 there
    // is nothing to split without overspending, so the level steps keep it. ProcessLimit 0 (or
    // resync) means unlimited everywhere, and the budgets below are then never consulted.
    uint32 budget = 0;
    uint32 areaBudget = 0;
    if (!unlimited)
    {
        if (levelsWanted && areaWanted && g_ProcessLimit >= 2)
        {
            areaBudget = g_ProcessLimit / 2;
            budget = g_ProcessLimit - areaBudget;
        }
        else if (levelsWanted)
        {
            budget = g_ProcessLimit;
        }
        else
        {
            areaBudget = g_ProcessLimit;
        }
    }

    uint32 downleveled = 0;
    uint32 promoted = 0;
    uint32 moved = 0;

    if (levelsWanted && g_AllowDownleveling)
    {
        downleveled = DownlevelAboveCeiling(eligible, bandLower, ceiling, unlimited, budget);
    }
    if (levelsWanted && g_TargetPercent > 0 && (unlimited || budget > 0))
    {
        promoted = PromoteIntoBand(eligible, bandLower, ceiling, desiredNear, unlimited, budget);
    }
    if (areaWanted && (unlimited || areaBudget > 0))
    {
        moved = GatherIntoZone(eligible, targetPlayer, areaZone, bandLower, ceiling, desiredNear, unlimited, areaBudget);
    }

    if ((g_FullDebug || g_LiteDebug) && (downleveled > 0 || promoted > 0 || moved > 0))
    {
        LOG_INFO("server.loading", "[RndBotSync] Target {} band [{}-{}], eligible {}, downleveled {}, promoted {}, moved into zone {}: {}.",
                 target, bandLower, ceiling, eligible.size(), downleveled, promoted, areaZone, moved);
    }
}

// ---- Cache refresh helper ----------------------------------------------------
static void RefreshCaches()
{
    LoadSocialFriendList();
    RefreshRealPlayerGuildCache();
}

// ---- WorldScript -------------------------------------------------------------
class RndBotSyncWorldScript : public WorldScript
{
public:
    RndBotSyncWorldScript()
        : WorldScript("RndBotSyncWorldScript"), m_timer(0), m_guildTimer(0) { }

    void OnStartup() override
    {
        LoadConfig();
        if (!g_Enabled)
        {
            LOG_INFO("server.loading", "[RndBotSync] Disabled via configuration.");
            return;
        }
        RefreshCaches();
        BuildCreatureLevelsInZone();
        EvaluateDeathKnightGate();
        g_DkApplied = g_DkProgressionEnabled;
        LOG_INFO("server.loading",
                 "[RndBotSync] Loaded. CheckFrequency {}s, TargetPercent {}%, TargetBand +/-, AreaSync {} (zone {}), FactionSync {}.",
                 g_CheckFrequency, g_TargetPercent, g_TargetBand, g_AreaSyncEnabled,
                 g_AreaSyncUsePlayerZone ? std::string("player's") : std::to_string(g_AreaSyncZone), g_FactionSyncEnabled);
    }

    void OnUpdate(uint32 diff) override
    {
        if (!g_Enabled)
        {
            return;
        }

        // Periods are clamped at load, so the millisecond products cannot overflow. The two
        // timers are deliberately out of phase: the (expensive) cache scan and the pass that
        // moves bots never fire in the same tick, so a pass always measures a fresh cache.
        uint64 const guildPeriod = static_cast<uint64>(g_GuildCacheRefreshFrequency) * 1000;
        m_guildTimer += diff;
        if (m_guildTimer >= guildPeriod)
        {
            // Carry the sub-period remainder instead of dropping it, so the effective
            // interval does not drift. Only the scan runs here, never a pass.
            m_guildTimer %= guildPeriod;
            RefreshCaches();
            if (g_DkProgressionEnabled != g_DkApplied)
            {
                EvaluateDeathKnightGate();
                g_DkApplied = g_DkProgressionEnabled;
            }
        }

        if (g_CheckFrequency == 0)
        {
            return; // Events-only mode.
        }
        uint64 const checkPeriod = static_cast<uint64>(g_CheckFrequency) * 1000;
        m_timer += diff;
        if (m_timer >= checkPeriod)
        {
            m_timer %= checkPeriod;
            // Phase by wall clock rather than by stored state, so a reload cannot invert it.
            // g_CheckFrequency is >= 1 after clamping, so the modulo is always defined.
            bool const scanTick = (static_cast<uint32>(GameTime::GetGameTime().count()) % g_CheckFrequency) <
                                  (g_CheckFrequency / 2);
            RunAdjustmentPass(false, scanTick ? PassPart::Level : PassPart::Area);
        }
    }

private:
    uint64 m_timer;
    uint64 m_guildTimer;
};

// ---- PlayerScript ------------------------------------------------------------
class RndBotSyncPlayerScript : public PlayerScript
{
public:
    RndBotSyncPlayerScript() : PlayerScript("RndBotSyncPlayerScript") { }

    void OnPlayerLogin(Player* player) override
    {
        if (!g_Enabled || !IsHumanPlayer(player))
        {
            return;
        }
        if (g_DkProgressionEnabled && !g_DkUnlocked)
        {
            EvaluateDeathKnightGate();
            g_DkApplied = g_DkProgressionEnabled;
        }
        RunAdjustmentPass();
    }

    void OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/) override
    {
        if (!g_Enabled || !IsHumanPlayer(player))
        {
            return;
        }
        RunAdjustmentPass();
    }
};

// ---- CommandScript -----------------------------------------------------------
class RndBotSyncCommandScript : public CommandScript
{
public:
    RndBotSyncCommandScript() : CommandScript("RndBotSyncCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable table =
        {
            { "rndbotsync reload", HandleReload, SEC_ADMINISTRATOR, Console::Yes },
            { "rndbotsync status", HandleStatus, SEC_ADMINISTRATOR, Console::Yes },
            { "rndbotsync resync", HandleResync, SEC_ADMINISTRATOR, Console::Yes }
        };
        return table;
    }

    static bool HandleReload(ChatHandler* handler)
    {
        LoadConfig();
        RefreshCaches();
        BuildCreatureLevelsInZone();
        EvaluateDeathKnightGate();
        g_DkApplied = g_DkProgressionEnabled;
        UpdateGlobalGearCap(nullptr);
        UpdatePlayerZoneTeleport();
        UpdateFactionRatio();
        handler->SendSysMessage("[RndBotSync] Config reloaded.");
        return true;
    }

    // Read-only snapshot of the current target, band, eligible-bot counts, and
    // the gear/DK state. Changes nothing.
    static bool HandleStatus(ChatHandler* handler)
    {
        int target = -1;
        Player* const targetPlayer = FindTargetPlayer(target);

        handler->PSendSysMessage("[RndBotSync] Enabled: {}, CheckFrequency: {}s, ProcessLimit: {}.",
                                 g_Enabled, g_CheckFrequency, g_ProcessLimit);

        if (target < 0)
        {
            handler->PSendSysMessage("  Target: none online (frozen).");
        }
        else
        {
            int bandLower = 0;
            int ceiling = 0;
            ComputeLevelBand(target, bandLower, ceiling);

            uint32 const botZone = ResolveAreaSyncZone(targetPlayer);
            std::vector<Player*> const eligibleBots = CollectEligibleBots();

            uint32 inBand = 0, aboveCeiling = 0, belowBand = 0, inArea = 0;
            for (Player* bot : eligibleBots)
            {
                int const level = static_cast<int>(bot->GetLevel());
                if (g_AreaSyncEnabled && botZone && bot->GetMapId() == targetPlayer->GetMapId() &&
                    bot->GetZoneId() == botZone &&
                    ((level >= bandLower && level <= ceiling) || BotSuitsItsZone(bot)))
                {
                    ++inArea;
                }
                if (level > ceiling)
                {
                    ++aboveCeiling;
                }
                else if (level < bandLower)
                {
                    ++belowBand;
                }
                else
                {
                    ++inBand;
                }
            }

            uint32 const eligible = static_cast<uint32>(eligibleBots.size());
            uint32 const desiredNear = static_cast<uint32>(std::lround(g_TargetPercent / 100.0 * eligible));

            handler->PSendSysMessage("  Target: {} (level {}).", targetPlayer->GetName(), target);
            handler->PSendSysMessage("  Band: [{}-{}] (ceiling {}), TargetPercent {} -> desired in-band {}.",
                                     bandLower, ceiling, ceiling, g_TargetPercent, desiredNear);
            handler->PSendSysMessage("  Eligible: {} (in-band {}, above-ceiling {}, below-band {}), AllowDownleveling: {}.",
                                     eligible, inBand, aboveCeiling, belowBand, g_AllowDownleveling);
            handler->PSendSysMessage("  AreaSync: {}, zone {} -> in-zone (player band or zone band) {} of desired {}.",
                                     g_AreaSyncEnabled,
                                     g_AreaSyncUsePlayerZone ? std::to_string(botZone) : std::to_string(g_AreaSyncZone),
                                     inArea, desiredNear);
        }

        handler->PSendSysMessage("  IlvlSync: {}, gear cap: {}, target best item level: {}.",
                                 g_IlvlSyncEnabled, sPlayerbotAIConfig.randomGearScoreLimit,
                                 targetPlayer ? GetBestOwnedItemLevel(targetPlayer) : 0);
        handler->PSendSysMessage("  DK gate: {}, {} (RequiredState {}).",
                                 g_DkProgressionEnabled, g_DkUnlocked ? "unlocked" : "locked", g_DkRequiredState);
        auto const [allianceOnline, hordeOnline] = CollectRealPlayerTeams();
        handler->PSendSysMessage("  FactionSync: {} (players online {}A/{}H), bot ratios {}A/{}H.",
                                 g_FactionSyncEnabled, +allianceOnline, +hordeOnline,
                                 sPlayerbotAIConfig.randomBotAllianceRatio, sPlayerbotAIConfig.randomBotHordeRatio);
        return true;
    }

    // Forces an immediate full pass that ignores the ProcessLimit throttle
    // (processes all eligible bots at once). Still respects the level ceiling,
    // gear cap, and exclusions.
    static bool HandleResync(ChatHandler* handler)
    {
        RefreshCaches();
        EvaluateDeathKnightGate();
        g_DkApplied = g_DkProgressionEnabled;
        RunAdjustmentPass(true);
        handler->SendSysMessage("[RndBotSync] Forced full resync complete.");
        return true;
    }
};

// ---- Registration ------------------------------------------------------------
void Addmod_rndbot_syncScripts()
{
    new RndBotSyncWorldScript();
    new RndBotSyncPlayerScript();
    new RndBotSyncCommandScript();
}
