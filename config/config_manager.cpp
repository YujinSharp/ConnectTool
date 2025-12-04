#include "config_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <simdjson.h>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#else
#include <curl/curl.h>
#endif

namespace {

#ifdef _WIN32
std::string httpGet(const std::string& url, std::string& error) {
    std::string result;
    
    // 解析 URL
    std::wstring wUrl(url.begin(), url.end());
    
    URL_COMPONENTS urlComp;
    ZeroMemory(&urlComp, sizeof(urlComp));
    urlComp.dwStructSize = sizeof(urlComp);
    
    wchar_t hostName[256] = {0};
    wchar_t urlPath[2048] = {0};
    urlComp.lpszHostName = hostName;
    urlComp.dwHostNameLength = sizeof(hostName) / sizeof(wchar_t);
    urlComp.lpszUrlPath = urlPath;
    urlComp.dwUrlPathLength = sizeof(urlPath) / sizeof(wchar_t);
    
    if (!WinHttpCrackUrl(wUrl.c_str(), 0, 0, &urlComp)) {
        error = "Failed to parse URL";
        return "";
    }
    
    // 打开会话
    HINTERNET hSession = WinHttpOpen(L"ConnectTool/1.0",
                                      WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                      WINHTTP_NO_PROXY_NAME,
                                      WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        error = "Failed to open WinHTTP session";
        return "";
    }
    
    // 设置超时时间为 3 秒
    WinHttpSetTimeouts(hSession, 3000, 3000, 3000, 3000);
    
    // 连接
    HINTERNET hConnect = WinHttpConnect(hSession, hostName, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        error = "Failed to connect";
        return "";
    }
    
    // 创建请求
    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", urlPath,
                                             NULL, WINHTTP_NO_REFERER,
                                             WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        error = "Failed to create request";
        return "";
    }
    
    // 发送请求
    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        error = "Failed to send request";
        return "";
    }
    
    // 接收响应
    if (!WinHttpReceiveResponse(hRequest, NULL)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        error = "Failed to receive response";
        return "";
    }
    
    // 读取数据
    DWORD bytesAvailable = 0;
    do {
        bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            break;
        }
        
        if (bytesAvailable == 0) break;
        
        std::vector<char> buffer(bytesAvailable + 1);
        DWORD bytesRead = 0;
        if (WinHttpReadData(hRequest, buffer.data(), bytesAvailable, &bytesRead)) {
            result.append(buffer.data(), bytesRead);
        }
    } while (bytesAvailable > 0);
    
    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    
    return result;
}
#else
// libcurl 回调
size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* userp) {
    userp->append((char*)contents, size * nmemb);
    return size * nmemb;
}

std::string httpGet(const std::string& url, std::string& error) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        error = "Failed to initialize CURL";
        return "";
    }
    
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 3L);
    
    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        error = curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        return "";
    }
    
    curl_easy_cleanup(curl);
    return response;
}
#endif

} // anonymous namespace

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

ConfigManager::ConfigManager() {
    // 设置备用配置 URL 列表（按优先级排序）
    configUrls_ = {
        // GitHub 原始地址
        "https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json",
        // gh-proxy.org 主站（Cloudflare 全球加速）
        "https://gh-proxy.org/https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json",
        // 香港节点（国内线路优化）
        "https://hk.gh-proxy.org/https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json",
        // Fastly CDN
        "https://cdn.gh-proxy.org/https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json",
        // EdgeOne 全球加速
        "https://edgeone.gh-proxy.org/https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json",
        // IPv6 支持
        "https://v6.gh-proxy.org/https://raw.githubusercontent.com/Ayndpa/ConnectTool/tun/config/default_config.json"
    };
}

bool ConfigManager::loadFromRemote() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::cout << "[ConfigManager] Loading configuration from remote..." << std::endl;
    
    for (size_t i = 0; i < configUrls_.size(); ++i) {
        const std::string& url = configUrls_[i];
        std::cout << "[ConfigManager] Trying URL " << (i + 1) << "/" << configUrls_.size() 
                  << ": " << url << std::endl;
        
        std::string error;
        std::string jsonContent = httpGet(url, error);
        
        if (!jsonContent.empty()) {
            if (parseJson(jsonContent)) {
                loaded_ = true;
                std::cout << "[ConfigManager] Configuration loaded successfully from: " << url << std::endl;
                return true;
            } else {
                std::cerr << "[ConfigManager] Failed to parse JSON from: " << url << std::endl;
            }
        } else {
            std::cerr << "[ConfigManager] Failed to fetch from " << url << ": " << error << std::endl;
        }
    }
    
    lastError_ = "Failed to load configuration from all URLs";
    std::cerr << "[ConfigManager] " << lastError_ << std::endl;
    return false;
}

bool ConfigManager::checkVersion() const {
    return compareVersion(APP_VERSION_STRING, config_.min_version);
}

bool ConfigManager::compareVersion(const std::string& appVersion, const std::string& minVersion) {
    // 解析版本号 (major.minor.patch)
    auto parseVersion = [](const std::string& ver, int& major, int& minor, int& patch) {
        major = minor = patch = 0;
        std::istringstream iss(ver);
        char dot;
        iss >> major >> dot >> minor >> dot >> patch;
    };
    
    int appMajor, appMinor, appPatch;
    int minMajor, minMinor, minPatch;
    
    parseVersion(appVersion, appMajor, appMinor, appPatch);
    parseVersion(minVersion, minMajor, minMinor, minPatch);
    
    // 比较版本
    if (appMajor != minMajor) return appMajor >= minMajor;
    if (appMinor != minMinor) return appMinor >= minMinor;
    return appPatch >= minPatch;
}

