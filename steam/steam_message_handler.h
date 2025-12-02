#ifndef STEAM_MESSAGE_HANDLER_H
#define STEAM_MESSAGE_HANDLER_H

#include <vector>
#include <mutex>
#include <thread>
#include <memory>
#include <atomic>
#include <steamnetworkingtypes.h>
#include <isteamnetworkingsockets.h>
#include <asio.hpp>

class SteamNetworkingManager;

/**
 * @brief Steam 网络消息处理器（Asio 版本）
 * 
 * 使用 Asio 定时器替代传统的 sleep 循环，实现更高效的消息轮询
 */
class SteamMessageHandler {
public:
    SteamMessageHandler(ISteamNetworkingSockets* interface, 
                        std::vector<HSteamNetConnection>& connections, 
                        std::mutex& connectionsMutex, 
                        SteamNetworkingManager* manager);
    ~SteamMessageHandler();

    void start();
    void stop();
    
    /**
     * @brief 设置外部 io_context（可选，用于共享事件循环）
     */
    void setIoContext(asio::io_context* externalContext);

private:
    void schedulePoll();
    void pollMessages();
    void runInternalLoop();

    ISteamNetworkingSockets* m_pInterface_;
    std::vector<HSteamNetConnection>& connections_;
    std::mutex& connectionsMutex_;
    SteamNetworkingManager* manager_;

    // Asio 相关
    std::unique_ptr<asio::io_context> internalIoContext_;
    asio::io_context* ioContext_;  // 指向当前使用的 io_context
    std::unique_ptr<asio::steady_timer> pollTimer_;
    std::unique_ptr<std::thread> ioThread_;
    
    std::atomic<bool> running_;
    std::chrono::microseconds currentPollInterval_;
    
    static constexpr std::chrono::microseconds MIN_POLL_INTERVAL{100};    // 0.1ms
    static constexpr std::chrono::microseconds MAX_POLL_INTERVAL{1000};   // 1ms
    static constexpr std::chrono::microseconds POLL_INCREMENT{100};       // 0.1ms
};

#endif // STEAM_MESSAGE_HANDLER_H