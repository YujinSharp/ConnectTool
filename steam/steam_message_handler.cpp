#include "steam_message_handler.h"
#include "steam_networking_manager.h"
#include "steam_message_handler.h"
#include "steam_vpn_bridge.h"
#include <iostream>
#include <cstring>
#include <chrono>
#include <steam_api.h>
#include <isteamnetworkingsockets.h>

SteamMessageHandler::SteamMessageHandler(ISteamNetworkingSockets* interface, std::vector<HSteamNetConnection>& connections, std::mutex& connectionsMutex, SteamNetworkingManager* manager)
    : m_pInterface_(interface), connections_(connections), connectionsMutex_(connectionsMutex), manager_(manager), running_(false), currentPollInterval_(0) {}

SteamMessageHandler::~SteamMessageHandler() {
    stop();
}

void SteamMessageHandler::start() {
    if (running_) return;
    running_ = true;
    pollThread_ = std::make_unique<std::thread>(&SteamMessageHandler::pollLoop, this);
}

void SteamMessageHandler::stop() {
    if (!running_) return;
    running_ = false;
    if (pollThread_ && pollThread_->joinable()) {
        pollThread_->join();
    }
}

void SteamMessageHandler::pollLoop() {
    while (running_) {
        // Poll networking callbacks
        m_pInterface_->RunCallbacks();
        
        // Receive messages and check if any were received
        int totalMessages = 0;
        std::vector<HSteamNetConnection> currentConnections;
        {
            std::lock_guard<std::mutex> lockConn(connectionsMutex_);
            currentConnections = connections_;
        }
        for (auto conn : currentConnections) {
            ISteamNetworkingMessage* pIncomingMsgs[10];
            int numMsgs = m_pInterface_->ReceiveMessagesOnConnection(conn, pIncomingMsgs, 10);
            totalMessages += numMsgs;
            for (int i = 0; i < numMsgs; ++i) {
                ISteamNetworkingMessage* pIncomingMsg = pIncomingMsgs[i];
                const uint8_t* data = (const uint8_t*)pIncomingMsg->m_pData;
                size_t size = pIncomingMsg->m_cbSize;
                
                // Check if this is a VPN message (first byte indicates message type)
                // VpnMessageType enum values range from 1 to 7
                if (size > 0 && data[0] >= 1 && data[0] <= 7) {
                    // This might be a VPN message, forward to VPN bridge
                    if (manager_) {
                        SteamVpnBridge* bridge = manager_->getVpnBridge();
                        if (bridge) {
                            bridge->handleVpnMessage(data, size, conn);
                        }
                    }
                }
                
                pIncomingMsg->Release();
            }
        }
        
        // Adaptive polling: if messages received, poll immediately; otherwise increase interval
        if (totalMessages > 0) {
            currentPollInterval_ = 0; // 有消息，立即轮询
        } else {
            // 无消息，逐渐增加间隔，最大1ms (Reduced from 10ms for lower latency)
            currentPollInterval_ = std::min(currentPollInterval_ + 1, 1);
        }
        
        if (currentPollInterval_ > 0) {
            // std::this_thread::sleep_for(std::chrono::milliseconds(currentPollInterval_));
             std::this_thread::sleep_for(std::chrono::microseconds(100)); // Sleep 0.1ms
        } else {
            std::this_thread::yield();
        }
    }
}

