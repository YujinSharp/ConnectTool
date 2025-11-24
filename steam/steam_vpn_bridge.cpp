#include "steam_vpn_bridge.h"
#include "steam_networking_manager.h"
#include <iostream>
#include <cstring>
#include <arpa/inet.h>
#include <algorithm>

SteamVpnBridge::SteamVpnBridge(SteamNetworkingManager* steamManager)
    : steamManager_(steamManager)
    , running_(false)
    , baseIP_(0)
    , subnetMask_(0)
    , nextIP_(0)
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

    // 创建TUN设备
    tunDevice_ = tun::create_tun();
    if (!tunDevice_) {
        std::cerr << "Failed to create TUN device" << std::endl;
        return false;
    }

    // 打开TUN设备
    if (!tunDevice_->open(tunDeviceName, 1400)) { // MTU设置为1400以留出Steam封装开销
        std::cerr << "Failed to open TUN device: " << tunDevice_->get_last_error() << std::endl;
        return false;
    }

    std::cout << "TUN device created: " << tunDevice_->get_device_name() << std::endl;

    // 初始化IP地址池
    baseIP_ = stringToIp(virtualSubnet);
    subnetMask_ = stringToIp(subnetMask);
    nextIP_ = baseIP_ + 1;  // .0 通常保留给网络地址

    // 根据SteamID分配本地IP（确保每个用户的IP是确定的）
    CSteamID mySteamID = SteamUser()->GetSteamID();
    localIP_ = generateIPFromSteamID(mySteamID);

    if (localIP_ != 0) {
        // 设置TUN设备IP
        std::string localIPStr = ipToString(localIP_);
        if (!tunDevice_->set_ip(localIPStr, subnetMask)) {
            std::cerr << "Failed to set IP address: " << tunDevice_->get_last_error() << std::endl;
            tunDevice_->close();
            return false;
        }

        // 启用设备
        if (!tunDevice_->set_up()) {
            std::cerr << "Failed to bring up TUN device: " << tunDevice_->get_last_error() << std::endl;
            tunDevice_->close();
            return false;
        }

        std::cout << "VPN local IP: " << localIPStr << std::endl;

        // 添加本地路由
        RouteEntry localRoute;
        localRoute.steamID = SteamUser()->GetSteamID();
        localRoute.conn = k_HSteamNetConnection_Invalid;
        localRoute.ipAddress = localIP_;
        localRoute.name = SteamFriends()->GetPersonaName();
        localRoute.isLocal = true;

        {
            std::lock_guard<std::mutex> lock(routingMutex_);
            routingTable_[localIP_] = localRoute;
        }
    }

    // 设置非阻塞模式
    tunDevice_->set_non_blocking(true);

    // 启动处理线程
    running_ = true;
    tunReadThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunReadThread, this);
    tunWriteThread_ = std::make_unique<std::thread>(&SteamVpnBridge::tunWriteThread, this);

    std::cout << "Steam VPN bridge started successfully" << std::endl;
    return true;
}

