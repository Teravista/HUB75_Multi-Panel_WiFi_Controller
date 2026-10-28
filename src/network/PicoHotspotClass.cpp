#include "network/include/PicoHotspotClass.hpp"
#include "networkConfig.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/etharp.h"
#include "lwip/tcp.h"
#include "cyw43_ll.h"
#include "network/include/clientActivity.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

// CYW43439 on Pico 2 W is documented for four simultaneous SoftAP clients.
#define HOTSPOT_SUPPORTED_CLIENTS 4
#define HOTSPOT_MAX_ASSOC_NAME_BYTES 9
#define HOTSPOT_MAX_ASSOC_SETTING_BYTES (HOTSPOT_MAX_ASSOC_NAME_BYTES + WIFI_HOTSPOT_IPV4_BYTES)
// CYW43 encodes the standard WLC_GET_ASSOCLIST command in bits above the set/get bit.
#define HOTSPOT_ASSOC_LIST_IOCTL (159u << 1u)
#define HOTSPOT_ASSOC_REPLY_BYTES (WIFI_HOTSPOT_IPV4_BYTES + WIFI_HOTSPOT_MAX_CLIENTS * WIFI_HOTSPOT_MAC_BYTES)

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_REQUEST_OPCODE 1
#define DHCP_REPLY_OPCODE 2
#define DHCP_ETHERNET_TYPE 1
#define DHCP_OPCODE_OFFSET 0
#define DHCP_HARDWARE_TYPE_OFFSET 1
#define DHCP_HARDWARE_LENGTH_OFFSET 2
#define DHCP_HOPS_OFFSET 3
#define DHCP_SECONDS_OFFSET 8
#define DHCP_FLAGS_OFFSET 10
#define DHCP_CLIENT_ADDRESS_OFFSET 12
#define DHCP_YOUR_ADDRESS_OFFSET 16
#define DHCP_RELAY_ADDRESS_OFFSET 24
#define DHCP_MAC_OFFSET 28
#define DHCP_SERVER_NAME_OFFSET 44
#define DHCP_COOKIE_OFFSET 236
#define DHCP_OPTIONS_OFFSET 240
#define DHCP_REPLY_BYTES 300
#define DHCP_COOKIE_0 99
#define DHCP_COOKIE_1 130
#define DHCP_COOKIE_2 83
#define DHCP_COOKIE_3 99
#define DHCP_OPTION_PADDING 0
#define DHCP_OPTION_SUBNET 1
#define DHCP_OPTION_REQUESTED_IP 50
#define DHCP_OPTION_LEASE 51
#define DHCP_OPTION_TYPE 53
#define DHCP_OPTION_SERVER 54
#define DHCP_OPTION_END 255
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_DECLINE 4
#define DHCP_ACK 5
#define DHCP_NAK 6
#define DHCP_RELEASE 7
#define DHCP_INFORM 8
#define DHCP_NO_LEASE -1
#define DHCP_TYPE_BYTES 1
#define DHCP_BROADCAST_OCTET 255
#define DHCP_POOL_FIRST_HOST 2
#define DHCP_HOST_BYTE 3
#define DHCP_LEASE_SECONDS 3600U
#define DHCP_LEASE_MILLISECONDS (DHCP_LEASE_SECONDS * 1000U)
#define DHCP_OFFER_MILLISECONDS 60000U
#define DHCP_BYTE_SHIFT_24 24
#define DHCP_BYTE_SHIFT_16 16
#define DHCP_BYTE_SHIFT_8 8

