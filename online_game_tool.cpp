#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <algorithm>
#include <cstring>
#include <boost/asio.hpp>
#include <memory>
#include <fstream>
#include <filesystem>
#include "steam/steam_networking_manager.h"
#include "steam/steam_room_manager.h"
#include "steam/steam_utils.h"
#include "steam/steam_vpn_bridge.h"

#ifdef _WIN32
#include <windows.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#else
#include <sys/file.h>
#include <unistd.h>
#include <signal.h>
#endif

using boost::asio::ip::tcp;

// New variables for multiple connections
std::vector<HSteamNetConnection> connections;
std::mutex connectionsMutex; // Add mutex for connections

#ifdef _WIN32
// Windows implementation using mutex and shared memory
HANDLE g_hMutex = nullptr;
HANDLE g_hMapFile = nullptr;
HWND* g_pSharedHwnd = nullptr;

bool checkSingleInstance()
{
    g_hMutex = CreateMutexW(nullptr, FALSE, L"Global\\OnlineGameTool_SingleInstance_Mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        // Another instance exists, try to find and activate it
        g_hMapFile = OpenFileMappingW(FILE_MAP_READ, FALSE, L"Global\\OnlineGameTool_HWND_Share");
        if (g_hMapFile != nullptr)
        {
            HWND* pHwnd = (HWND*)MapViewOfFile(g_hMapFile, FILE_MAP_READ, 0, 0, sizeof(HWND));
            if (pHwnd != nullptr && *pHwnd != nullptr && IsWindow(*pHwnd))
            {
                // Restore and bring to front
                if (IsIconic(*pHwnd))
                {
                    ShowWindow(*pHwnd, SW_RESTORE);
                }
                SetForegroundWindow(*pHwnd);
                UnmapViewOfFile(pHwnd);
            }
            CloseHandle(g_hMapFile);
        }
        if (g_hMutex)
        {
            CloseHandle(g_hMutex);
        }
        return false;
    }

    // Create shared memory for HWND
    g_hMapFile = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(HWND), L"Global\\OnlineGameTool_HWND_Share");
    if (g_hMapFile != nullptr)
    {
        g_pSharedHwnd = (HWND*)MapViewOfFile(g_hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(HWND));
    }
    return true;
}

void storeWindowHandle(GLFWwindow* window)
{
    if (g_pSharedHwnd != nullptr)
    {
        *g_pSharedHwnd = glfwGetWin32Window(window);
    }
}

void cleanupSingleInstance()
{
    if (g_pSharedHwnd != nullptr)
    {
        UnmapViewOfFile(g_pSharedHwnd);
        g_pSharedHwnd = nullptr;
    }
    if (g_hMapFile != nullptr)
    {
        CloseHandle(g_hMapFile);
        g_hMapFile = nullptr;
    }
    if (g_hMutex != nullptr)
    {
        CloseHandle(g_hMutex);
        g_hMutex = nullptr;
    }
}

#else
// Unix/Linux/macOS implementation using file lock and signal
int g_lockfd = -1;
std::string g_lockFilePath;

void signalHandler(int signum)
{
    // Signal received to bring window to front
    std::cout << "Received signal to activate window" << std::endl;
}

bool checkSingleInstance()
{
    std::string tempDir;
#ifdef __APPLE__
    const char* tmpdir = getenv("TMPDIR");
    tempDir = tmpdir ? tmpdir : "/tmp";
#else
    tempDir = "/tmp";
#endif
    
    g_lockFilePath = tempDir + "/OnlineGameTool.lock";
    
    g_lockfd = open(g_lockFilePath.c_str(), O_CREAT | O_RDWR, 0666);
    if (g_lockfd < 0)
    {
        std::cerr << "Failed to open lock file" << std::endl;
        return false;
    }

    // Try to acquire exclusive lock
    if (flock(g_lockfd, LOCK_EX | LOCK_NB) != 0)
    {
        // Lock failed, another instance is running
        // Read PID and send signal
        char pidBuf[32];
        ssize_t bytesRead = read(g_lockfd, pidBuf, sizeof(pidBuf) - 1);
        if (bytesRead > 0)
        {
            pidBuf[bytesRead] = '\0';
            pid_t existingPid = atoi(pidBuf);
            if (existingPid > 0)
            {
                // Send SIGUSR1 to existing instance
                kill(existingPid, SIGUSR1);
            }
        }
        close(g_lockfd);
        g_lockfd = -1;
        return false;
    }

    // Write our PID to the lock file
    ftruncate(g_lockfd, 0);
    pid_t myPid = getpid();
    std::string pidStr = std::to_string(myPid);
    write(g_lockfd, pidStr.c_str(), pidStr.length());

    // Set up signal handler
    signal(SIGUSR1, signalHandler);

    return true;
}

void storeWindowHandle(GLFWwindow* window)
{
    // GLFW doesn't provide a standard way to bring window to front on Unix
    // but we can request attention
    glfwRequestWindowAttention(window);
}

