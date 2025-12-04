#include "steam_vpn_bridge.h"
#include "steam_networking_manager.h"
#include "config/config_manager.h"
#include "steam_vpn_utils.h"
#include "../vpn/vpn_utils.h"
#include <iostream>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

using namespace SteamVpnUtils;
using namespace VpnUtils;

SteamVpnBridge::SteamVpnBridge(SteamNetworkingManager* steamManager)
    : steamManager_(steamManager)
    , running_(false)
    , baseIP_(0)
    , subnetMask_(0)
    , localIP_(0)
{
    memset(&stats_, 0, sizeof(stats_));
}

SteamVpnBridge::~SteamVpnBridge() {
    stop();
}

bool SteamVpnBridge::start(const std::string& tunDeviceName,
                            const std::string& virtualSubnet,
                            const std::string& subnetMask) {
    if (running_) {
        std::cerr << "VPN bridge is already running" << std::endl;
        return false;
    }

    const auto& config = ConfigManager::instance().getConfig();
    int steamMtuDataSize = querySteamMtuDataSize();
    int mtu = calculateTunMtu(steamMtuDataSize);
    
    if (config.vpn.default_mtu > 0 && config.vpn.default_mtu < mtu) {
        std::cout << "[MTU] Using config MTU (" << config.vpn.default_mtu 
                  << ") instead of calculated (" << mtu << ")" << std::endl;
        mtu = config.vpn.default_mtu;
    }

    tunDevice_ = tun::create_tun();
    if (!tunDevice_) {
        std::cerr << "Failed to create TUN device" << std::endl;
        return false;
    }

    if (!tunDevice_->open(tunDeviceName, mtu)) { 
        std::cerr << "Failed to open TUN device: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device created: " << tunDevice_->get_device_name() << std::endl;

    if (!tunDevice_->set_mtu(mtu)) {
        std::cerr << "Failed to set TUN device MTU: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device MTU set to: " << mtu << std::endl;
    baseIP_ = stringToIp(virtualSubnet);
    if (baseIP_ == 0) {
        std::cerr << "Invalid virtual subnet: " << virtualSubnet << std::endl;
        return false;
    }
    subnetMask_ = stringToIp(subnetMask);

    CSteamID mySteamID = SteamUser()->GetSteamID();
    ipNegotiator_.initialize(mySteamID, baseIP_, subnetMask_);
    
    // Initialize Route Manager Callbacks
    routeManager_.setCallbacks(
        [this](VpnMessageType type, const uint8_t* payload, size_t len, CSteamID target, bool reliable) {
            sendVpnMessage(type, payload, len, target, reliable);
        },
        [this](VpnMessageType type, const uint8_t* payload, size_t len, bool reliable) {
            broadcastVpnMessage(type, payload, len, reliable);
        },
        [this](uint32_t ip) {
            ipNegotiator_.markIPUsed(ip);
        }
    );

    ipNegotiator_.setSendCallback(
        [this](VpnMessageType type, const uint8_t* payload, size_t len, CSteamID targetSteamID, bool reliable) {
            sendVpnMessage(type, payload, len, targetSteamID, reliable);
        },
        [this](VpnMessageType type, const uint8_t* payload, size_t len, bool reliable) {
            broadcastVpnMessage(type, payload, len, reliable);
        }
    );
    
    ipNegotiator_.setSuccessCallback(
        [this](uint32_t ip, const NodeID& nodeId) {
            onNegotiationSuccess(ip, nodeId);
        }
    );
    
    heartbeatManager_.setSendCallback(
        [this](VpnMessageType type, const uint8_t* payload, size_t len, bool reliable) {
            broadcastVpnMessage(type, payload, len, reliable);
        }
    );
    
    heartbeatManager_.setNodeExpiredCallback(
        [this](const NodeID& nodeId, uint32_t ip) {
            onNodeExpired(nodeId, ip);
        }
    );

    ipNegotiator_.startNegotiation();
    tunDevice_->set_non_blocking(false);

    running_ = true;
    tunReadThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunReadThread, this);
    
    std::cout << "Steam VPN bridge started successfully" << std::endl;
    return true;
}

void SteamVpnBridge::stop() {
    if (!running_) return;

    running_ = false;
    heartbeatManager_.stop();

    if (tunReadThread_ && tunReadThread_->joinable()) {
        tunReadThread_->join();
    }

    if (tunDevice_) {
        tunDevice_->close();
    }

    routeManager_.clear();
    localIP_ = 0;

    std::cout << "Steam VPN bridge stopped" << std::endl;
}

std::string SteamVpnBridge::getLocalIP() const {
    if (localIP_ == 0) return "Not assigned";
    return ipToString(localIP_);
}

std::string SteamVpnBridge::getTunDeviceName() const {
    if (tunDevice_ && tunDevice_->is_open()) {
        return tunDevice_->get_device_name();
    }
    return "N/A";
}

std::map<uint32_t, RouteEntry> SteamVpnBridge::getRoutingTable() const {
    return routeManager_.getRoutingTable();
}

void SteamVpnBridge::tunReadThread() {
    std::cout << "TUN read thread started" << std::endl;
    
    constexpr size_t BUFFER_SIZE = 16384;
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    std::vector<uint8_t> vpnPacketBuffer(BUFFER_SIZE + sizeof(VpnMessageHeader) + sizeof(VpnPacketWrapper));
    
    auto lastTimeoutCheck = std::chrono::steady_clock::now();
    
    while (running_) {
        int bytesRead = tunDevice_->read(buffer.data(), buffer.size());
        
        if (bytesRead > 0) {
            uint32_t destIP = extractDestIP(buffer.data(), bytesRead);
            
            VpnMessageHeader* header = reinterpret_cast<VpnMessageHeader*>(vpnPacketBuffer.data());
            header->type = VpnMessageType::IP_PACKET;
            
            VpnPacketWrapper* wrapper = reinterpret_cast<VpnPacketWrapper*>(vpnPacketBuffer.data() + sizeof(VpnMessageHeader));
            wrapper->senderNodeId = ipNegotiator_.getLocalNodeID();
            
            size_t totalPayloadSize = sizeof(VpnPacketWrapper) + bytesRead;
            header->length = htons(static_cast<uint16_t>(totalPayloadSize));
            
            memcpy(vpnPacketBuffer.data() + sizeof(VpnMessageHeader) + sizeof(VpnPacketWrapper), buffer.data(), bytesRead);
            uint32_t vpnPacketSize = static_cast<uint32_t>(sizeof(VpnMessageHeader) + totalPayloadSize);

            if (isBroadcastAddress(destIP, baseIP_, subnetMask_)) {
                steamManager_->broadcastMessage(vpnPacketBuffer.data(), vpnPacketSize, 
                    k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay);
                
                auto members = steamManager_->getRoomMembers();
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsSent += members.size();
                stats_.bytesSent += bytesRead * members.size();
            } else {
                CSteamID targetSteamID;
                bool found = false;
                
                RouteEntry entry;
                if (routeManager_.getRoute(destIP, entry) && !entry.isLocal) {
                    targetSteamID = entry.steamID;
                    found = true;
                }

                if (found) {
                    steamManager_->sendMessageToUser(targetSteamID, vpnPacketBuffer.data(), vpnPacketSize,
                        k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay);

                    std::lock_guard<std::mutex> lock(statsMutex_);
                    stats_.packetsSent++;
                    stats_.bytesSent += bytesRead;
                }
            }
        }
        
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTimeoutCheck).count() >= 50) {
            lastTimeoutCheck = now;
            ipNegotiator_.checkTimeout();
        }
    }
    
    std::cout << "TUN read thread stopped" << std::endl;
}

