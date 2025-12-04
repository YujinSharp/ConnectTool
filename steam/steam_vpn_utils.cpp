#include "steam_vpn_utils.h"
#include "../vpn/vpn_protocol.h"
#include <iostream>
#include <isteamnetworkingutils.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace SteamVpnUtils {

    int querySteamMtuDataSize() {
        ISteamNetworkingUtils* pUtils = SteamNetworkingUtils();
        if (!pUtils) {
            std::cerr << "[MTU] SteamNetworkingUtils not available, using default MTU" << std::endl;
            return RECOMMENDED_MTU;
        }
        
        int32 mtuDataSize = 0;
        size_t cbResult = sizeof(mtuDataSize);
        ESteamNetworkingConfigDataType dataType;
        
        ESteamNetworkingGetConfigValueResult result = pUtils->GetConfigValue(
            k_ESteamNetworkingConfig_MTU_DataSize,
            k_ESteamNetworkingConfig_Global,
            0,
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

} // namespace SteamVpnUtils
