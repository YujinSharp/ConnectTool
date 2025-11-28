#pragma once
#include <vector>
#include <string>
#include <steam_api.h>

struct FriendLobbyInfo {
    CSteamID lobbyID;
    std::string friendName;
};

class SteamUtilsHelper {
public:
    static std::vector<std::pair<CSteamID, std::string>> getFriendsList();
    static std::vector<FriendLobbyInfo> getFriendLobbies();
};