void SteamVpnBridge::handleVpnMessage(const uint8_t* data, size_t length, CSteamID senderSteamID) {
    if (length < sizeof(VpnMessageHeader)) return;

    VpnMessageHeader header;
    memcpy(&header, data, sizeof(VpnMessageHeader));
    uint16_t payloadLength = ntohs(header.length);

    if (length < sizeof(VpnMessageHeader) + payloadLength) return;

    const uint8_t* payload = data + sizeof(VpnMessageHeader);
    std::string peerName = SteamFriends()->GetFriendPersonaName(senderSteamID);
    
    if (header.type == VpnMessageType::IP_PACKET) {
        if (tunDevice_ && payloadLength > sizeof(VpnPacketWrapper)) {
            const uint8_t* ipPacket = payload + sizeof(VpnPacketWrapper);
            size_t ipPacketLen = payloadLength - sizeof(VpnPacketWrapper);
            
            uint32_t destIP = extractDestIP(ipPacket, ipPacketLen);
            
            if (destIP == localIP_ || isBroadcastAddress(destIP, baseIP_, subnetMask_)) {
                tunDevice_->write(ipPacket, ipPacketLen);
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsReceived++;
                stats_.bytesReceived += ipPacketLen;
            } else {
                CSteamID targetSteamID;
                bool found = false;
                
                RouteEntry entry;
                if (routeManager_.getRoute(destIP, entry) && !entry.isLocal) {
                    targetSteamID = entry.steamID;
                    found = true;
                }
                
                if (found && targetSteamID != senderSteamID) {
                    sendVpnMessage(VpnMessageType::IP_PACKET, payload, payloadLength, targetSteamID, false);
                }
            }
        }
        return;
    }

    switch (header.type) {
        case VpnMessageType::ROUTE_UPDATE: {
            routeManager_.handleRouteUpdate(payload, payloadLength, baseIP_, subnetMask_, SteamUser()->GetSteamID());
            break;
        }

        case VpnMessageType::PROBE_REQUEST: {
            if (payloadLength >= sizeof(ProbeRequestPayload)) {
                ProbeRequestPayload request;
                memcpy(&request, payload, sizeof(ProbeRequestPayload));
                ipNegotiator_.handleProbeRequest(request, senderSteamID);
            }
            break;
        }
            
        case VpnMessageType::PROBE_RESPONSE: {
            if (payloadLength >= sizeof(ProbeResponsePayload)) {
                ProbeResponsePayload response;
                memcpy(&response, payload, sizeof(ProbeResponsePayload));
                ipNegotiator_.handleProbeResponse(response, senderSteamID);
            }
            break;
        }
            
        case VpnMessageType::ADDRESS_ANNOUNCE: {
            if (payloadLength >= sizeof(AddressAnnouncePayload)) {
                AddressAnnouncePayload announce;
                memcpy(&announce, payload, sizeof(AddressAnnouncePayload));
                
                uint32_t announcedIP = ntohl(announce.ipAddress);
                bool isNewRoute = false;
                
                RouteEntry entry;
                isNewRoute = !routeManager_.getRoute(announcedIP, entry);
                
                ipNegotiator_.handleAddressAnnounce(announce, senderSteamID, peerName);
                
                routeManager_.updateRoute(announce.nodeId, senderSteamID, announcedIP, peerName);
                
                if (isNewRoute) {
                    routeManager_.broadcastRouteUpdate();
                }
            }
            break;
        }

        case VpnMessageType::FORCED_RELEASE: {
            if (payloadLength >= sizeof(ForcedReleasePayload)) {
                ForcedReleasePayload release;
                memcpy(&release, payload, sizeof(ForcedReleasePayload));
                ipNegotiator_.handleForcedRelease(release, senderSteamID);
            }
            break;
        }
            
        case VpnMessageType::HEARTBEAT: {
            if (payloadLength >= sizeof(HeartbeatPayload)) {
                HeartbeatPayload heartbeat;
                memcpy(&heartbeat, payload, sizeof(HeartbeatPayload));
                heartbeatManager_.handleHeartbeat(heartbeat, senderSteamID, peerName);
            }
            break;
        }
            
        default:
            break;
    }
}

