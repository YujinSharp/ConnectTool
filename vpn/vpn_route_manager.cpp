#include "../vpn/vpn_route_manager.h"
#include "node_identity.h"
#include "vpn_utils.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

using VpnUtils::ipToString;

VpnRouteManager::VpnRouteManager() {}

void VpnRouteManager::setCallbacks(VpnRouteManager::VpnSendCallback sendCb, VpnRouteManager::VpnBroadcastCallback broadcastCb, VpnRouteManager::OnRouteAddedCallback onRouteAddedCb) {
    sendCallback_ = sendCb;
    broadcastCallback_ = broadcastCb;
    onRouteAddedCallback_ = onRouteAddedCb;
}

void VpnRouteManager::updateRoute(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress, const std::string& name) {
    RouteEntry entry;
    entry.steamID = steamId;
    entry.ipAddress = ipAddress;
    entry.name = name;
    entry.isLocal = (steamId == SteamUser()->GetSteamID());
    entry.nodeId = nodeId;
    
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        // Remove old entries for this SteamID
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
            if (it->second.steamID == steamId && it->first != ipAddress) {
                it = routingTable_.erase(it);
            } else {
                ++it;
            }
        }
        routingTable_[ipAddress] = entry;
    }
    
    if (onRouteAddedCallback_) {
        onRouteAddedCallback_(ipAddress);
    }
    
    std::cout << "Route updated: " << ipToString(ipAddress) << " -> " << name << std::endl;
}

void VpnRouteManager::removeRoute(uint32_t ipAddress) {
    std::lock_guard<std::mutex> lock(routingMutex_);
    routingTable_.erase(ipAddress);
}

void VpnRouteManager::removeRoutesForUser(CSteamID steamID, std::function<void(uint32_t, const NodeID&)> onRemoved) {
    std::lock_guard<std::mutex> lock(routingMutex_);
    for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
        if (it->second.steamID == steamID) {
            if (onRemoved) {
                onRemoved(it->first, it->second.nodeId);
            }
            it = routingTable_.erase(it);
        } else {
            ++it;
        }
    }
}

std::map<uint32_t, RouteEntry> VpnRouteManager::getRoutingTable() const {
    std::lock_guard<std::mutex> lock(routingMutex_);
    return routingTable_;
}

bool VpnRouteManager::getRoute(uint32_t ipAddress, RouteEntry& outEntry) const {
    std::lock_guard<std::mutex> lock(routingMutex_);
    auto it = routingTable_.find(ipAddress);
    if (it != routingTable_.end()) {
        outEntry = it->second;
        return true;
    }
    return false;
}

void VpnRouteManager::handleRouteUpdate(const uint8_t* payload, size_t length, uint32_t myBaseIP, uint32_t mySubnetMask, CSteamID mySteamID) {
    size_t offset = 0;
    while (offset + 12 <= length) {
        uint64_t steamID64;
        uint32_t ipAddress;
        memcpy(&steamID64, payload + offset, 8);
        memcpy(&ipAddress, payload + offset + 8, 4);
        ipAddress = ntohl(ipAddress);
        offset += 12;

        CSteamID csteamID(steamID64);
        
        // Skip own routes
        if (csteamID == mySteamID) {
            continue;
        }
        
        // Check if route already exists
        {
            std::lock_guard<std::mutex> lock(routingMutex_);
            auto it = routingTable_.find(ipAddress);
            if (it != routingTable_.end()) {
                continue; // Already have this route
            }
        }

        if ((ipAddress & mySubnetMask) == (myBaseIP & mySubnetMask)) {
            NodeID nodeId = NodeIdentity::generate(csteamID);
            std::string name = SteamFriends()->GetFriendPersonaName(csteamID);
            
            updateRoute(nodeId, csteamID, ipAddress, name);
        }
    }
}

void VpnRouteManager::broadcastRouteUpdate() {
    std::vector<uint8_t> routeData;

    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (const auto& entry : routingTable_) {
            uint64_t steamID = entry.second.steamID.ConvertToUint64();
            uint32_t ipAddress = htonl(entry.second.ipAddress);
            
            size_t offset = routeData.size();
            routeData.resize(offset + 12);
            memcpy(routeData.data() + offset, &steamID, 8);
            memcpy(routeData.data() + offset + 8, &ipAddress, 4);
        }
    }

    if (broadcastCallback_) {
        broadcastCallback_(VpnMessageType::ROUTE_UPDATE, routeData.data(), routeData.size(), true);
    }
}

void VpnRouteManager::sendRouteUpdateTo(CSteamID targetSteamID) {
    std::vector<uint8_t> routeData;

    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (const auto& entry : routingTable_) {
            uint64_t steamID = entry.second.steamID.ConvertToUint64();
            uint32_t ipAddress = htonl(entry.second.ipAddress);
            
            size_t offset = routeData.size();
            routeData.resize(offset + 12);
            memcpy(routeData.data() + offset, &steamID, 8);
            memcpy(routeData.data() + offset + 8, &ipAddress, 4);
        }
    }

    if (sendCallback_) {
        sendCallback_(VpnMessageType::ROUTE_UPDATE, routeData.data(), routeData.size(), targetSteamID, true);
    }
}

void VpnRouteManager::clear() {
    std::lock_guard<std::mutex> lock(routingMutex_);
    routingTable_.clear();
}
