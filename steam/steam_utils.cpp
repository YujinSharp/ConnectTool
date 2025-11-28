#include "steam_utils.h"
#include <iostream>

std::vector<std::pair<CSteamID, std::string>> SteamUtilsHelper::getFriendsList() {
    std::vector<std::pair<CSteamID, std::string>> friendsList;
    int friendCount = SteamFriends()->GetFriendCount(k_EFriendFlagAll);
    for (int i = 0; i < friendCount; ++i) {
        CSteamID friendID = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagAll);
        const char* name = SteamFriends()->GetFriendPersonaName(friendID);
        friendsList.push_back({friendID, name});
    }
    return friendsList;
}

std::vector<FriendLobbyInfo> SteamUtilsHelper::getFriendLobbies() {
    std::vector<FriendLobbyInfo> lobbyList;
    int friendCount = SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
    AppId_t currentAppID = SteamUtils()->GetAppID();

    for (int i = 0; i < friendCount; ++i) {
        CSteamID friendID = SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
        FriendGameInfo_t friendGameInfo;
        if (SteamFriends()->GetFriendGamePlayed(friendID, &friendGameInfo)) {
            if (friendGameInfo.m_gameID.AppID() == currentAppID) {
                if (friendGameInfo.m_steamIDLobby.IsValid()) {
                    const char* name = SteamFriends()->GetFriendPersonaName(friendID);
                    lobbyList.push_back({friendGameInfo.m_steamIDLobby, name});
                }
            }
        }
    }
    return lobbyList;
}
