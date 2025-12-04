#ifndef VPN_UTILS_H
#define VPN_UTILS_H

#include <string>
#include <cstdint>
#include "vpn_protocol.h"

/**
 * @brief Generic VPN Utility functions
 */
namespace VpnUtils {

    /**
     * @brief Calculate suitable TUN MTU
     * @param steamMtuDataSize Steam allowed max unfragmented data size
     * @return MTU value to set for TUN device
     */
    int calculateTunMtu(int steamMtuDataSize);

    /**
     * @brief Convert IP address to string
     */
    std::string ipToString(uint32_t ip);

    /**
     * @brief Convert string to IP address
     */
    uint32_t stringToIp(const std::string& ipStr);

    /**
     * @brief Extract destination IP from packet
     */
    uint32_t extractDestIP(const uint8_t* packet, size_t length);

    /**
     * @brief Extract source IP from packet
     */
    uint32_t extractSourceIP(const uint8_t* packet, size_t length);

    /**
     * @brief Check if IP is a broadcast address
     */
    bool isBroadcastAddress(uint32_t ip, uint32_t baseIP, uint32_t subnetMask);

} // namespace VpnUtils

#endif // VPN_UTILS_H
