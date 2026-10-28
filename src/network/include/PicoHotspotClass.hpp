#pragma once
#include "config/defines.h"
#include "pico/types.h"
#include "lwip/udp.h"
#include "lwip/netif.h"
struct tcp_pcb;

/**
    Provides the controller's direct Wi-Fi network and automatic phone addressing.

    Core 1 owns the access-point interface and a bounded DHCP lease table. DHCP
    packets are accepted only on that interface, so this server cannot answer
    requests from the home network. Existing HTTP/TCP listeners serve both local
    addresses; this class supplies neither Internet forwarding nor a second web
    server. Actual radio associations drive stable device dots; accepted application
    traffic briefly marks only the corresponding peer active. Status polling does not
    count as meaningful communication. Packet callbacks execute under SDK serialization.
*/
class PicoHotspot {
public:
    // Starts the password-protected hotspot and its interface-bound DHCP server.
    bool Start(void);

    // Removes DHCP callbacks and disables the hotspot before radio shutdown.
    void Stop(void);

    // Reports whether the hotspot and address server are running.
    bool IsReady(void) const;

    // Refreshes stable device slots from actual radio associations.
    void UpdateClients(uint32_t nowMs);

    // Copies bounded off, connected and active device states for the display core.
    void CopyClientStates(uint8_t* states, uint32_t nowMs) const;

private:
    struct Lease {
        uint8_t mac[WIFI_HOTSPOT_MAC_BYTES]{};
        absolute_time_t expires{};
        bool occupied = false;
    };

    struct Request {
        uint8_t type = 0;
        uint8_t requestedAddress[WIFI_HOTSPOT_IPV4_BYTES]{};
        uint8_t serverAddress[WIFI_HOTSPOT_IPV4_BYTES]{};
        bool hasRequestedAddress = false;
        bool hasServerAddress = false;
    };

    struct Device {
        uint8_t mac[WIFI_HOTSPOT_MAC_BYTES]{};
        uint32_t lastActivityMs = 0;
        bool connected = false;
        bool activitySeen = false;
    };

    Device devices_[WIFI_HOTSPOT_MAX_CLIENTS]{};
    uint32_t lastClientPollMs_ = 0;
    uint8_t clientLimit_ = 0;
    bool clientsPolled_ = false;
    udp_pcb* server_ = nullptr;
    netif* interface_ = nullptr;
    Lease leases_[WIFI_HOTSPOT_LEASE_COUNT]{};
    uint8_t packet_[WIFI_HOTSPOT_PACKET_BYTES]{};

    // Applies the configured client ceiling within the radio's supported capacity.
    bool ConfigureClientLimit(void);

    // Encodes an unsigned radio-control value in little-endian byte order.
    static void WriteDriverUnsigned32(uint8_t* destination, uint32_t value);

    // Decodes a little-endian unsigned value returned by the radio.
    static uint32_t ReadDriverUnsigned32(const uint8_t* source);

    // Marks meaningful traffic only for its associated hotspot device.
    static void ObserveCommunication(void* context, const tcp_pcb* client);

    // Validates DHCP option boundaries and extracts the addressing request.
    bool ParseOptions(size_t length, Request& request);

    // Finds a matching or reusable lease, respecting a requested pool address.
    int FindLease(const uint8_t* mac, const uint8_t* requestedAddress);

    // Builds and sends one DHCP offer, acknowledgement or rejection.
    void SendReply(uint8_t type, int lease);

    // Handles a validated DHCP datagram without retaining its packet storage.
    void HandlePacket(pbuf* packet);

    // Delivers AP-interface DHCP traffic to the owning hotspot instance.
    static void Receive(void* context, udp_pcb* pcb, pbuf* packet, const ip_addr_t* address, u16_t port);

};
