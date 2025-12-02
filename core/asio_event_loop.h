#ifndef ASIO_EVENT_LOOP_H
#define ASIO_EVENT_LOOP_H

#include <memory>
#include <thread>
#include <functional>
#include <asio.hpp>

/**
 * @brief Asio 事件循环管理器
 * 
 * 提供统一的 io_context 用于整个应用程序的异步操作
 */
class AsioEventLoop {
public:
    /**
     * @brief 获取单例实例
     */
    static AsioEventLoop& instance() {
        static AsioEventLoop inst;
        return inst;
    }

    /**
     * @brief 获取 io_context 引用
     */
    asio::io_context& getContext() {
        return ioContext_;
    }

    /**
     * @brief 启动事件循环（在当前线程运行）
     */
    void run() {
        workGuard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(ioContext_)
        );
        ioContext_.run();
    }

    /**
     * @brief 启动事件循环（在后台线程运行）
     */
    void runInBackground() {
        if (backgroundThread_ && backgroundThread_->joinable()) {
            return; // 已经在运行
        }
        workGuard_ = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
            asio::make_work_guard(ioContext_)
        );
        backgroundThread_ = std::make_unique<std::thread>([this]() {
            ioContext_.run();
        });
    }

    /**
     * @brief 停止事件循环
     */
    void stop() {
        if (workGuard_) {
            workGuard_.reset();
        }
        ioContext_.stop();
        if (backgroundThread_ && backgroundThread_->joinable()) {
            backgroundThread_->join();
            backgroundThread_.reset();
        }
    }

    /**
     * @brief 重置事件循环（用于停止后重新启动）
     */
    void reset() {
        ioContext_.restart();
    }

    /**
     * @brief 投递任务到事件循环
     */
    template<typename Handler>
    void post(Handler&& handler) {
        asio::post(ioContext_, std::forward<Handler>(handler));
    }

    /**
     * @brief 创建一个定时器
     */
    std::unique_ptr<asio::steady_timer> createTimer() {
        return std::make_unique<asio::steady_timer>(ioContext_);
    }

private:
    AsioEventLoop() = default;
    ~AsioEventLoop() {
        stop();
    }

    AsioEventLoop(const AsioEventLoop&) = delete;
    AsioEventLoop& operator=(const AsioEventLoop&) = delete;

    asio::io_context ioContext_;
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> workGuard_;
    std::unique_ptr<std::thread> backgroundThread_;
};

#endif // ASIO_EVENT_LOOP_H
