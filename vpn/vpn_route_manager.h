#pragma once

#include <map>
#include <mutex>
#include <vector>
#include <string>
#include <functional>
#include "steam_api.h"
#include "vpn_protocol.h"

class VpnRouteManager {
public:
    using VpnSendCallback = std::function<void(VpnMessageType type, const uint8_t* payload, size_t len, CSteamID target, bool reliable)>;
    using VpnBroadcastCallback = std::function<void(VpnMessageType type, const uint8_t* payload, size_t len, bool reliable)>;
    using OnRouteAddedCallback = std::function<void(uint32_t ipAddress)>;

    VpnRouteManager();
    
    void setCallbacks(VpnSendCallback sendCb, VpnBroadcastCallback broadcastCb, OnRouteAddedCallback onRouteAddedCb);

    void updateRoute(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress, const std::string& name);
    void removeRoute(uint32_t ipAddress);
    
    // Remove all routes associated with a SteamID. 
    // onRemoved callback is called for each removed route with (ip, nodeId)
    void removeRoutesForUser(CSteamID steamID, std::function<void(uint32_t, const NodeID&)> onRemoved);
    
    std::map<uint32_t, RouteEntry> getRoutingTable() const;
    bool getRoute(uint32_t ipAddress, RouteEntry& outEntry) const;
    
    // Handle incoming ROUTE_UPDATE message
    void handleRouteUpdate(const uint8_t* payload, size_t length, uint32_t myBaseIP, uint32_t mySubnetMask, CSteamID mySteamID);

    void broadcastRouteUpdate();
    void sendRouteUpdateTo(CSteamID targetSteamID);
    
    void clear();

private:
    std::map<uint32_t, RouteEntry> routingTable_;
    mutable std::mutex routingMutex_;
    
    VpnSendCallback sendCallback_;
    VpnBroadcastCallback broadcastCallback_;
    OnRouteAddedCallback onRouteAddedCallback_;
};