void SteamVpnBridge::onUserJoined(CSteamID steamID) {
    std::cout << "User joined: " << steamID.ConvertToUint64() << std::endl;
    
    if (ipNegotiator_.getState() == NegotiationState::STABLE) {
        ipNegotiator_.sendAddressAnnounceTo(steamID);
        routeManager_.sendRouteUpdateTo(steamID);
    }
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
    std::cout << "User left: " << steamID.ConvertToUint64() << std::endl;
    
    routeManager_.removeRoutesForUser(steamID, [this](uint32_t ip, const NodeID& nodeId) {
        heartbeatManager_.unregisterNode(nodeId);
        ipNegotiator_.markIPUnused(ip);
    });
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void SteamVpnBridge::onNegotiationSuccess(uint32_t ipAddress, const NodeID& nodeId) {
    localIP_ = ipAddress;
    
    std::string localIPStr = ipToString(localIP_);
    std::string subnetMaskStr = ipToString(subnetMask_);
    
    if (tunDevice_->set_ip(localIPStr, subnetMaskStr) && tunDevice_->set_up(true)) {
        CSteamID mySteamID = SteamUser()->GetSteamID();
        routeManager_.updateRoute(nodeId, mySteamID, localIP_, SteamFriends()->GetPersonaName());
        
        heartbeatManager_.initialize(nodeId, localIP_);
        heartbeatManager_.registerNode(nodeId, mySteamID, localIP_, SteamFriends()->GetPersonaName());
        heartbeatManager_.start();
        
        routeManager_.broadcastRouteUpdate();
    } else {
        std::cerr << "Failed to configure TUN device IP." << std::endl;
    }
}

void SteamVpnBridge::onNodeExpired(const NodeID& nodeId, uint32_t ipAddress) {
    routeManager_.removeRoute(ipAddress);
    ipNegotiator_.markIPUnused(ipAddress);
}

void SteamVpnBridge::sendVpnMessage(VpnMessageType type, const uint8_t* payload, 
                                     size_t payloadLength, CSteamID targetSteamID, bool reliable) {
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = type;
    header.length = htons(static_cast<uint16_t>(payloadLength));
    
    message.resize(sizeof(VpnMessageHeader) + payloadLength);
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    if (payloadLength > 0 && payload) {
        memcpy(message.data() + sizeof(VpnMessageHeader), payload, payloadLength);
    }
    
    int flags = reliable ? k_nSteamNetworkingSend_Reliable : 
                           (k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay);
    steamManager_->sendMessageToUser(targetSteamID, message.data(), 
        static_cast<uint32_t>(message.size()), flags);
}

void SteamVpnBridge::broadcastVpnMessage(VpnMessageType type, const uint8_t* payload, 
                                          size_t payloadLength, bool reliable) {
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = type;
    header.length = htons(static_cast<uint16_t>(payloadLength));
    
    message.resize(sizeof(VpnMessageHeader) + payloadLength);
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    if (payloadLength > 0 && payload) {
        memcpy(message.data() + sizeof(VpnMessageHeader), payload, payloadLength);
    }
    
    int flags = reliable ? k_nSteamNetworkingSend_Reliable : 
                           (k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay);
    steamManager_->broadcastMessage(message.data(), static_cast<uint32_t>(message.size()), flags);
}