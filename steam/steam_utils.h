#pragma once
#include <vector>
#include <string>
#include <steam_api.h>

struct FriendLobbyInfo {
    CSteamID lobbyID;
    CSteamID friendID;
    std::string friendName;
};

class SteamUtilsHelper {
public:
    // 获取好友列表
    static std::vector<std::pair<CSteamID, std::string>> getFriendsList();
    // 获取好友所在的Lobby信息
    static std::vector<FriendLobbyInfo> getFriendLobbies();
};