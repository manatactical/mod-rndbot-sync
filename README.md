# ![logo](https://raw.githubusercontent.com/azerothcore/azerothcore.github.io/master/images/logo-github.png) AzerothCore

# mod-rndbot-sync

[![core-build](https://github.com/Yuof/mod-rndbot-sync/actions/workflows/core-build.yml/badge.svg)](https://github.com/Yuof/mod-rndbot-sync/actions/workflows/core-build.yml)

## Description

`mod-rndbot-sync` keeps [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots)
random bots in sync with the highest-level real player online — in **level**,
**gear**, and **progression** — so the world feels populated at *your* level
instead of clustering wherever the default bot pool happens to sit.

Out of the box, mod-playerbots spreads its random bots across the whole 1–80
range. On a populated endgame realm that looks great, but on a small or solo
server the handful of bots near your level is tiny: you level a fresh character
through a near-empty world while the bots idle at max level. This module fixes
that with one idea — **the highest real player online sets the level the bots
gravitate to** — and keeps the rest of the bot world consistent with where that
player actually is.

When no real player is online, the module does nothing: it never churns bots in
an empty world.

## Features

- **Level sync** — keeps a configurable share of random bots within `±TargetBand`
  levels of the highest online real player; promotes the lowest-level bots first
  so the population drifts upward as you level.
- **Level ceiling / downleveling** — the player's level plus the band is a ceiling
  on the whole bot population; bots above it are pulled down, so a lone low-level
  player brings the bots down to their level (optional, on by default).
- **Item-level cap** — caps bot gear at the player's item level so bots never
  out-gear you, measured from the player's best owned gear (robust to transient
  situational equips like a fishing pole).
- **Area sync** — progressively moves random bots that are standing outside the
  highest online real player's **zone** onto one of mod-playerbots' own teleport
  locations for that zone — and relevels them into the band on the way — so about
  `TargetPercent`% of the bots end up **both in that zone and inside the band**.
  Skips instances and maps not listed in `Playerbots.RandomBotMaps`. While area sync is
  on the module also forces playerbots' own `Playerbots.RandomBotConcentrateInPlayerZone`
  to 1, so playerbots' periodic random teleports land in a zone a real player is standing
  in instead of scattering the gathered population back across the world; turning area
  sync off restores the value from `playerbots.conf`.
- **Zone level spread** — `ZoneSpreadPercent` of the bots in that zone are levelled to
  match the **zone** rather than the player, so walking your level 80 into a level-10
  zone finds level-10 bots there instead of an empty field. Their level comes from the
  zone's own creature spawns; the share is re-rolled each pass so the same bots are not
  always the low ones. `0` (the default) keeps the old player-level-only behaviour.
- **Faction mirroring** — when the online real players are all on one faction, every
  random bot is put on that faction (playerbots' alliance/horde ratios are driven to
  100/0), so a Horde-only realm stops filling half its world with Alliance bots. Both
  factions online, or nobody online, restores the configured ratios.
- **Death Knight progression gating** — optionally keeps Death Knight bots out of
  the world until a real-player account has progressed far enough
  (requires [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)).
- **Smart exclusions** — never touches bots that are *yours*: in a guild with a
  real player (online or offline), on a friends list, in an arena team, in any
  party/raid group, or name-listed.
- **Safety-aware** — never relevels a bot that is busy: dead, in combat, in a
  battleground/arena/random dungeon (or queue), or in flight.
- **Event-driven** — reacts to real-player login and level-up instantly, with a
  configurable safety timer as a fallback.
- **Ships no database schema** — reads existing tables only.

## Requirements

- AzerothCore (latest master recommended)
- [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) (required)
- [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
  (optional — only for Death Knight progression gating)

## Installation

### 1. Clone the module

```bash
cd <ACoreDir>/modules
git clone https://github.com/Yuof/mod-rndbot-sync.git
```

### 2. Re-compile AzerothCore

```bash
cd <ACoreDir>/build
cmake ../ -DCMAKE_INSTALL_PREFIX=/path/to/server
make -j $(nproc)
make install
```

### 3. Configure

Copy `mod_rndbot_sync.conf.dist` to `mod_rndbot_sync.conf` in your server's
`configs/modules` directory and adjust the options (see below). The defaults are
sensible for a small or solo server.

### 4. Restart the server

```bash
./worldserver
```

## Commands

All commands are administrator-only and work from the console and in-game.

| Command | Description |
|---------|-------------|
| `.rndbotsync reload` | Reload the configuration at runtime. |
| `.rndbotsync status` | Print a read-only snapshot: target player, band/ceiling, eligible-bot counts, gear cap, and Death Knight gate state. |
| `.rndbotsync resync` | Force an immediate full pass, ignoring the `ProcessLimit` throttle (still respects the ceiling, gear cap, exclusions, and safety checks). |

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `RndBotSync.Enabled` | Boolean | 1 | Enable/disable the module. |
| `RndBotSync.CheckFrequency` | Integer | 300 | Seconds between safety-timer passes (`0` = react to events only). |
| `RndBotSync.TargetPercent` | Integer | 30 | Percent of eligible bots to keep within the band; with area sync on, the same number is the in-zone-in-band quota. |
| `RndBotSync.TargetBand` | Integer | 3 | Band half-width (± levels) and ceiling offset. |
| `RndBotSync.ZoneSpreadPercent` | Integer | 0 | Percent of the zone's bots levelled to the zone's own creature level instead of the player's. |
| `RndBotSync.ProcessLimit` | Integer | 5 | Max bots adjusted per pass (`0` = unlimited). |
| `RndBotSync.AllowDownleveling` | Boolean | 1 | Pull bots above the ceiling down. `0` = upward-only. |
| `RndBotSync.IgnoreGuildBotsWithRealPlayers` | Boolean | 1 | Skip bots in a guild with any real-player member. |
| `RndBotSync.IgnoreFriendListed` | Boolean | 1 | Skip bots on a real player's friends list. |
| `RndBotSync.IgnoreArenaTeamBots` | Boolean | 1 | Skip bots in an arena team. |
| `RndBotSync.IgnoreGroupedBots` | Boolean | 1 | Skip bots in any party/raid group. |
| `RndBotSync.ExcludeNames` | String | "" | Comma-separated, case-insensitive bot names to skip. |
| `RndBotSync.GuildCacheRefreshFrequency` | Integer | 600 | Seconds between real-player-guild / friend cache refreshes. |
| `RndBotSync.IlvlSync.Enabled` | Boolean | 1 | Cap bot gear at the player's item level. |
| `RndBotSync.AreaSync.Enabled` | Boolean | 1 | Progressively gather bots into the player's zone. |
| `RndBotSync.AreaSync.ZoneId` | Integer | 0 | Pin the target zone id (e.g. 1519 = Stormwind City); 0 = the player's own zone. |
| `RndBotSync.FactionSync.Enabled` | Boolean | 1 | Force every bot onto the faction the online real players are on. |
| `Playerbots.RandomBotConcentrateInPlayerZone` | Boolean | 0 | Read from `playerbots.conf`, not from this file (newer playerbots builds spell it `Playerbots.randomBotConcentrateInPlayerZone`). Forced to 1 while area sync is enabled; restored when it is off. |
| `RndBotSync.DeathKnightProgression.Enabled` | Boolean | 0 | Gate Death Knight bots on mod-individual-progression. |
| `RndBotSync.DeathKnightProgression.RequiredState` | Integer | 13 | Required `ProgressionState` (1–18) to unlock DK bots (13 = WotLK content). |
| `RndBotSync.FullDebugMode` | Boolean | 0 | Verbose logging. |
| `RndBotSync.LiteDebugMode` | Boolean | 0 | Summary logging. |

## How it works

There is a single **target**: the highest-level real (non-bot) player online.
Around it sits a **band** of `±TargetBand` levels; the top of that band is the
**ceiling** on the entire bot population. Each pass:

1. **Pulls stragglers up** — raises the lowest-level bots into the band so that
   about `TargetPercent`% of bots sit near the player.
2. **Pulls the over-leveled down** — reshuffles any bot above the ceiling back
   down (when `AllowDownleveling` is on).
3. **Caps gear** — drives mod-playerbots' own gear-score limit so all bot gear,
   including freshly initialized bots, stays at or below the player's item level.
4. **Gathers the stragglers** — moves bots standing in a different zone than the
   player onto one of mod-playerbots' own teleport locations for the player's zone,
   nearest first, releveling each one into the band as it goes, until about
   `TargetPercent`% of the bots are both in that zone and in the band. A bot already
   in the zone but out of the band is only releveled. In-band bots in the zone are
   never touched, so the world converges over successive passes.
5. **Mirrors the faction** — snaps playerbots' alliance/horde ratios to the teams the
   online real players are actually on, so a one-faction realm does not spawn bots on
   the other one.

Passes run on real-player login and level-up, plus the safety timer. The
`safety timer` alternates between the level steps and the zone step, so a pass never
re-does the work the previous pass just did. The `ProcessLimit` spreads large corrections
over several passes so nothing happens in a single laggy burst; when area sync is on, half
of that budget belongs to the zone step so level promotion cannot starve it.

## Credits

- **Yuof** — author.
- Derived from [mod-player-bot-level-brackets](https://github.com/DustinHendrickson/mod-player-bot-level-brackets)
  (AGPL-3.0) — the bot-exclusion checks and level-set path originate there.
- Built on [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) and
  [AzerothCore](https://www.azerothcore.org/).

## License

Released under the [GNU AGPL v3](LICENSE) license.