/**
    Starts the password-protected hotspot and its interface-bound DHCP server.

    Enables the second CYW43 interface without changing the station interface.
    The UDP socket is bound to the AP netif as well as the DHCP server port;
    home-network clients must never receive this server's offers. Failure releases
    the partial socket and AP interface so a later network pass can retry safely.

    @return true - The AP and its DHCP server are ready, including an existing session.
    @return false - The AP interface or UDP socket could not be started.
*/
bool
PicoHotspot::Start (
    void
    )
{
    ip4_addr_t address;
    ip4_addr_t mask;
    bool ready;

    if (server_) {
        return true;
    }

    cyw43_arch_enable_ap_mode(WIFI_AP_SSID, WIFI_AP_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    cyw43_arch_lwip_begin();
    interface_ = &cyw43_state.netif[CYW43_ITF_AP];
    ready = ConfigureClientLimit() && netif_is_up(interface_) && ip4addr_aton(WIFI_HOTSPOT_ADDRESS, &address) &&
        ip4addr_aton(WIFI_HOTSPOT_NETMASK, &mask);
    if (ready) {
        netif_set_addr(interface_, &address, &mask, &address);
        server_ = udp_new_ip_type(IPADDR_TYPE_V4);
        ready = server_ != nullptr;
    }
    if (ready) {
        ready = udp_bind(server_, IP_ADDR_ANY, DHCP_SERVER_PORT) == ERR_OK;
    }
    if (ready) {
        udp_bind_netif(server_, interface_);
        ip_set_option(server_, SOF_BROADCAST);
        udp_recv(server_, Receive, this);
        ClientActivity::SetObserver(ObserveCommunication, this);
    }
    cyw43_arch_lwip_end();

    if (!ready) {
        Stop();
        return false;
    }
    printf("[hotspot] ready ssid=%s http://%s/ clients=%u slots=%u\n", WIFI_AP_SSID,
        WIFI_HOTSPOT_ADDRESS, unsigned(clientLimit_), unsigned(WIFI_HOTSPOT_MAX_CLIENTS));
    return true;
} // Start

/**
    Removes DHCP callbacks and disables the hotspot before radio shutdown.
*/
void
PicoHotspot::Stop (
    void
    )
{
    int index;

    cyw43_arch_lwip_begin();
    ClientActivity::SetObserver(nullptr, nullptr);
    if (server_) {
        udp_recv(server_, nullptr, nullptr);
        udp_remove(server_);
        server_ = nullptr;
    }
    interface_ = nullptr;
    clientLimit_ = 0;
    clientsPolled_ = false;
    for (index = 0; index < WIFI_HOTSPOT_MAX_CLIENTS; ++index) {
        devices_[index] = {};
    }
    for (index = 0; index < WIFI_HOTSPOT_LEASE_COUNT; ++index) {
        leases_[index] = {};
    }
    cyw43_arch_lwip_end();
    cyw43_arch_disable_ap_mode();
} // Stop

/**
    Reports whether the hotspot and address server are running.

    @return true - The network worker owns a running AP/DHCP session.
    @return false - The hotspot has not started or was stopped.
*/
bool
PicoHotspot::IsReady (
    void
    ) const
{
    return server_ != nullptr;
} // IsReady

/**
    Validates DHCP option boundaries and extracts the addressing request.

    Walks the copied packet once, accepting padding and skipping unknown bounded
    options. Duplicate addressing fields, truncated options and missing end/type
    markers are rejected before leases or replies can be changed.

    @param length Number of copied bytes in the DHCP datagram.
    @param request Destination for the message type and requested/server addresses.

    @return true - The option stream ended with one valid DHCP message type.
    @return false - An option is malformed, duplicated or incomplete.
*/
bool
PicoHotspot::ParseOptions (
    size_t length,
    Request& request
    )
{
    size_t offset;
    uint8_t option;
    uint8_t bytes;

    offset = DHCP_OPTIONS_OFFSET;
    while (offset < length) {
        option = packet_[offset++];
        if (option == DHCP_OPTION_END) {
            return request.type != 0;
        }
        if (option == DHCP_OPTION_PADDING) {
            continue;
        }
        if (offset >= length) {
            return false;
        }
        bytes = packet_[offset++];
        if (bytes > length - offset) {
            return false;
        }

        if (option == DHCP_OPTION_TYPE) {
            if (bytes != DHCP_TYPE_BYTES || request.type) {
                return false;
            }
            request.type = packet_[offset];
        } else if (option == DHCP_OPTION_REQUESTED_IP) {
            if (bytes != WIFI_HOTSPOT_IPV4_BYTES || request.hasRequestedAddress) {
                return false;
            }
            request.hasRequestedAddress = true;
            std::memcpy(request.requestedAddress, packet_ + offset, bytes);
        } else if (option == DHCP_OPTION_SERVER) {
            if (bytes != WIFI_HOTSPOT_IPV4_BYTES || request.hasServerAddress) {
                return false;
            }
            request.hasServerAddress = true;
            std::memcpy(request.serverAddress, packet_ + offset, bytes);
        }
        offset += bytes;
    }
    return false;
} // ParseOptions

/**
    Finds a matching or reusable lease, respecting a requested pool address.

    Existing unexpired MAC leases take precedence. A requested address must belong
    to this fixed /24 pool and must not be leased to another client. Discoveries
    without a requested address reuse expired or unused entries. Expiration uses
    SDK monotonic deadlines and does not require a wall clock.

    @param mac Six-byte Ethernet client address.
    @param requestedAddress Optional network-order address requested by the client.

    @return index - Matching or available lease-table entry.
    @return DHCP_NO_LEASE - The requested address is invalid or no entry is free.
*/
int
PicoHotspot::FindLease (
    const uint8_t* mac,
    const uint8_t* requestedAddress
    )
{
    int available;
    int requested;
    int index;
    bool active;
    const uint8_t* serverAddress = nullptr;

    serverAddress = reinterpret_cast<const uint8_t*>(netif_ip4_addr(interface_));
    requested = DHCP_NO_LEASE;
    if (requestedAddress) {
        if (std::memcmp(requestedAddress, serverAddress, DHCP_HOST_BYTE) != 0 ||
            requestedAddress[DHCP_HOST_BYTE] < DHCP_POOL_FIRST_HOST ||
            requestedAddress[DHCP_HOST_BYTE] >= DHCP_POOL_FIRST_HOST + WIFI_HOTSPOT_LEASE_COUNT) {
            return DHCP_NO_LEASE;
        }
        requested = requestedAddress[DHCP_HOST_BYTE] - DHCP_POOL_FIRST_HOST;
    }

    available = DHCP_NO_LEASE;
    for (index = 0; index < WIFI_HOTSPOT_LEASE_COUNT; ++index) {
        active = leases_[index].occupied &&
            absolute_time_diff_us(get_absolute_time(), leases_[index].expires) > 0;
        if (active && std::memcmp(leases_[index].mac, mac, WIFI_HOTSPOT_MAC_BYTES) == 0) {
            if (requestedAddress && requested != index) {
                return DHCP_NO_LEASE;
            }
            return index;
        }
        if (!active && (requested == DHCP_NO_LEASE || requested == index)) {
            available = index;
        }
    }
    return available;
} // FindLease

/**
    Builds and sends one DHCP offer, acknowledgement or rejection.

    Preserves the transaction ID and client MAC from the validated request, then
    replaces its options with the AP address, mask and lease. Initial offers and
    rejections use broadcast; renewals and INFORM replies use the client's
    configured address. No DNS server or Internet gateway is advertised; the
    phone accesses only the local subnet.

    @param type DHCP reply message type.
    @param lease Assigned lease-table entry, or DHCP_NO_LEASE for NAK/INFORM.
*/
void
PicoHotspot::SendReply (
    uint8_t type,
    int lease
    )
{
    size_t offset;
    pbuf* response = nullptr;
    ip_addr_t destination;

    IP_ADDR4(&destination, DHCP_BROADCAST_OCTET, DHCP_BROADCAST_OCTET,
        DHCP_BROADCAST_OCTET, DHCP_BROADCAST_OCTET);
    if (type != DHCP_NAK) {
        std::memcpy(&destination, packet_ + DHCP_CLIENT_ADDRESS_OFFSET, WIFI_HOTSPOT_IPV4_BYTES);
        if (ip4_addr_isany_val(destination)) {
            IP_ADDR4(&destination, DHCP_BROADCAST_OCTET, DHCP_BROADCAST_OCTET,
                DHCP_BROADCAST_OCTET, DHCP_BROADCAST_OCTET);
        }
    } else {
        std::memset(packet_ + DHCP_CLIENT_ADDRESS_OFFSET, 0, WIFI_HOTSPOT_IPV4_BYTES);
    }
    packet_[DHCP_OPCODE_OFFSET] = DHCP_REPLY_OPCODE;
    packet_[DHCP_HOPS_OFFSET] = 0;
    packet_[DHCP_SECONDS_OFFSET] = 0;
    packet_[DHCP_SECONDS_OFFSET + 1] = 0;
    std::memset(packet_ + DHCP_YOUR_ADDRESS_OFFSET, 0,
        DHCP_MAC_OFFSET - DHCP_YOUR_ADDRESS_OFFSET);
    if (lease != DHCP_NO_LEASE) {
        std::memcpy(packet_ + DHCP_YOUR_ADDRESS_OFFSET, netif_ip4_addr(interface_), WIFI_HOTSPOT_IPV4_BYTES);
        packet_[DHCP_YOUR_ADDRESS_OFFSET + DHCP_HOST_BYTE] = uint8_t(DHCP_POOL_FIRST_HOST + lease);
    }
    std::memset(packet_ + DHCP_SERVER_NAME_OFFSET, 0, DHCP_COOKIE_OFFSET - DHCP_SERVER_NAME_OFFSET);
    std::memset(packet_ + DHCP_OPTIONS_OFFSET, 0, DHCP_REPLY_BYTES - DHCP_OPTIONS_OFFSET);

    offset = DHCP_OPTIONS_OFFSET;
    packet_[offset++] = DHCP_OPTION_TYPE;
    packet_[offset++] = DHCP_TYPE_BYTES;
    packet_[offset++] = type;
    packet_[offset++] = DHCP_OPTION_SERVER;
    packet_[offset++] = WIFI_HOTSPOT_IPV4_BYTES;
    std::memcpy(packet_ + offset, netif_ip4_addr(interface_), WIFI_HOTSPOT_IPV4_BYTES);
    offset += WIFI_HOTSPOT_IPV4_BYTES;
    if (type != DHCP_NAK) {
        packet_[offset++] = DHCP_OPTION_SUBNET;
        packet_[offset++] = WIFI_HOTSPOT_IPV4_BYTES;
        std::memcpy(packet_ + offset, netif_ip4_netmask(interface_), WIFI_HOTSPOT_IPV4_BYTES);
        offset += WIFI_HOTSPOT_IPV4_BYTES;
        if (lease != DHCP_NO_LEASE) {
            packet_[offset++] = DHCP_OPTION_LEASE;
            packet_[offset++] = WIFI_HOTSPOT_IPV4_BYTES;
            packet_[offset++] = uint8_t(DHCP_LEASE_SECONDS >> DHCP_BYTE_SHIFT_24);
            packet_[offset++] = uint8_t(DHCP_LEASE_SECONDS >> DHCP_BYTE_SHIFT_16);
            packet_[offset++] = uint8_t(DHCP_LEASE_SECONDS >> DHCP_BYTE_SHIFT_8);
            packet_[offset++] = uint8_t(DHCP_LEASE_SECONDS);
        }
    }
    packet_[offset] = DHCP_OPTION_END;

    response = pbuf_alloc(PBUF_TRANSPORT, DHCP_REPLY_BYTES, PBUF_RAM);
    if (response) {
        if (pbuf_take(response, packet_, DHCP_REPLY_BYTES) == ERR_OK) {
            udp_sendto_if(server_, response, &destination, DHCP_CLIENT_PORT, interface_);
        }
        pbuf_free(response);
    }
} // SendReply

/**
    Handles a validated DHCP datagram without retaining its packet storage.

    Rejects non-Ethernet, relayed, oversized and malformed traffic. Requests for
    another DHCP server do not consume a lease. Discover reserves a short offer;
    request/renewal extends it only after the address matches. Release and decline
    modify only the client's existing entry; no SD or framebuffer work occurs.

    @param packet Borrowed received packet, freed by Receive after this method.
*/
void
PicoHotspot::HandlePacket (
    pbuf* packet
    )
{
    Request request;
    size_t length;
    int lease;
    int index;
    const uint8_t* requestedAddress = nullptr;
    uint8_t zeroAddress[WIFI_HOTSPOT_IPV4_BYTES]{};

    length = packet->tot_len;
    if (length < DHCP_OPTIONS_OFFSET || length > sizeof(packet_) ||
        pbuf_copy_partial(packet, packet_, uint16_t(length), 0) != length) {
        return;
    }
    if (packet_[DHCP_OPCODE_OFFSET] != DHCP_REQUEST_OPCODE ||
        packet_[DHCP_HARDWARE_TYPE_OFFSET] != DHCP_ETHERNET_TYPE ||
        packet_[DHCP_HARDWARE_LENGTH_OFFSET] != WIFI_HOTSPOT_MAC_BYTES ||
        std::memcmp(packet_ + DHCP_RELAY_ADDRESS_OFFSET, zeroAddress, sizeof(zeroAddress)) != 0 ||
        packet_[DHCP_COOKIE_OFFSET] != DHCP_COOKIE_0 || packet_[DHCP_COOKIE_OFFSET + 1] != DHCP_COOKIE_1 ||
        packet_[DHCP_COOKIE_OFFSET + 2] != DHCP_COOKIE_2 || packet_[DHCP_COOKIE_OFFSET + 3] != DHCP_COOKIE_3 ||
        !ParseOptions(length, request)) {
        return;
    }
    if (request.hasServerAddress && std::memcmp(request.serverAddress,
        netif_ip4_addr(interface_), WIFI_HOTSPOT_IPV4_BYTES) != 0) {
        return;
    }

    if (request.type == DHCP_RELEASE || request.type == DHCP_DECLINE) {
        for (index = 0; index < WIFI_HOTSPOT_LEASE_COUNT; ++index) {
            if (leases_[index].occupied && std::memcmp(leases_[index].mac,
                packet_ + DHCP_MAC_OFFSET, WIFI_HOTSPOT_MAC_BYTES) == 0) {
                leases_[index].occupied = false;
                if (request.type == DHCP_DECLINE) {
                    std::memset(leases_[index].mac, 0, WIFI_HOTSPOT_MAC_BYTES);
                    leases_[index].occupied = true;
                    leases_[index].expires = make_timeout_time_ms(DHCP_OFFER_MILLISECONDS);
                }
            }
        }
        return;
    }
    if (request.type == DHCP_INFORM) {
        if (std::memcmp(packet_ + DHCP_CLIENT_ADDRESS_OFFSET, zeroAddress, sizeof(zeroAddress)) != 0) {
            SendReply(DHCP_ACK, DHCP_NO_LEASE);
        }
        return;
    }
    if (request.type != DHCP_DISCOVER && request.type != DHCP_REQUEST) {
        return;
    }

    if (request.hasRequestedAddress) {
        requestedAddress = request.requestedAddress;
    } else if (request.type == DHCP_REQUEST &&
        std::memcmp(packet_ + DHCP_CLIENT_ADDRESS_OFFSET, zeroAddress, sizeof(zeroAddress)) != 0) {
        requestedAddress = packet_ + DHCP_CLIENT_ADDRESS_OFFSET;
    }
    if (request.type == DHCP_REQUEST && !requestedAddress) {
        return;
    }
    lease = FindLease(packet_ + DHCP_MAC_OFFSET, requestedAddress);
    if (lease == DHCP_NO_LEASE && request.type == DHCP_DISCOVER) {
        lease = FindLease(packet_ + DHCP_MAC_OFFSET, nullptr);
    }
    if (lease == DHCP_NO_LEASE) {
        if (request.type == DHCP_REQUEST) {
            SendReply(DHCP_NAK, DHCP_NO_LEASE);
        }
        return;
    }

    std::memcpy(leases_[lease].mac, packet_ + DHCP_MAC_OFFSET, WIFI_HOTSPOT_MAC_BYTES);
    leases_[lease].occupied = true;
    if (request.type == DHCP_DISCOVER) {
        leases_[lease].expires = make_timeout_time_ms(DHCP_OFFER_MILLISECONDS);
        SendReply(DHCP_OFFER, lease);
    } else {
        leases_[lease].expires = make_timeout_time_ms(DHCP_LEASE_MILLISECONDS);
        SendReply(DHCP_ACK, lease);
    }
} // HandlePacket

/**
    Delivers AP-interface DHCP traffic to the owning hotspot instance.

    @param context Owning PicoHotspot registered with the UDP socket.
    @param pcb UDP socket receiving the packet.
    @param packet Received datagram; always freed before returning.
    @param address Sender address supplied by lwIP.
    @param port Sender port, which must be the direct DHCP client port.
*/
void
PicoHotspot::Receive (
    void* context,
    udp_pcb* pcb,
    pbuf* packet,
    const ip_addr_t* address,
    u16_t port
    )
{
    PicoHotspot* self = nullptr;

    (void)address;
    self = static_cast<PicoHotspot*>(context);
    if (!packet) {
        return;
    }
    if (self && pcb == self->server_ && port == DHCP_CLIENT_PORT) {
        self->HandlePacket(packet);
    }
    pbuf_free(packet);
} // Receive

/**
    Applies the configured client ceiling within the radio's supported capacity.

    Reads the firmware limit and restricts it to the requested application limit
    and the documented four-client capacity of this board. A lower requested
    limit is written back and verified before DHCP starts. The display slots
    and lease table use the same configured capacity.

    @return true - A positive, supported association ceiling is enforced.
    @return false - The radio could not report or enforce its client limit.
*/
bool
PicoHotspot::ConfigureClientLimit (
    void
    )
{
    int reported;
    int desired;
    int result;
    uint8_t setting[HOTSPOT_MAX_ASSOC_SETTING_BYTES]{};

    reported = 0;
    cyw43_wifi_ap_get_max_stas(&cyw43_state, &reported);
    desired = std::min(int(WIFI_HOTSPOT_MAX_CLIENTS), HOTSPOT_SUPPORTED_CLIENTS);
    if (reported <= 0 || desired <= 0) {
        return false;
    }
    desired = std::min(desired, reported);
    if (reported > desired) {
        std::memcpy(setting, "maxassoc", HOTSPOT_MAX_ASSOC_NAME_BYTES);
        WriteDriverUnsigned32(setting + HOTSPOT_MAX_ASSOC_NAME_BYTES, uint32_t(desired));
        result = cyw43_ioctl(&cyw43_state, CYW43_IOCTL_SET_VAR, sizeof(setting), setting, CYW43_ITF_AP);
        if (result != 0) {
            return false;
        }
        reported = 0;
        cyw43_wifi_ap_get_max_stas(&cyw43_state, &reported);
    }
    if (reported <= 0 || reported > desired) {
        return false;
    }
    clientLimit_ = uint8_t(reported);
    return true;
} // ConfigureClientLimit

/**
    Encodes an unsigned radio-control value in little-endian byte order.

    @param destination Four writable command bytes.
    @param value Unsigned value to send to the CYW43 firmware.
*/
void
PicoHotspot::WriteDriverUnsigned32 (
    uint8_t* destination,
    uint32_t value
    )
{
    unsigned index;

    for (index = 0; index < WIFI_HOTSPOT_IPV4_BYTES; ++index) {
        destination[index] = uint8_t(value >> (index * BITS_PER_BYTE));
    }
} // WriteDriverUnsigned32

/**
    Decodes a little-endian unsigned value returned by the radio.

    @param source Four readable command-result bytes.

    @return value - Decoded unsigned radio-control value.
*/
uint32_t
PicoHotspot::ReadDriverUnsigned32 (
    const uint8_t* source
    )
{
    uint32_t value;
    unsigned index;

    value = 0;
    for (index = 0; index < WIFI_HOTSPOT_IPV4_BYTES; ++index) {
        value |= uint32_t(source[index]) << (index * BITS_PER_BYTE);
    }
    return value;
} // ReadDriverUnsigned32

/**
    Refreshes stable device slots from actual radio associations.

    A bounded firmware query runs only when its refresh deadline is due. Existing
    MAC addresses keep their slots regardless of the radio's list order. Departed
    clients release their slot and DHCP lease; new associations occupy free slots.
    A failed or oversized reply retains the last confirmed snapshot for retry.

    @param nowMs Monotonic network-worker time in milliseconds.
*/
void
PicoHotspot::UpdateClients (
    uint32_t nowMs
    )
{
    uint8_t reply[HOTSPOT_ASSOC_REPLY_BYTES]{};
    uint32_t count;
    int result;
    size_t slot;
    size_t entry;
    size_t lease;
    bool present;
    bool known;
    const uint8_t* mac = nullptr;

    if (!server_ || (clientsPolled_ && nowMs - lastClientPollMs_ < WIFI_HOTSPOT_CLIENT_REFRESH_MS)) {
        return;
    }
    clientsPolled_ = true;
    lastClientPollMs_ = nowMs;
    WriteDriverUnsigned32(reply, clientLimit_);
    cyw43_arch_lwip_begin();
    result = cyw43_ioctl(&cyw43_state, HOTSPOT_ASSOC_LIST_IOCTL, sizeof(reply), reply, CYW43_ITF_AP);
    count = ReadDriverUnsigned32(reply);
    if (result != 0 || count > clientLimit_ || count > WIFI_HOTSPOT_MAX_CLIENTS) {
        cyw43_arch_lwip_end();
        return;
    }

    // Disconnects remove only that device's state; surviving peers keep stable dots.
    for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
        if (!devices_[slot].connected) {
            continue;
        }
        present = false;
        for (entry = 0; entry < count; ++entry) {
            mac = reply + WIFI_HOTSPOT_IPV4_BYTES + entry * WIFI_HOTSPOT_MAC_BYTES;
            if (std::memcmp(mac, devices_[slot].mac, WIFI_HOTSPOT_MAC_BYTES) == 0) {
                present = true;
                break;
            }
        }
        if (!present) {
            for (lease = 0; lease < WIFI_HOTSPOT_LEASE_COUNT; ++lease) {
                if (std::memcmp(leases_[lease].mac, devices_[slot].mac, WIFI_HOTSPOT_MAC_BYTES) == 0) {
                    leases_[lease] = {};
                }
            }
            devices_[slot] = {};
        }
    }
    for (entry = 0; entry < count; ++entry) {
        mac = reply + WIFI_HOTSPOT_IPV4_BYTES + entry * WIFI_HOTSPOT_MAC_BYTES;
        known = false;
        for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
            if (devices_[slot].connected && std::memcmp(mac, devices_[slot].mac, WIFI_HOTSPOT_MAC_BYTES) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
                if (!devices_[slot].connected) {
                    std::memcpy(devices_[slot].mac, mac, WIFI_HOTSPOT_MAC_BYTES);
                    devices_[slot].connected = true;
                    break;
                }
            }
        }
    }
    cyw43_arch_lwip_end();
} // UpdateClients

