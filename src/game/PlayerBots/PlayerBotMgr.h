#ifndef _PLAYERBOTMGR_H
#define _PLAYERBOTMGR_H

#include "Common.h"
#include "Policies/Singleton.h"
#include "Database/DatabaseEnv.h"
#include "PlayerBotAI.h"
#include "BattleGroundDefines.h"

#include <vector>
#include <memory>

class PlayerBotAI;
class WorldSession;
class Player;

enum PlayerBotAutoEquip
{
    PLAYER_BOT_AUTO_EQUIP_STARTING_GEAR = 0,
    PLAYER_BOT_AUTO_EQUIP_RANDOM_GEAR = 1,
    PLAYER_BOT_AUTO_EQUIP_PREMADE_GEAR = 2,
};

enum PlayerBotState
{
    PB_STATE_OFFLINE,
    PB_STATE_LOADING,
    PB_STATE_ONLINE
};

enum PlayerBotTalentProfileState
{
    PB_TALENT_PROFILE_UNCHECKED,
    PB_TALENT_PROFILE_USABLE,
    PB_TALENT_PROFILE_CONFLICT,
    PB_TALENT_PROFILE_INVALID,
    PB_TALENT_PROFILE_DISABLED,
    PB_TALENT_PROFILE_ERROR,
};

struct PlayerBotEntry
{
    uint64 playerGUID;
    std::string name;
    uint32 accountId;

    uint8 specTab; // Class-local profile: 0..2; 255 means unassigned.
    CombatBotRoles activeRole; // Persisted independently from the talent profile.
    PlayerBotTalentProfileState talentProfileState; // Runtime compatibility result; UI derives its own DB view.
    uint32 chance;
    uint8 state; //Online, in queue or offline
    bool isChatBot; // bot des joueurs en discussion via le site.
    bool customBot; // Enabled even if PlayerBot system disabled (AutoTesting system for example)
    bool requestRemoval;
    std::unique_ptr<PlayerBotAI> ai;
    // [COMPANION] Non-zero = this entry is a real account's own character
    // summoned for the owner's session (SuiCompanion.h). Never persisted,
    // never random-cycled, never bridge-connected.
    uint32 ownerAccountId;
    ObjectGuid ownerGuid;
    // World session-map key of this bot's session (WorldSession::GetSessionKey).
    // == accountId for every fleet bot; a companion lives under a synthetic key
    // while keeping the owner's real accountId. PlayerBotMgr must find bot
    // sessions by THIS, never by accountId, or a companion's login lands on its
    // owner's live session.
    uint32 sessionKey;

    PlayerBotEntry(uint64 guid, uint32 account, uint32 chance_)
        : playerGUID(guid), accountId(account), specTab(255), activeRole(ROLE_INVALID), talentProfileState(PB_TALENT_PROFILE_UNCHECKED),
          chance(chance_), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), requestRemoval(false), ai(nullptr), ownerAccountId(0), sessionKey(account)
    {}
    PlayerBotEntry()
        : playerGUID(0), accountId(0), specTab(255), activeRole(ROLE_INVALID), talentProfileState(PB_TALENT_PROFILE_UNCHECKED),
          chance(100.0f), state(PB_STATE_OFFLINE), isChatBot(false), customBot(false), requestRemoval(false), ai(nullptr), ownerAccountId(0), sessionKey(0)
    {}
};

struct PlayerBotStats
{
    /* Stats */
    uint32 onlineCount;
    uint32 loadingCount;
    uint32 totalBots;
    uint32 onlineChat;

    /* Config */
    uint32 confMaxOnline;
    uint32 confMinOnline;
    uint32 confRandomBotsRefresh;
    uint32 confUpdateDiff;

    PlayerBotStats() 
    : onlineCount(0), loadingCount(0), totalBots(0), onlineChat(0),
    confMaxOnline(0), confMinOnline(0), confRandomBotsRefresh(0), confUpdateDiff(0) {}
};


class PlayerBotMgr
{
    public:
        PlayerBotMgr();
        ~PlayerBotMgr();

        void LoadConfig();
        void Load();

        void Update(uint32 diff);
        bool AddOrRemoveBot();

        bool AddBot(PlayerBotAI* ai);
        bool AddBot(uint32 playerGuid, bool chatBot = false, PlayerBotAI* pAI = nullptr);
        // [COMPANION] Log one of `ownerAccount`'s own characters in on a bot
        // session that keeps the REAL account id (synthetic session key only).
        // Takes ownership of `ai` whether or not it succeeds.
        bool AddCompanion(uint32 playerGuid, uint32 ownerAccount, ObjectGuid ownerGuid, PlayerBotAI* ai);
        // Drop a registry entry without touching its session (companion logout).
        void ForgetBot(uint32 playerGuid);
        bool DeleteBot(std::map<uint64, std::shared_ptr<PlayerBotEntry>>::iterator iter);
        bool DeleteBot(uint32 playerGuid);

        bool AddRandomBot();
        bool DeleteRandomBot();

        void AddBattleBot(BattleGroundQueueTypeId queueType, Team botTeam, uint32 botLevel, bool temporary);
        void DeleteBattleBots();

        void DeleteAll();
        void AddAllBots();

        void OnBotLogout(PlayerBotEntry *e);
        void OnBotLogin(PlayerBotEntry *e);
        void OnPlayerInWorld(Player* pPlayer);
        void AddTempBot(uint32 account, uint32 time);
        void RefreshTempBot(uint32 account);

        bool ForceAccountConnection(WorldSession* sess);
        bool IsPermanentBot(uint32 playerGuid);
        bool IsChatBot(uint32 playerGuid);
        bool IsSavingAllowed() { return m_confAllowSaving; }

        uint32 GenBotAccountId() { return ++m_maxAccountId; }
        PlayerBotStats& GetStats(){ return m_stats; }
        void Start() { m_confEnableRandomBots = true; }
    protected:
        // How long since last update?
        uint32 m_elapsedTime;
        uint32 m_lastBotsRefresh;
        uint32 m_lastUpdate;
        uint32 m_totalChance;
        uint32 m_maxAccountId;
        time_t m_lastBattleBotQueueUpdate;

        std::map<uint64 /*pl guid*/, std::shared_ptr<PlayerBotEntry>> m_bots;
        std::map<uint32 /*account*/, uint32> m_tempBots;
        PlayerBotStats m_stats;

        uint32 m_confMinRandomBots;
        uint32 m_confMaxRandomBots;
        uint32 m_confRandomBotsRefresh;
        uint32 m_confUpdateDiff;
        bool m_confAllowSaving;
        bool m_confDebug;
        bool m_confEnableRandomBots;
        bool m_confBattleBotAutoJoin;
};

#define sPlayerBotMgr MaNGOS::Singleton<PlayerBotMgr>::Instance()
#endif
