#pragma once

#include "tun_interface.h"

#ifdef _WIN32

#include <windows.h>
#include <guiddef.h>

#include "../third_party/wintun/api/wintun.h"

namespace tun {

/**
 * @brief Windows 平台的 TUN 实现
 * 
 * 使用 Wintun 驱动（高性能，现代化）
 * 需要 wintun.dll（从 third_party/wintun 动态加载）
 */
class TunWindows : public TunInterface {
public:
    TunWindows();
    ~TunWindows() override;

    bool open(const std::string& device_name = "", uint32_t mtu = 1500) override;
    void close() override;
    bool is_open() const override;
    std::string get_device_name() const override;
    bool set_ip(const std::string& ip_address, const std::string& netmask) override;
    bool set_up() override;
    int read(uint8_t* buffer, size_t max_length) override;
    int write(const uint8_t* buffer, size_t length) override;
    std::string get_last_error() const override;
    uint32_t get_mtu() const override;
    bool set_non_blocking(bool non_blocking) override;
    uint32_t get_interface_index() const override;

private:
    WINTUN_ADAPTER_HANDLE adapter_;     // Wintun 适配器句柄
    WINTUN_SESSION_HANDLE session_;     // Wintun 会话句柄
    std::string device_name_;           // 设备名称
    uint32_t mtu_;                      // MTU
    std::string last_error_;            // 最后的错误信息
    bool non_blocking_;                 // 是否非阻塞
    HANDLE read_event_;                 // 读事件（用于非阻塞）
    GUID adapter_guid_;                 // 适配器 GUID
    
    // Wintun DLL 句柄
    HMODULE wintun_dll_;
    
    // Wintun API 函数指针
    // Wintun API 函数指针
    WINTUN_CREATE_ADAPTER_FUNC* WintunCreateAdapter_;
    WINTUN_OPEN_ADAPTER_FUNC* WintunOpenAdapter_;
    WINTUN_CLOSE_ADAPTER_FUNC* WintunCloseAdapter_;
    
    WINTUN_START_SESSION_FUNC* WintunStartSession_;
    WINTUN_END_SESSION_FUNC* WintunEndSession_;
    WINTUN_GET_READ_WAIT_EVENT_FUNC* WintunGetReadWaitEvent_;
    WINTUN_RECEIVE_PACKET_FUNC* WintunReceivePacket_;
    WINTUN_RELEASE_RECEIVE_PACKET_FUNC* WintunReleaseReceivePacket_;
    WINTUN_ALLOCATE_SEND_PACKET_FUNC* WintunAllocateSendPacket_;
    WINTUN_SEND_PACKET_FUNC* WintunSendPacket_;
    WINTUN_GET_ADAPTER_LUID_FUNC* WintunGetAdapterLUID_;
    WINTUN_SET_LOGGER_FUNC* WintunSetLogger_;

    static void CALLBACK wintun_logger_callback(WINTUN_LOGGER_LEVEL Level, DWORD64 Timestamp, LPCWSTR Message);
    
    bool load_wintun_dll();
    void unload_wintun_dll();

    // 辅助模板：加载 DLL 函数并自动转换类型
    template<typename T>
    T load_func(const char* func_name) {
        return reinterpret_cast<T>(GetProcAddress(wintun_dll_, func_name));
    }

    std::wstring string_to_wstring(const std::string& str);
    std::string get_windows_error(DWORD error_code);
};

} // namespace tun

#endif // _WIN32
