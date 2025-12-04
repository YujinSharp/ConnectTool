cmake_minimum_required(VERSION 3.10)
project(ConnectTool)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    add_compile_definitions(WIN32_LEAN_AND_MEAN)
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

# Find packages
find_package(Protobuf CONFIG REQUIRED)
find_package(gRPC CONFIG REQUIRED)
find_package(absl CONFIG REQUIRED)

# Find Asio
find_package(asio CONFIG REQUIRED)

# Find simdjson
find_package(simdjson CONFIG REQUIRED)

# Add nanoid_cpp
add_subdirectory(third_party/nanoid_cpp)

# TUN module sources (platform-specific)
set(TUN_SOURCES
    tun/tun_factory.cpp
)

if(UNIX AND NOT APPLE)
    list(APPEND TUN_SOURCES tun/tun_linux.cpp)
elseif(APPLE)
    list(APPEND TUN_SOURCES tun/tun_macos.cpp)
elseif(WIN32)
    list(APPEND TUN_SOURCES tun/tun_windows.cpp)
    include_directories(${CMAKE_SOURCE_DIR}/third_party/wintun/include)
endif()

# Include directories
include_directories(${CMAKE_SOURCE_DIR})
include_directories(${CMAKE_SOURCE_DIR}/steam)
include_directories(${CMAKE_SOURCE_DIR}/third_party/nanoid_cpp/inc)
include_directories(${CMAKE_SOURCE_DIR}/tun)
include_directories(${CMAKE_SOURCE_DIR}/third_party/steamsdk/public/steam)

# Generate Protobuf/gRPC sources
set(PROTO_SRC_DIR ${CMAKE_SOURCE_DIR}/protos)
set(PROTO_BINARY_DIR ${CMAKE_BINARY_DIR}/protos)
file(MAKE_DIRECTORY ${PROTO_BINARY_DIR})

# Helper function to generate protos
function(generate_grpc_proto PROTO_FILE)
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    
    # Generated files
    set(PROTO_SRCS "${PROTO_BINARY_DIR}/${PROTO_NAME}.pb.cc")
    set(PROTO_HDRS "${PROTO_BINARY_DIR}/${PROTO_NAME}.pb.h")
    set(GRPC_SRCS "${PROTO_BINARY_DIR}/${PROTO_NAME}.grpc.pb.cc")
    set(GRPC_HDRS "${PROTO_BINARY_DIR}/${PROTO_NAME}.grpc.pb.h")

    add_custom_command(
        OUTPUT ${PROTO_SRCS} ${PROTO_HDRS} ${GRPC_SRCS} ${GRPC_HDRS}
        COMMAND protobuf::protoc
        ARGS --grpc_out=${PROTO_BINARY_DIR}
             --cpp_out=${PROTO_BINARY_DIR}
             --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
             -I ${PROTO_SRC_DIR}
             ${PROTO_FILE}
        DEPENDS ${PROTO_FILE} protobuf::protoc gRPC::grpc_cpp_plugin
    )
    
    set(GENERATED_PROTO_SRCS ${PROTO_SRCS} ${GRPC_SRCS} PARENT_SCOPE)
    set(GENERATED_PROTO_HDRS ${PROTO_HDRS} ${GRPC_HDRS} PARENT_SCOPE)
endfunction()

generate_grpc_proto(${PROTO_SRC_DIR}/connect_tool.proto)

# Net module sources
set(NET_SOURCES
    vpn/node_identity.cpp
    vpn/ip_negotiator.cpp
    vpn/heartbeat_manager.cpp
    vpn/vpn_utils.cpp
    vpn/vpn_route_manager.cpp
)

# Config module sources
set(CONFIG_SOURCES
    config/config_manager.cpp
)

# Include net and config directories
include_directories(${CMAKE_SOURCE_DIR}/net)
include_directories(${CMAKE_SOURCE_DIR}/config)

# ConnectToolCore Executable
add_executable(ConnectToolCore
    server_main.cpp
    core/connect_tool_core.cpp
    steam/steam_message_handler.cpp 
    steam/steam_networking_manager.cpp 
    steam/steam_room_manager.cpp 
    steam/steam_utils.cpp 
    steam/steam_vpn_bridge.cpp
    steam/steam_vpn_utils.cpp
    ${NET_SOURCES}
    ${CONFIG_SOURCES}
    ${TUN_SOURCES}
    ${GENERATED_PROTO_SRCS}
    ${GENERATED_PROTO_HDRS}
)

