#ifndef STEAM_NETWORKING_MANAGER_H
#define STEAM_NETWORKING_MANAGER_H

#include <vector>
#include <set>
#include <mutex>
#include <memory>
#include <steam_api.h>
#include <isteamnetworkingmessages.h>
#include <isteamnetworkingutils.h>
#include <steamnetworkingtypes.h>
#include "steam_message_handler.h"

// Forward declarations
class SteamNetworkingManager;
class SteamVpnBridge;
class SteamRoomManager;

// User info structure
struct UserInfo {
    CSteamID steamID;
    std::string name;
    int ping;
    bool isRelay;
};

/**
 * @brief Steam 网络管理器（ISteamNetworkingMessages 版本）
 * 
 * 使用 ISteamNetworkingMessages 接口实现无连接的消息传递，
 * 无需手动维护连接状态，连接会自动建立和管理。
 */
class SteamNetworkingManager {
public:
    static SteamNetworkingManager* instance;
    SteamNetworkingManager();
    ~SteamNetworkingManager();

    bool initialize();
    void shutdown();

    // 发送消息给指定用户（使用 ISteamNetworkingMessages）
    bool sendMessageToUser(CSteamID peerID, const void* data, uint32_t size, int flags);
    
    // 广播消息给房间内所有成员（实时从房间获取）
    void broadcastMessage(const void* data, uint32_t size, int flags);

    // 获取房间内所有成员（实时从房间获取）
    std::set<CSteamID> getRoomMembers() const;
    
    // 设置房间管理器
    void setRoomManager(SteamRoomManager* roomManager) { roomManager_ = roomManager; }

    // 获取与指定用户的会话信息
    int getPeerPing(CSteamID peerID) const;
    bool isPeerConnected(CSteamID peerID) const;
    std::string getPeerConnectionType(CSteamID peerID) const;

    // Getters
    bool isInRoom() const;
    ISteamNetworkingMessages* getMessagesInterface() const { return m_pMessagesInterface; }

    // Message handler
    void startMessageHandler();
    void stopMessageHandler();
    SteamMessageHandler* getMessageHandler() { return messageHandler_; }

    // VPN Bridge
    void setVpnBridge(SteamVpnBridge* vpnBridge) { vpnBridge_ = vpnBridge; }
    SteamVpnBridge* getVpnBridge() { return vpnBridge_; }

    // VPN 消息通道
    static constexpr int VPN_CHANNEL = 0;

private:
    // Steam Networking Messages API
    ISteamNetworkingMessages* m_pMessagesInterface;

    // 房间管理器（用于实时获取房间成员）
    SteamRoomManager* roomManager_;

    // Message handler
    SteamMessageHandler* messageHandler_;

    // VPN Bridge
    SteamVpnBridge* vpnBridge_;

    // 回调处理
    STEAM_CALLBACK(SteamNetworkingManager, OnSessionRequest, SteamNetworkingMessagesSessionRequest_t);
    STEAM_CALLBACK(SteamNetworkingManager, OnSessionFailed, SteamNetworkingMessagesSessionFailed_t);

    friend class SteamRoomManager;
};

#endif // STEAM_NETWORKING_MANAGER_H