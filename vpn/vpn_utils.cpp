#include "vpn_utils.h"
#include <iostream>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace VpnUtils {

    int calculateTunMtu(int steamMtuDataSize) {
        int tunMtu = steamMtuDataSize - static_cast<int>(VPN_MESSAGE_OVERHEAD) - 15;
        if (tunMtu < 576) tunMtu = 576;
        else if (tunMtu > 1500) tunMtu = 1500;
        
        std::cout << "[MTU] Calculated TUN MTU: " << tunMtu 
                  << " (Steam limit: " << steamMtuDataSize 
                  << ", overhead: " << VPN_MESSAGE_OVERHEAD << ")" << std::endl;
        return tunMtu;
    }

    std::string ipToString(uint32_t ip) {
        char buffer[INET_ADDRSTRLEN];
        struct in_addr addr;
        addr.s_addr = htonl(ip);
        inet_ntop(AF_INET, &addr, buffer, INET_ADDRSTRLEN);
        return std::string(buffer);
    }

    uint32_t stringToIp(const std::string& ipStr) {
        struct in_addr addr;
        if (inet_pton(AF_INET, ipStr.c_str(), &addr) == 1) {
            return ntohl(addr.s_addr);
        }
        return 0;
    }

    uint32_t extractDestIP(const uint8_t* packet, size_t length) {
        if (length < 20) return 0;
        uint8_t version = (packet[0] >> 4) & 0x0F;
        if (version != 4) return 0;

        uint32_t destIP;
        memcpy(&destIP, packet + 16, 4);
        return ntohl(destIP);
    }

    uint32_t extractSourceIP(const uint8_t* packet, size_t length) {
        if (length < 20) return 0;
        uint8_t version = (packet[0] >> 4) & 0x0F;
        if (version != 4) return 0;

        uint32_t srcIP;
        memcpy(&srcIP, packet + 12, 4);
        return ntohl(srcIP);
    }

    bool isBroadcastAddress(uint32_t ip, uint32_t baseIP, uint32_t subnetMask) {
        if (ip == 0xFFFFFFFF) return true;
        
        uint32_t subnetBroadcast = (baseIP & subnetMask) | (~subnetMask);
        if (ip == subnetBroadcast) return true;
        
        uint8_t firstOctet = (ip >> 24) & 0xFF;
        if (firstOctet >= 224 && firstOctet <= 239) return true;
        
        return false;
    }

} // namespace VpnUtils
