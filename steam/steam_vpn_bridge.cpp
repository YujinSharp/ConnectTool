#include "steam_vpn_bridge.h"
#include "steam_networking_manager.h"
#include "config/config_manager.h"
#include <iostream>
#include <cstring>
#include <vector>
#include <isteamnetworkingutils.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

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

/**
 * @brief 查询 Steam Networking 的 MTU 数据大小限制
 * @return 可用于发送的最大数据大小，失败时返回默认值
 */
static int querySteamMtuDataSize() {
    ISteamNetworkingUtils* pUtils = SteamNetworkingUtils();
    if (!pUtils) {
        std::cerr << "[MTU] SteamNetworkingUtils not available, using default MTU" << std::endl;
        return RECOMMENDED_MTU;  // 使用默认值
    }
    
    int32 mtuDataSize = 0;
    size_t cbResult = sizeof(mtuDataSize);
    ESteamNetworkingConfigDataType dataType;
    
    ESteamNetworkingGetConfigValueResult result = pUtils->GetConfigValue(
        k_ESteamNetworkingConfig_MTU_DataSize,
        k_ESteamNetworkingConfig_Global,
        0,  // scopeObj (ignored for global)
        &dataType,
        &mtuDataSize,
        &cbResult
    );
    
    if (result == k_ESteamNetworkingGetConfigValue_OK || 
        result == k_ESteamNetworkingGetConfigValue_OKInherited) {
        std::cout << "[MTU] Steam MTU_DataSize from API: " << mtuDataSize << " bytes" << std::endl;
        return mtuDataSize;
    } else {
        std::cerr << "[MTU] Failed to query MTU_DataSize (result=" << result 
                  << "), using default: " << RECOMMENDED_MTU << std::endl;
        return RECOMMENDED_MTU;
    }
}

/**
 * @brief 计算适合的 TUN MTU
 * @param steamMtuDataSize Steam 允许的最大不分片数据大小
 * @return 应该设置给 TUN 设备的 MTU 值
 */
static int calculateTunMtu(int steamMtuDataSize) {
    // 减去 VPN 封装开销: VpnMessageHeader (3) + VpnPacketWrapper (32) = 35 bytes
    // 再减去一些安全余量 (15 bytes)
    int tunMtu = steamMtuDataSize - static_cast<int>(VPN_MESSAGE_OVERHEAD) - 15;
    
    // 确保 MTU 在合理范围内
    if (tunMtu < 576) {
        tunMtu = 576;  // IPv4 最小 MTU
    } else if (tunMtu > 1500) {
        tunMtu = 1500;  // 以太网标准 MTU
    }
    
    std::cout << "[MTU] Calculated TUN MTU: " << tunMtu 
              << " (Steam limit: " << steamMtuDataSize 
              << ", overhead: " << VPN_MESSAGE_OVERHEAD << ")" << std::endl;
    
    return tunMtu;
}

bool SteamVpnBridge::start(const std::string& tunDeviceName,
                            const std::string& virtualSubnet,
                            const std::string& subnetMask) {
    if (running_) {
        std::cerr << "VPN bridge is already running" << std::endl;
        return false;
    }

    // 获取配置
    const auto& config = ConfigManager::instance().getConfig();
    
    // 运行时查询 Steam 的 MTU 数据大小限制，并计算合适的 TUN MTU
    int steamMtuDataSize = querySteamMtuDataSize();
    int mtu = calculateTunMtu(steamMtuDataSize);
    
    // 如果配置文件中的 MTU 比计算值更小，使用配置值（更保守）
    if (config.vpn.default_mtu > 0 && config.vpn.default_mtu < mtu) {
        std::cout << "[MTU] Using config MTU (" << config.vpn.default_mtu 
                  << ") instead of calculated (" << mtu << ")" << std::endl;
        mtu = config.vpn.default_mtu;
    }

    // 创建TUN设备
    tunDevice_ = tun::create_tun();
    if (!tunDevice_) {
        std::cerr << "Failed to create TUN device" << std::endl;
        return false;
    }

    // 打开TUN设备（使用配置的 MTU）
    if (!tunDevice_->open(tunDeviceName, mtu)) { 
        std::cerr << "Failed to open TUN device: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device created: " << tunDevice_->get_device_name() << std::endl;

    // 设置 TUN 设备的 MTU
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

    // 初始化 IP 协商器
    CSteamID mySteamID = SteamUser()->GetSteamID();
    ipNegotiator_.initialize(mySteamID, baseIP_, subnetMask_);
    
    // 设置回调 - 使用 CSteamID 而非连接句柄
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
    
    // 初始化心跳管理器
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

    // 开始IP协商
    ipNegotiator_.startNegotiation();

    // 使用阻塞模式，让 WinTUN 内部使用事件等待，避免 CPU 空转
    tunDevice_->set_non_blocking(false);

    // 启动处理线程
    running_ = true;
    tunReadThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunReadThread, this);
    
    std::cout << "Steam VPN bridge started successfully" << std::endl;
    return true;
}

void SteamVpnBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // 停止心跳管理器
    heartbeatManager_.stop();

    // 等待线程结束
    if (tunReadThread_ && tunReadThread_->joinable()) {
        tunReadThread_->join();
    }

    // 关闭TUN设备
    if (tunDevice_) {
        tunDevice_->close();
    }

    // 清理路由表
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        routingTable_.clear();
    }

    localIP_ = 0;

    std::cout << "Steam VPN bridge stopped" << std::endl;
}

std::string SteamVpnBridge::getLocalIP() const {
    if (localIP_ == 0) {
        return "Not assigned";
    }
    return ipToString(localIP_);
}

std::string SteamVpnBridge::getTunDeviceName() const {
    if (tunDevice_ && tunDevice_->is_open()) {
        return tunDevice_->get_device_name();
    }
    return "N/A";
}

std::map<uint32_t, RouteEntry> SteamVpnBridge::getRoutingTable() const {
    std::lock_guard<std::mutex> lock(routingMutex_);
    return routingTable_;
}

void SteamVpnBridge::tunReadThread() {
    std::cout << "TUN read thread started" << std::endl;
    
    // 使用合理大小的缓冲区（16KB 足够处理大多数 MTU 配置）
    // 使用 std::vector 避免栈溢出风险
    constexpr size_t BUFFER_SIZE = 16384;  // 16KB
    std::vector<uint8_t> buffer(BUFFER_SIZE);
    std::vector<uint8_t> vpnPacketBuffer(BUFFER_SIZE + sizeof(VpnMessageHeader) + sizeof(VpnPacketWrapper));
    
    auto lastTimeoutCheck = std::chrono::steady_clock::now();
    
    while (running_) {
        // 从TUN设备读取数据包
        int bytesRead = tunDevice_->read(buffer.data(), buffer.size());
        
        if (bytesRead > 0) {
            // 提取目标IP
            uint32_t destIP = extractDestIP(buffer.data(), bytesRead);
            
            // 封装VPN消息（包含 Node ID）
            // 使用预分配的 vpnPacketBuffer 避免每次循环动态分配
            VpnMessageHeader* header = reinterpret_cast<VpnMessageHeader*>(vpnPacketBuffer.data());
            header->type = VpnMessageType::IP_PACKET;
            
            VpnPacketWrapper* wrapper = reinterpret_cast<VpnPacketWrapper*>(vpnPacketBuffer.data() + sizeof(VpnMessageHeader));
            wrapper->senderNodeId = ipNegotiator_.getLocalNodeID();
            
            size_t totalPayloadSize = sizeof(VpnPacketWrapper) + bytesRead;
            header->length = htons(static_cast<uint16_t>(totalPayloadSize));
            
            memcpy(vpnPacketBuffer.data() + sizeof(VpnMessageHeader) + sizeof(VpnPacketWrapper), buffer.data(), bytesRead);
            uint32_t vpnPacketSize = static_cast<uint32_t>(sizeof(VpnMessageHeader) + totalPayloadSize);

            if (isBroadcastAddress(destIP)) {
                // 广播包 - 发送给房间内所有成员
                steamManager_->broadcastMessage(vpnPacketBuffer.data(), vpnPacketSize, 
                    k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_NoDelay);
                
                auto members = steamManager_->getRoomMembers();
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsSent += members.size();
                stats_.bytesSent += bytesRead * members.size();
            } else {
                // 单播包 - 查找目标节点
                CSteamID targetSteamID;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(destIP);
                    if (it != routingTable_.end() && !it->second.isLocal) {
                        targetSteamID = it->second.steamID;
                        found = true;
                    }
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
        
        // 定期检查协商超时（每 50ms 检查一次）
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
    
    // 快速路径：IP_PACKET 是最常见的消息类型，优先处理
    if (header.type == VpnMessageType::IP_PACKET) {
        if (tunDevice_ && payloadLength > sizeof(VpnPacketWrapper)) {
            const uint8_t* ipPacket = payload + sizeof(VpnPacketWrapper);
            size_t ipPacketLen = payloadLength - sizeof(VpnPacketWrapper);
            
            // 检查目标 IP
            uint32_t destIP = extractDestIP(ipPacket, ipPacketLen);
            
            // 如果是发给本地的包，或者是广播包，写入 TUN 设备
            if (destIP == localIP_ || isBroadcastAddress(destIP)) {
                tunDevice_->write(ipPacket, ipPacketLen);
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsReceived++;
                stats_.bytesReceived += ipPacketLen;
            }
            // 如果是发给其他节点的包，转发（P2P 中继）
            else {
                CSteamID targetSteamID;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(destIP);
                    if (it != routingTable_.end() && !it->second.isLocal) {
                        targetSteamID = it->second.steamID;
                        found = true;
                    }
                }
                
                // 不要转发回发送者
                if (found && targetSteamID != senderSteamID) {
                    sendVpnMessage(VpnMessageType::IP_PACKET, payload, payloadLength, targetSteamID, false);
                }
            }
        }
        return;  // IP_PACKET 处理完毕，直接返回
    }

    // 慢速路径：其他消息类型（较少发生）
    switch (header.type) {
        case VpnMessageType::ROUTE_UPDATE: {
            size_t offset = 0;
            while (offset + 12 <= payloadLength) {
                uint64_t steamID;
                uint32_t ipAddress;
                memcpy(&steamID, payload + offset, 8);
                memcpy(&ipAddress, payload + offset + 8, 4);
                ipAddress = ntohl(ipAddress);
                offset += 12;

                CSteamID csteamID(steamID);
                
                // 跳过自己的路由
                if (csteamID == SteamUser()->GetSteamID()) {
                    continue;
                }
                
                // 检查是否已经有这个路由
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(ipAddress);
                    if (it != routingTable_.end()) {
                        continue; // 已有这个路由，跳过
                    }
                }

                if ((ipAddress & subnetMask_) == (baseIP_ & subnetMask_)) {
                    NodeID nodeId = NodeIdentity::generate(csteamID);
                    updateRoute(nodeId, csteamID, ipAddress,
                                SteamFriends()->GetFriendPersonaName(csteamID));
                }
            }
            
            // 注意：不再在收到 ROUTE_UPDATE 后自动广播
            // 路由传播只通过 ADDRESS_ANNOUNCE 和新用户加入时主动发送
            // 这样可以避免路由风暴（每个节点收到更新后又广播导致无限循环）
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
                
                // 检查是否是新的路由
                uint32_t announcedIP = ntohl(announce.ipAddress);
                bool isNewRoute = false;
                {
                    std::lock_guard<std::mutex> lock(routingMutex_);
                    auto it = routingTable_.find(announcedIP);
                    isNewRoute = (it == routingTable_.end());
                }
                
                ipNegotiator_.handleAddressAnnounce(announce, senderSteamID, peerName);
                
                // 更新路由表
                updateRoute(announce.nodeId, senderSteamID, announcedIP, peerName);
                
                // 如果是新路由，广播整个路由表给所有人
                if (isNewRoute) {
                    broadcastRouteUpdate();
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
    
    // 不再主动发送 SESSION_HELLO
    // Steam 的 ISteamNetworkingMessages 会在发送实际消息时通过 
    // k_nSteamNetworkingSend_AutoRestartBrokenSession 自动建立连接
    
    // 如果本地已经有稳定的 IP，发送自己的地址宣布给新加入的用户
    // 这个消息本身会触发连接建立
    if (ipNegotiator_.getState() == NegotiationState::STABLE) {
        ipNegotiator_.sendAddressAnnounceTo(steamID);
        
        // 同时把完整的路由表发送给新用户
        sendRouteUpdateTo(steamID);
    }
}

void SteamVpnBridge::onSessionHelloReceived(CSteamID senderSteamID) {
    std::cout << "Received SESSION_HELLO from " << senderSteamID.ConvertToUint64() << std::endl;
    
    // 如果本地已经有稳定的 IP，回复自己的地址宣布和路由表
    // 这确保了即使 OnLobbyChatUpdate 回调时序有问题，对方也能收到我们的地址信息
    if (ipNegotiator_.getState() == NegotiationState::STABLE) {
        std::cout << "Replying with ADDRESS_ANNOUNCE and route table to " 
                  << senderSteamID.ConvertToUint64() << std::endl;
        ipNegotiator_.sendAddressAnnounceTo(senderSteamID);
        sendRouteUpdateTo(senderSteamID);
    }
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
    std::cout << "User left: " << steamID.ConvertToUint64() << std::endl;
    
    // 从路由表中移除
    std::lock_guard<std::mutex> lock(routingMutex_);
    for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
        if (it->second.steamID == steamID) {
            heartbeatManager_.unregisterNode(it->second.nodeId);
            ipNegotiator_.markIPUnused(it->first);
            it = routingTable_.erase(it);
        } else {
            ++it;
        }
    }
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void SteamVpnBridge::onNegotiationSuccess(uint32_t ipAddress, const NodeID& nodeId) {
    localIP_ = ipAddress;
    
    // 配置 TUN 设备
    std::string localIPStr = ipToString(localIP_);
    std::string subnetMaskStr = ipToString(subnetMask_);
    
    if (tunDevice_->set_ip(localIPStr, subnetMaskStr) && tunDevice_->set_up(true)) {
        // 添加本地路由
        CSteamID mySteamID = SteamUser()->GetSteamID();
        updateRoute(nodeId, mySteamID, localIP_, SteamFriends()->GetPersonaName());
        
        // 初始化并启动心跳管理器
        heartbeatManager_.initialize(nodeId, localIP_);
        heartbeatManager_.registerNode(nodeId, mySteamID, localIP_, SteamFriends()->GetPersonaName());
        heartbeatManager_.start();
        
        broadcastRouteUpdate();
    } else {
        std::cerr << "Failed to configure TUN device IP." << std::endl;
    }
}

void SteamVpnBridge::onNodeExpired(const NodeID& nodeId, uint32_t ipAddress) {
    removeRoute(ipAddress);
    ipNegotiator_.markIPUnused(ipAddress);
}

void SteamVpnBridge::updateRoute(const NodeID& nodeId, CSteamID steamId, uint32_t ipAddress,
                                  const std::string& name) {
    RouteEntry entry;
    entry.steamID = steamId;
    entry.ipAddress = ipAddress;
    entry.name = name;
    entry.isLocal = (steamId == SteamUser()->GetSteamID());
    entry.nodeId = nodeId;
    
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        // 移除该 SteamID 的旧条目
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ) {
            if (it->second.steamID == steamId && it->first != ipAddress) {
                it = routingTable_.erase(it);
            } else {
                ++it;
            }
        }
        routingTable_[ipAddress] = entry;
    }
    
    // 标记 IP 为已使用
    ipNegotiator_.markIPUsed(ipAddress);
    
    std::cout << "Route updated: " << ipToString(ipAddress) << " -> " << name << std::endl;
}

void SteamVpnBridge::removeRoute(uint32_t ipAddress) {
    std::lock_guard<std::mutex> lock(routingMutex_);
    routingTable_.erase(ipAddress);
}

void SteamVpnBridge::broadcastRouteUpdate() {
    std::vector<uint8_t> message;
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

    VpnMessageHeader header;
    header.type = VpnMessageType::ROUTE_UPDATE;
    header.length = htons(static_cast<uint16_t>(routeData.size()));

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    steamManager_->broadcastMessage(message.data(), static_cast<uint32_t>(message.size()),
        k_nSteamNetworkingSend_Reliable);
}

void SteamVpnBridge::sendRouteUpdateTo(CSteamID targetSteamID) {
    std::vector<uint8_t> message;
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

    VpnMessageHeader header;
    header.type = VpnMessageType::ROUTE_UPDATE;
    header.length = htons(static_cast<uint16_t>(routeData.size()));

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    steamManager_->sendMessageToUser(targetSteamID, message.data(), 
        static_cast<uint32_t>(message.size()), k_nSteamNetworkingSend_Reliable);
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

std::string SteamVpnBridge::ipToString(uint32_t ip) {
    char buffer[INET_ADDRSTRLEN];
    struct in_addr addr;
    addr.s_addr = htonl(ip);
    inet_ntop(AF_INET, &addr, buffer, INET_ADDRSTRLEN);
    return std::string(buffer);
}

uint32_t SteamVpnBridge::stringToIp(const std::string& ipStr) {
    struct in_addr addr;
    if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
        return ntohl(addr.s_addr);
    }
    return 0;
}

uint32_t SteamVpnBridge::extractDestIP(const uint8_t* packet, size_t length) {
    if (length < 20) return 0;
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) return 0;

    uint32_t destIP;
    memcpy(&destIP, packet + 16, 4);
    return ntohl(destIP);
}

uint32_t SteamVpnBridge::extractSourceIP(const uint8_t* packet, size_t length) {
    if (length < 20) return 0;
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) return 0;

    uint32_t srcIP;
    memcpy(&srcIP, packet + 12, 4);
    return ntohl(srcIP);
}

bool SteamVpnBridge::isBroadcastAddress(uint32_t ip) const {
    if (ip == 0xFFFFFFFF) return true;
    
    uint32_t subnetBroadcast = (baseIP_ & subnetMask_) | (~subnetMask_);
    if (ip == subnetBroadcast) return true;
    
    uint8_t firstOctet = (ip >> 24) & 0xFF;
    if (firstOctet >= 224 && firstOctet <= 239) return true;
    
    return false;
}