const AppConfig& ConfigManager::getConfig() const {
    return config_;
}

AppConfig& ConfigManager::getConfigMutable() {
    return config_;
}

bool ConfigManager::parseJson(const std::string& jsonContent) {
    try {
        simdjson::ondemand::parser parser;
        simdjson::padded_string padded(jsonContent);
        simdjson::ondemand::document doc = parser.iterate(padded);
        
        // 解析最低版本要求
        auto minVersion = doc["min_version"].get_string();
        if (!minVersion.error()) {
            config_.min_version = std::string(minVersion.value());
        }

        // 解析 app 部分
        auto appSection = doc["app"].get_object();
        if (!appSection.error()) {
            auto name = appSection["name"].get_string();
            if (!name.error()) config_.app.name = std::string(name.value());
            
            auto steamAppId = appSection["steam_app_id"].get_int64();
            if (!steamAppId.error()) config_.app.steam_app_id = static_cast<int>(steamAppId.value());
        }
        
        // 解析 vpn 部分
        auto vpnSection = doc["vpn"].get_object();
        if (!vpnSection.error()) {
            auto subnet = vpnSection["virtual_subnet"].get_string();
            if (!subnet.error()) config_.vpn.virtual_subnet = std::string(subnet.value());
            
            auto mask = vpnSection["subnet_mask"].get_string();
            if (!mask.error()) config_.vpn.subnet_mask = std::string(mask.value());
            
            auto mtu = vpnSection["default_mtu"].get_int64();
            if (!mtu.error()) config_.vpn.default_mtu = static_cast<int>(mtu.value());
            
            auto tunName = vpnSection["tun_device_name"].get_string();
            if (!tunName.error()) config_.vpn.tun_device_name = std::string(tunName.value());
        }
        
        // 解析 protocol 部分
        auto protocolSection = doc["protocol"].get_object();
        if (!protocolSection.error()) {
            auto salt = protocolSection["app_secret_salt"].get_string();
            if (!salt.error()) config_.protocol.app_secret_salt = std::string(salt.value());
            
            auto probeTimeout = protocolSection["probe_timeout_ms"].get_int64();
            if (!probeTimeout.error()) config_.protocol.probe_timeout_ms = probeTimeout.value();
            
            auto heartbeatInterval = protocolSection["heartbeat_interval_ms"].get_int64();
            if (!heartbeatInterval.error()) config_.protocol.heartbeat_interval_ms = heartbeatInterval.value();
            
            auto leaseTime = protocolSection["lease_time_ms"].get_int64();
            if (!leaseTime.error()) config_.protocol.lease_time_ms = leaseTime.value();
            
            auto leaseExpiry = protocolSection["lease_expiry_ms"].get_int64();
            if (!leaseExpiry.error()) config_.protocol.lease_expiry_ms = leaseExpiry.value();
            
            auto heartbeatExpiry = protocolSection["heartbeat_expiry_ms"].get_int64();
            if (!heartbeatExpiry.error()) config_.protocol.heartbeat_expiry_ms = heartbeatExpiry.value();
            
            auto nodeIdSize = protocolSection["node_id_size"].get_int64();
            if (!nodeIdSize.error()) config_.protocol.node_id_size = static_cast<size_t>(nodeIdSize.value());
        }
        
        // 解析 networking 部分
        auto networkingSection = doc["networking"].get_object();
        if (!networkingSection.error()) {
            auto sendRate = networkingSection["send_rate_mb"].get_int64();
            if (!sendRate.error()) config_.networking.send_rate_mb = static_cast<int>(sendRate.value());
            
            auto sendBufferSize = networkingSection["send_buffer_size_mb"].get_int64();
            if (!sendBufferSize.error()) config_.networking.send_buffer_size_mb = static_cast<int>(sendBufferSize.value());
            
            auto nagleTime = networkingSection["nagle_time"].get_int64();
            if (!nagleTime.error()) config_.networking.nagle_time = static_cast<int>(nagleTime.value());
            
            auto callbackInterval = networkingSection["steam_callback_interval_ms"].get_int64();
            if (!callbackInterval.error()) config_.networking.steam_callback_interval_ms = static_cast<int>(callbackInterval.value());
        }
        
        // 解析 server 部分
        auto serverSection = doc["server"].get_object();
        if (!serverSection.error()) {
            auto pathWindows = serverSection["unix_socket_path_windows"].get_string();
            if (!pathWindows.error()) config_.server.unix_socket_path_windows = std::string(pathWindows.value());
            
            auto pathUnix = serverSection["unix_socket_path_unix"].get_string();
            if (!pathUnix.error()) config_.server.unix_socket_path_unix = std::string(pathUnix.value());
        }
        
        return true;
    } catch (const simdjson::simdjson_error& e) {
        lastError_ = std::string("JSON parse error: ") + e.what();
        std::cerr << "[ConfigManager] " << lastError_ << std::endl;
        return false;
    } catch (const std::exception& e) {
        lastError_ = std::string("JSON parse error: ") + e.what();
        std::cerr << "[ConfigManager] " << lastError_ << std::endl;
        return false;
    }
}