void cleanupSingleInstance()
{
    if (g_lockfd >= 0)
    {
        flock(g_lockfd, LOCK_UN);
        close(g_lockfd);
        g_lockfd = -1;
        unlink(g_lockFilePath.c_str());
    }
}
#endif

int main()
{
    // Check for single instance
    if (!checkSingleInstance())
    {
        std::cout << "另一个实例已在运行，正在激活该窗口..." << std::endl;
        return 0;
    }

    // Initialize Steam API first
    if (!SteamAPI_Init())
    {
        std::cerr << "Failed to initialize Steam API" << std::endl;
        return 1;
    }

    boost::asio::io_context io_context;
    auto work_guard = boost::asio::make_work_guard(io_context);
    std::thread io_thread([&io_context]()
                          { io_context.run(); });

    // Initialize Steam Networking Manager
    SteamNetworkingManager steamManager;
    if (!steamManager.initialize())
    {
        std::cerr << "Failed to initialize Steam Networking Manager" << std::endl;
        SteamAPI_Shutdown();
        return 1;
    }

    // Initialize Steam Room Manager
    SteamRoomManager roomManager(&steamManager);

    // Initialize Steam VPN Bridge
    SteamVpnBridge vpnBridge(&steamManager);
    steamManager.setVpnBridge(&vpnBridge);

    // Initialize GLFW
    if (!glfwInit())
    {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        steamManager.shutdown();
        return -1;
    }

    // Create window
    GLFWwindow *window = glfwCreateWindow(1280, 720, "在线游戏工具 - 1.0.0", nullptr, nullptr);
    if (!window)
    {
        std::cerr << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        cleanupSingleInstance();
        SteamAPI_Shutdown();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Store window handle for single instance activation
    storeWindowHandle(window);

    // Initialize ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    (void)io;
    // Load Chinese font
    io.Fonts->AddFontFromFileTTF("font.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    ImGui::StyleColorsDark();

    // Initialize ImGui backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    // Start message handler
    steamManager.startMessageHandler();

    // Steam Networking variables
    char filterBuffer[256] = "";
    
    // VPN variables
    bool vpnEnabled = false;

    // Lambda to get connection info for a member
    auto getMemberConnectionInfo = [&](const CSteamID &memberID) -> std::pair<int, std::string>
    {
        int ping = 0;
        std::string relayInfo = "-";

        // Find connection for this member
        HSteamNetConnection conn = steamManager.getConnectionForPeer(memberID);
        if (conn != k_HSteamNetConnection_Invalid)
        {
            ping = steamManager.getConnectionPing(conn);
            relayInfo = steamManager.getConnectionRelayInfo(conn);
        }

        return {ping, relayInfo};
    };

    // Lambda to render invite friends UI
    auto renderInviteFriends = [&]()
    {
        ImGui::InputText("过滤朋友", filterBuffer, IM_ARRAYSIZE(filterBuffer));
        ImGui::Text("朋友:");
        for (const auto &friendPair : SteamUtils::getFriendsList())
        {
            std::string nameStr = friendPair.second;
            std::string filterStr(filterBuffer);
            // Convert to lowercase for case-insensitive search
            std::transform(nameStr.begin(), nameStr.end(), nameStr.begin(), ::tolower);
            std::transform(filterStr.begin(), filterStr.end(), filterStr.begin(), ::tolower);
            if (filterStr.empty() || nameStr.find(filterStr) != std::string::npos)
            {
                ImGui::PushID(friendPair.first.ConvertToUint64());
                if (ImGui::Button(("邀请 " + friendPair.second).c_str()))
                {
                    // Send invite via Steam to lobby
                    if (SteamMatchmaking())
                    {
                        SteamMatchmaking()->InviteUserToLobby(roomManager.getCurrentLobby(), friendPair.first);
                        std::cout << "Sent lobby invite to " << friendPair.second << std::endl;
                    }
                    else
                    {
                        std::cerr << "SteamMatchmaking() is null! Cannot send invite." << std::endl;
                    }
                }
                ImGui::PopID();
            }
        }
    };

    // Frame rate limiting
    const double targetFrameTimeForeground = 1.0 / 60.0; // 60 FPS when focused
    const double targetFrameTimeBackground = 1.0; // 1 FPS when in background
    double lastFrameTime = glfwGetTime();

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        // Frame rate control based on window focus
        bool isFocused = glfwGetWindowAttrib(window, GLFW_FOCUSED);
        double targetFrameTime = isFocused ? targetFrameTimeForeground : targetFrameTimeBackground;
        
        double currentTime = glfwGetTime();
        double deltaTime = currentTime - lastFrameTime;
        if (deltaTime < targetFrameTime)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(targetFrameTime - deltaTime));
        }
        lastFrameTime = glfwGetTime();

        // Poll events
        glfwPollEvents();

        SteamAPI_RunCallbacks();

        // Update Steam networking info
        steamManager.update();

        // Start ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // Create a window for online game tool
        ImGui::Begin("在线游戏工具");
        ImGui::Separator();

        if (!steamManager.isConnected())
        {
            if (ImGui::Button("创建房间"))
            {
                roomManager.createLobby();
            }
            ImGui::SameLine();
            if (ImGui::Button("加入好友房间"))
            {
                // 通过Steam好友列表邀请来加入
                ImGui::OpenPopup("选择好友房间");
            }
        }
        else
        {
            ImGui::Text("已连接到房间。邀请朋友!");
            ImGui::Separator();
            
            // VPN Control Section
            ImGui::Text("Steam VPN:");
            if (!vpnEnabled)
            {
                if (ImGui::Button("启动虚拟局域网"))
                {
                    if (vpnBridge.start("", "10.0.0.0", "255.255.255.0"))
                    {
                        vpnEnabled = true;
                        std::cout << "VPN started successfully" << std::endl;
                    }
                    else
                    {
                        std::cerr << "Failed to start VPN" << std::endl;
                    }
                }
            }
            else
            {
                ImGui::Text("虚拟局域网已启动");
                ImGui::Text("本机IP: %s", vpnBridge.getLocalIP().c_str());
                ImGui::Text("设备: %s", vpnBridge.getTunDeviceName().c_str());
                
                auto stats = vpnBridge.getStatistics();
                ImGui::Text("发送: %llu 包 / %llu 字节", 
                           (unsigned long long)stats.packetsSent, 
                           (unsigned long long)stats.bytesSent);
                ImGui::Text("接收: %llu 包 / %llu 字节", 
                           (unsigned long long)stats.packetsReceived, 
                           (unsigned long long)stats.bytesReceived);
                ImGui::Text("丢弃: %llu 包", (unsigned long long)stats.packetsDropped);
                
                if (ImGui::Button("停止虚拟局域网"))
                {
                    vpnBridge.stop();
                    vpnEnabled = false;
                    std::cout << "VPN stopped" << std::endl;
                }
            }
            ImGui::Separator();
            
            if (ImGui::Button("断开连接"))
            {
                // Stop VPN if running
                if (vpnEnabled)
                {
                    vpnBridge.stop();
                    vpnEnabled = false;
                }
                
                roomManager.leaveLobby();
                steamManager.disconnect();
            }
            ImGui::Separator();
            renderInviteFriends();
        }

        ImGui::End();

        // Room status window - only show when connected
        if (steamManager.isConnected() && roomManager.getCurrentLobby().IsValid())
        {
            ImGui::Begin("房间状态");
            ImGui::Text("用户列表:");
            if (ImGui::BeginTable("UserTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("名称");
                ImGui::TableSetupColumn("延迟 (ms)");
                ImGui::TableSetupColumn("连接类型");
                ImGui::TableHeadersRow();
                {
                    std::vector<CSteamID> members = roomManager.getLobbyMembers();
                    CSteamID mySteamID = SteamUser()->GetSteamID();
                    for (const auto &memberID : members)
                    {
                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        const char *name = SteamFriends()->GetFriendPersonaName(memberID);
                        ImGui::Text("%s", name);
                        ImGui::TableNextColumn();

                        if (memberID == mySteamID)
                        {
                            ImGui::Text("-");
                            ImGui::TableNextColumn();
                            ImGui::Text("-");
                        }
                        else
                        {
                            auto [ping, relayInfo] = getMemberConnectionInfo(memberID);

                            if (relayInfo != "-")
                            {
                                ImGui::Text("%d", ping);
                            }
                            else
                            {
                                ImGui::Text("连接中...");
                            }
                            ImGui::TableNextColumn();
                            ImGui::Text("%s", relayInfo.c_str());
                        }
                    }
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        // VPN Routing Table window - only show when VPN is enabled
        if (vpnEnabled)
        {
            ImGui::Begin("虚拟局域网路由表");
            ImGui::Text("IP地址分配:");
            if (ImGui::BeginTable("VpnRouteTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("用户名");
                ImGui::TableSetupColumn("IP地址");
                ImGui::TableSetupColumn("状态");
                ImGui::TableHeadersRow();
                
                auto routingTable = vpnBridge.getRoutingTable();
                for (const auto& entry : routingTable)
                {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", entry.second.name.c_str());
                    ImGui::TableNextColumn();
                    
                    // Convert IP to string
                    uint32_t ip = entry.first;
                    char ipStr[16];
                    snprintf(ipStr, sizeof(ipStr), "%d.%d.%d.%d",
                            (ip >> 24) & 0xFF,
                            (ip >> 16) & 0xFF,
                            (ip >> 8) & 0xFF,
                            ip & 0xFF);
                    ImGui::Text("%s", ipStr);
                    
                    ImGui::TableNextColumn();
                    ImGui::Text("%s", entry.second.isLocal ? "本机" : "在线");
                }
                ImGui::EndTable();
            }
            ImGui::End();
        }

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // Swap buffers
        glfwSwapBuffers(window);
    }

    // Stop message handler
    steamManager.stopMessageHandler();

    // Cleanup
    // Stop io_context and join thread
    work_guard.reset();
    io_context.stop();
    if (io_thread.joinable())
    {
        io_thread.join();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    steamManager.shutdown();

    // Cleanup single instance resources
    cleanupSingleInstance();

    return 0;
}