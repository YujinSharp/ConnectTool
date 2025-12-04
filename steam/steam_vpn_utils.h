#ifndef STEAM_VPN_UTILS_H
#define STEAM_VPN_UTILS_H

#include <string>
#include <cstdint>
#include <steam_api.h>
#include "../vpn/vpn_utils.h"

/**
 * @brief Steam VPN Utility functions
 */
namespace SteamVpnUtils {

    /**
     * @brief Query Steam Networking MTU data size limit
     * @return Max data size for sending, or default if failed
     */
    int querySteamMtuDataSize();

} // namespace SteamVpnUtils

#endif // STEAM_VPN_UTILS_H