/**
    Copies bounded off, connected and active device states for the display core.

    @param states Destination with WIFI_HOTSPOT_MAX_CLIENTS entries.
    @param nowMs Monotonic time used to expire the visible activity hold.
*/
void
PicoHotspot::CopyClientStates (
    uint8_t* states,
    uint32_t nowMs
    ) const
{
    size_t slot;

    std::memset(states, WIFI_HOTSPOT_CLIENT_OFF, WIFI_HOTSPOT_MAX_CLIENTS);
    if (!server_) {
        return;
    }
    cyw43_arch_lwip_begin();
    for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
        if (devices_[slot].connected) {
            states[slot] = WIFI_HOTSPOT_CLIENT_CONNECTED;
            if (devices_[slot].activitySeen && nowMs - devices_[slot].lastActivityMs < WIFI_HOTSPOT_ACTIVITY_HOLD_MS) {
                states[slot] = WIFI_HOTSPOT_CLIENT_ACTIVE;
            }
        }
    }
    cyw43_arch_lwip_end();
} // CopyClientStates

/**
    Marks meaningful traffic only for its associated hotspot device.

    Transport callbacks already hold the serialized lwIP context. The local TCP
    address excludes home-interface traffic. ARP identifies static-IP clients;
    an unexpired DHCP lease provides a fallback during initial ARP setup. Only
    an already associated device can change a dot's activity state.

    @param context Owning hotspot registered with the transport activity observer.
    @param client Borrowed TCP connection carrying accepted application traffic.
*/
void
PicoHotspot::ObserveCommunication (
    void* context,
    const tcp_pcb* client
    )
{
    PicoHotspot* self = nullptr;
    const uint8_t* mac = nullptr;
    eth_addr* hardwareAddress = nullptr;
    const ip4_addr_t* cachedAddress = nullptr;
    const ip4_addr_t* remoteAddress = nullptr;
    uint8_t leaseAddress[WIFI_HOTSPOT_IPV4_BYTES];
    size_t lease;
    size_t slot;

    self = static_cast<PicoHotspot*>(context);
    if (!self->server_ || !ip4_addr_cmp(ip_2_ip4(&client->local_ip), netif_ip4_addr(self->interface_))) {
        return;
    }
    remoteAddress = ip_2_ip4(&client->remote_ip);
    if (etharp_find_addr(self->interface_, remoteAddress, &hardwareAddress, &cachedAddress) >= 0) {
        mac = hardwareAddress->addr;
    } else {
        for (lease = 0; lease < WIFI_HOTSPOT_LEASE_COUNT; ++lease) {
            if (!self->leases_[lease].occupied ||
                absolute_time_diff_us(get_absolute_time(), self->leases_[lease].expires) <= 0) {
                continue;
            }
            std::memcpy(leaseAddress, netif_ip4_addr(self->interface_), WIFI_HOTSPOT_IPV4_BYTES);
            leaseAddress[DHCP_HOST_BYTE] = uint8_t(DHCP_POOL_FIRST_HOST + lease);
            if (std::memcmp(leaseAddress, remoteAddress, WIFI_HOTSPOT_IPV4_BYTES) == 0) {
                mac = self->leases_[lease].mac;
                break;
            }
        }
    }
    if (!mac) {
        return;
    }
    for (slot = 0; slot < WIFI_HOTSPOT_MAX_CLIENTS; ++slot) {
        if (self->devices_[slot].connected && std::memcmp(self->devices_[slot].mac, mac, WIFI_HOTSPOT_MAC_BYTES) == 0) {
            self->devices_[slot].activitySeen = true;
            self->devices_[slot].lastActivityMs = to_ms_since_boot(get_absolute_time());
            return;
        }
    }
} // ObserveCommunication