target_include_directories(ConnectToolCore PRIVATE ${PROTO_BINARY_DIR} ${CMAKE_BINARY_DIR})

target_link_libraries(ConnectToolCore PRIVATE 
    nanoid 
    ${CMAKE_SOURCE_DIR}/third_party/steamsdk/redistributable_bin/win64/steam_api64.lib 
    ws2_32
    protobuf::libprotobuf 
    gRPC::grpc++
    absl::base
    absl::strings
    asio
    simdjson::simdjson
)

target_link_libraries(ConnectToolCore PRIVATE 
    nanoid 
    ${CMAKE_SOURCE_DIR}/third_party/steamsdk/redistributable_bin/win64/steam_api64.lib 
    ws2_32
    protobuf::libprotobuf 
    gRPC::grpc++
    absl::base
    absl::strings
)

if(WIN32)
    target_link_libraries(ConnectToolCore PRIVATE iphlpapi ole32 rpcrt4 winhttp)
endif()

# Copy steam_api64.dll to output directory
add_custom_command(TARGET ConnectToolCore POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_SOURCE_DIR}/third_party/steamsdk/redistributable_bin/win64/steam_api64.dll"
    $<TARGET_FILE_DIR:ConnectToolCore>)

# Copy wintun.dll to output directory (Windows only)
if(WIN32)
    add_custom_command(TARGET ConnectToolCore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/third_party/wintun/bin/amd64/wintun.dll"
        $<TARGET_FILE_DIR:ConnectToolCore>)
endif()

# Require Administrator privileges (Windows)
if(WIN32)
    # Embed manifest requiring admin privileges
    # 使用 /MANIFEST:NO 禁用自动清单生成，然后用 mt.exe 手动合并
    set_target_properties(ConnectToolCore PROPERTIES
        LINK_FLAGS "/MANIFEST:NO"
    )
    # 查找 mt.exe (Windows SDK Manifest Tool)
    find_program(MT_EXECUTABLE mt.exe
        HINTS
        "$ENV{WindowsSdkVerBinPath}/x64"
        "$ENV{WindowsSdkDir}/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.22000.0/x64"
        "C:/Program Files (x86)/Windows Kits/10/bin/10.0.19041.0/x64"
    )
    if(NOT MT_EXECUTABLE)
        message(WARNING "mt.exe not found, admin privilege manifest will not be embedded")
    else()
        message(STATUS "Found mt.exe: ${MT_EXECUTABLE}")
        # 使用 POST_BUILD 手动嵌入清单
        add_custom_command(TARGET ConnectToolCore POST_BUILD
            COMMAND "${MT_EXECUTABLE}" -manifest "${CMAKE_SOURCE_DIR}/app.manifest" "-outputresource:$<TARGET_FILE:ConnectToolCore>;#1"
            COMMENT "Embedding manifest with admin privileges"
        )
    endif()
endif()

# Linux/macOS: Add install target with setcap/setuid for privilege elevation
if(UNIX)
    # Install the executable
    install(TARGETS ConnectToolCore DESTINATION bin)
    
    if(APPLE)
        # macOS: Create a post-install script hint
        # Note: macOS apps typically use Authorization Services or require sudo
        message(STATUS "macOS: Run with 'sudo' or use Authorization Services for elevated privileges")
    else()
        # Linux: Use setcap to grant network admin capabilities (preferred over setuid)
        # This allows the program to manage network interfaces without full root
        install(CODE "
            execute_process(
                COMMAND setcap cap_net_admin,cap_net_raw+eip \${CMAKE_INSTALL_PREFIX}/bin/ConnectToolCore
                RESULT_VARIABLE SETCAP_RESULT
            )
            if(NOT SETCAP_RESULT EQUAL 0)
                message(WARNING \"Failed to set capabilities. Run: sudo setcap cap_net_admin,cap_net_raw+eip \${CMAKE_INSTALL_PREFIX}/bin/ConnectToolCore\")
            endif()
        ")
    endif()
endif()