void SteamVpnBridge::stop() {
    if (!running_) {
        return;
    }

    running_ = false;

    // 等待线程结束
    if (tunReadThread_ && tunReadThread_->joinable()) {
        tunReadThread_->join();
    }
    if (tunWriteThread_ && tunWriteThread_->joinable()) {
        tunWriteThread_->join();
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

    // 清理IP分配
    {
        std::lock_guard<std::mutex> lock(ipAllocationMutex_);
        allocatedIPs_.clear();
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
    
    uint8_t buffer[2048];
    
    while (running_) {
        // 从TUN设备读取数据包
        int bytesRead = tunDevice_->read(buffer, sizeof(buffer));
        
        if (bytesRead > 0) {
            // 提取目标IP
            uint32_t destIP = extractDestIP(buffer, bytesRead);
            
            if (destIP == 0) {
                // 无效的IP包
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsDropped++;
                continue;
            }

            // 查找路由
            HSteamNetConnection targetConn = k_HSteamNetConnection_Invalid;
            {
                std::lock_guard<std::mutex> lock(routingMutex_);
                auto it = routingTable_.find(destIP);
                if (it != routingTable_.end()) {
                    if (!it->second.isLocal) {
                        targetConn = it->second.conn;
                    } else {
                        // 发送给本地，忽略
                        continue;
                    }
                } else {
                    // 如果是广播或未知目标，发送给所有连接
                    // 这里简化处理，丢弃未知目标的包
                    std::lock_guard<std::mutex> lock2(statsMutex_);
                    stats_.packetsDropped++;
                    continue;
                }
            }

            if (targetConn != k_HSteamNetConnection_Invalid) {
                // 封装VPN消息
                std::vector<uint8_t> vpnPacket;
                VpnMessageHeader header;
                header.type = VpnMessageType::IP_PACKET;
                header.length = htons(bytesRead);
                
                vpnPacket.resize(sizeof(VpnMessageHeader) + bytesRead);
                memcpy(vpnPacket.data(), &header, sizeof(VpnMessageHeader));
                memcpy(vpnPacket.data() + sizeof(VpnMessageHeader), buffer, bytesRead);

                // 通过Steam发送
                ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
                EResult result = steamInterface->SendMessageToConnection(
                    targetConn,
                    vpnPacket.data(),
                    vpnPacket.size(),
                    k_nSteamNetworkingSend_Reliable,
                    nullptr
                );

                if (result == k_EResultOK) {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    stats_.packetsSent++;
                    stats_.bytesSent += bytesRead;
                } else {
                    std::lock_guard<std::mutex> lock(statsMutex_);
                    stats_.packetsDropped++;
                }
            }
        } else if (bytesRead < 0) {
            // 读取错误，等待一下再试
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else {
            // 没有数据，等待一下
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    std::cout << "TUN read thread stopped" << std::endl;
}

void SteamVpnBridge::tunWriteThread() {
    std::cout << "TUN write thread started" << std::endl;
    
    while (running_) {
        std::vector<OutgoingPacket> packetsToSend;
        
        {
            std::lock_guard<std::mutex> lock(sendQueueMutex_);
            if (!sendQueue_.empty()) {
                packetsToSend = std::move(sendQueue_);
                sendQueue_.clear();
            }
        }
        
        for (const auto& packet : packetsToSend) {
            // 写入TUN设备
            int bytesWritten = tunDevice_->write(packet.data.data(), packet.data.size());
            
            if (bytesWritten > 0) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsReceived++;
                stats_.bytesReceived += bytesWritten;
            } else {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.packetsDropped++;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    
    std::cout << "TUN write thread stopped" << std::endl;
}

void SteamVpnBridge::handleVpnMessage(const uint8_t* data, size_t length, HSteamNetConnection fromConn) {
    if (length < sizeof(VpnMessageHeader)) {
        return;
    }

    VpnMessageHeader header;
    memcpy(&header, data, sizeof(VpnMessageHeader));
    uint16_t payloadLength = ntohs(header.length);

    if (length < sizeof(VpnMessageHeader) + payloadLength) {
        return;
    }

    const uint8_t* payload = data + sizeof(VpnMessageHeader);

    switch (header.type) {
        case VpnMessageType::IP_PACKET: {
            // 将IP包写入TUN设备
            OutgoingPacket packet;
            packet.data.resize(payloadLength);
            memcpy(packet.data.data(), payload, payloadLength);
            packet.targetConn = fromConn;

            std::lock_guard<std::mutex> lock(sendQueueMutex_);
            sendQueue_.push_back(std::move(packet));
            break;
        }

        case VpnMessageType::IP_ASSIGNMENT: {
            if (payloadLength >= 4) {
                uint32_t assignedIP;
                memcpy(&assignedIP, payload, 4);
                assignedIP = ntohl(assignedIP);

                localIP_ = assignedIP;

                // 配置TUN设备IP
                std::string ipStr = ipToString(assignedIP);
                std::string maskStr = ipToString(subnetMask_);
                
                if (tunDevice_->set_ip(ipStr, maskStr) && tunDevice_->set_up()) {
                    std::cout << "Received IP assignment: " << ipStr << std::endl;

                    // 添加本地路由
                    RouteEntry localRoute;
                    localRoute.steamID = SteamUser()->GetSteamID();
                    localRoute.conn = k_HSteamNetConnection_Invalid;
                    localRoute.ipAddress = localIP_;
                    localRoute.name = SteamFriends()->GetPersonaName();
                    localRoute.isLocal = true;

                    {
                        std::lock_guard<std::mutex> lock(routingMutex_);
                        routingTable_[localIP_] = localRoute;
                    }
                }
            }
            break;
        }

        case VpnMessageType::ROUTE_UPDATE: {
            // 路由表更新
            size_t offset = 0;
            while (offset + 12 <= payloadLength) {  // 12 = 8 (SteamID) + 4 (IP)
                uint64_t steamID;
                uint32_t ipAddress;
                memcpy(&steamID, payload + offset, 8);
                memcpy(&ipAddress, payload + offset + 8, 4);
                ipAddress = ntohl(ipAddress);
                offset += 12;

                CSteamID csteamID(steamID);
                
                // 查找连接
                HSteamNetConnection conn = k_HSteamNetConnection_Invalid;
                const auto& connections = steamManager_->getConnections();
                for (auto c : connections) {
                    SteamNetConnectionInfo_t info;
                    if (steamManager_->getInterface()->GetConnectionInfo(c, &info)) {
                        if (info.m_identityRemote.GetSteamID() == csteamID) {
                            conn = c;
                            break;
                        }
                    }
                }

                if (conn != k_HSteamNetConnection_Invalid) {
                    RouteEntry entry;
                    entry.steamID = csteamID;
                    entry.conn = conn;
                    entry.ipAddress = ipAddress;
                    entry.name = SteamFriends()->GetFriendPersonaName(csteamID);
                    entry.isLocal = false;

                    std::lock_guard<std::mutex> lock(routingMutex_);
                    routingTable_[ipAddress] = entry;
                    
                    std::cout << "Route updated: " << ipToString(ipAddress) 
                              << " -> " << entry.name << std::endl;
                }
            }
            break;
        }

        case VpnMessageType::PING:
        case VpnMessageType::PONG:
            // 心跳包处理（暂时忽略）
            break;
    }
}

void SteamVpnBridge::onUserJoined(CSteamID steamID, HSteamNetConnection conn) {
    // 生成对方的IP地址（基于SteamID）
    uint32_t newIP = generateIPFromSteamID(steamID);
    if (newIP == 0) {
        std::cerr << "Failed to generate IP for user " << steamID.ConvertToUint64() << std::endl;
        return;
    }

    // 添加到路由表
    RouteEntry entry;
    entry.steamID = steamID;
    entry.conn = conn;
    entry.ipAddress = newIP;
    entry.name = SteamFriends()->GetFriendPersonaName(steamID);
    entry.isLocal = false;

    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        routingTable_[newIP] = entry;
    }

    std::cout << "Added route for " << entry.name << " with IP " << ipToString(newIP) << std::endl;
}

void SteamVpnBridge::onUserLeft(CSteamID steamID) {
    // 从路由表中移除
    uint32_t ipToRemove = 0;
    {
        std::lock_guard<std::mutex> lock(routingMutex_);
        for (auto it = routingTable_.begin(); it != routingTable_.end(); ++it) {
            if (it->second.steamID == steamID) {
                ipToRemove = it->first;
                routingTable_.erase(it);
                break;
            }
        }
    }

    if (ipToRemove != 0) {
        std::cout << "Removed route for IP " << ipToString(ipToRemove) 
                  << " from user " << steamID.ConvertToUint64() << std::endl;
    }
}

SteamVpnBridge::Statistics SteamVpnBridge::getStatistics() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

uint32_t SteamVpnBridge::allocateIPAddress() {
    std::lock_guard<std::mutex> lock(ipAllocationMutex_);

    // 简单的顺序分配
    uint32_t maxIP = baseIP_ | (~subnetMask_);
    
    while (nextIP_ < maxIP) {
        uint32_t candidate = nextIP_++;
        
        // 跳过网络地址和广播地址
        if ((candidate & ~subnetMask_) == 0 || (candidate & ~subnetMask_) == ~subnetMask_) {
            continue;
        }
        
        // 检查是否已分配
        if (std::find(allocatedIPs_.begin(), allocatedIPs_.end(), candidate) == allocatedIPs_.end()) {
            allocatedIPs_.push_back(candidate);
            return candidate;
        }
    }

    return 0;  // 无可用IP
}

void SteamVpnBridge::releaseIPAddress(uint32_t ipAddress) {
    std::lock_guard<std::mutex> lock(ipAllocationMutex_);
    auto it = std::find(allocatedIPs_.begin(), allocatedIPs_.end(), ipAddress);
    if (it != allocatedIPs_.end()) {
        allocatedIPs_.erase(it);
    }
}

void SteamVpnBridge::sendIPAssignment(CSteamID steamID, HSteamNetConnection conn, uint32_t ipAddress) {
    std::vector<uint8_t> message;
    VpnMessageHeader header;
    header.type = VpnMessageType::IP_ASSIGNMENT;
    header.length = htons(4);

    message.resize(sizeof(VpnMessageHeader) + 4);
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    
    uint32_t ipNetwork = htonl(ipAddress);
    memcpy(message.data() + sizeof(VpnMessageHeader), &ipNetwork, 4);

    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    steamInterface->SendMessageToConnection(
        conn,
        message.data(),
        message.size(),
        k_nSteamNetworkingSend_Reliable,
        nullptr
    );
}

void SteamVpnBridge::broadcastRouteUpdate() {
    // 构建路由更新消息
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
    header.length = htons(routeData.size());

    message.resize(sizeof(VpnMessageHeader) + routeData.size());
    memcpy(message.data(), &header, sizeof(VpnMessageHeader));
    memcpy(message.data() + sizeof(VpnMessageHeader), routeData.data(), routeData.size());

    // 发送给所有连接
    ISteamNetworkingSockets* steamInterface = steamManager_->getInterface();
    const auto& connections = steamManager_->getConnections();
    
    for (auto conn : connections) {
        steamInterface->SendMessageToConnection(
            conn,
            message.data(),
            message.size(),
            k_nSteamNetworkingSend_Reliable,
            nullptr
        );
    }
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
    // IPv4包头最小20字节
    if (length < 20) {
        return 0;
    }

    // 检查IP版本
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) {
        return 0;  // 只支持IPv4
    }

    // 目标IP在偏移16-19字节
    uint32_t destIP;
    memcpy(&destIP, packet + 16, 4);
    return ntohl(destIP);
}

uint32_t SteamVpnBridge::extractSourceIP(const uint8_t* packet, size_t length) {
    // IPv4包头最小20字节
    if (length < 20) {
        return 0;
    }

    // 检查IP版本
    uint8_t version = (packet[0] >> 4) & 0x0F;
    if (version != 4) {
        return 0;  // 只支持IPv4
    }

    // 源IP在偏移12-15字节
    uint32_t srcIP;
    memcpy(&srcIP, packet + 12, 4);
    return ntohl(srcIP);
}

uint32_t SteamVpnBridge::generateIPFromSteamID(CSteamID steamID) {
    // 使用SteamID的低32位来生成IP地址
    uint64_t steamID64 = steamID.ConvertToUint64();
    uint32_t hash = static_cast<uint32_t>(steamID64 ^ (steamID64 >> 32));
    
    // 确保IP在子网范围内，并且不是网络地址或广播地址
    uint32_t maxOffset = (~subnetMask_) - 1;  // 减1避免广播地址
    uint32_t offset = (hash % maxOffset) + 1;  // 加1避免网络地址
    
    uint32_t ip = (baseIP_ & subnetMask_) | offset;
    
    return ip;
}
