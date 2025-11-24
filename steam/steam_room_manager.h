#pragma once
#include <steam_api.h>
#include <vector>
#include <iostream>
#include <mutex>

class SteamNetworkingManager; // Forward declaration
class SteamRoomManager; // Forward declaration for callbacks

class SteamFriendsCallbacks
{
public:
    SteamFriendsCallbacks(SteamNetworkingManager *manager, SteamRoomManager *roomManager);

private:
    SteamNetworkingManager *manager_;
    SteamRoomManager *roomManager_;
    
    STEAM_CALLBACK(SteamFriendsCallbacks, OnGameLobbyJoinRequested, GameLobbyJoinRequested_t);
};

class SteamMatchmakingCallbacks
{
public:
    SteamMatchmakingCallbacks(SteamNetworkingManager *manager, SteamRoomManager *roomManager);
    
    CCallResult<SteamMatchmakingCallbacks, LobbyCreated_t> m_CallResultLobbyCreated;
    CCallResult<SteamMatchmakingCallbacks, LobbyMatchList_t> m_CallResultLobbyMatchList;
    
    void OnLobbyCreated(LobbyCreated_t *pCallback, bool bIOFailure);
    void OnLobbyListReceived(LobbyMatchList_t *pCallback, bool bIOFailure);

private:
    SteamNetworkingManager *manager_;
    SteamRoomManager *roomManager_;
    
    STEAM_CALLBACK(SteamMatchmakingCallbacks, OnLobbyEntered, LobbyEnter_t);
    STEAM_CALLBACK(SteamMatchmakingCallbacks, OnLobbyChatUpdate, LobbyChatUpdate_t);
};

class SteamRoomManager
{
public:
    SteamRoomManager(SteamNetworkingManager *networkingManager);
    ~SteamRoomManager();

    bool createLobby();
    void leaveLobby();
    bool searchLobbies();
    bool joinLobby(CSteamID lobbyID);

    CSteamID getCurrentLobby() const { return currentLobby; }
    const std::vector<CSteamID>& getLobbies() const { return lobbies; }
    std::vector<CSteamID> getLobbyMembers() const;

    void setCurrentLobby(CSteamID lobby) { currentLobby = lobby; }
    void addLobby(CSteamID lobby) { lobbies.push_back(lobby); }
    void clearLobbies() { lobbies.clear(); }

private:
    SteamNetworkingManager *networkingManager_;
    CSteamID currentLobby;
    std::vector<CSteamID> lobbies;
    SteamFriendsCallbacks *steamFriendsCallbacks;
    SteamMatchmakingCallbacks *steamMatchmakingCallbacks;
};