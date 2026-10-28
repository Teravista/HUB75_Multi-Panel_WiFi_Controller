#include "core/include/serialLog.hpp"
#include "network/include/WifiConnectionClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"
#include "http/include/webServer.hpp"
#include "config/defines.h"
#include "networkConfig.h"
#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "cyw43.h"
#include "lwip/netif.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

#define MAC_BYTE_INDEX_2 2
#define MAC_BYTE_INDEX_3 3
#define MAC_BYTE_INDEX_4 4
#define MAC_BYTE_INDEX_5 5
#define MAC_TEXT_BYTES 18
#define WIFI_RADIO_RESTART_SETTLE_MS 200
#define WIFI_REACHABILITY_GRACE_MS 30000


#define ARP_HARDWARE_LENGTH_OFFSET 18
#define ARP_HARDWARE_TYPE_HIGH_OFFSET 14
#define ARP_HARDWARE_TYPE_LOW_OFFSET 15
#define ARP_OPERATION_HIGH_OFFSET 20
#define ARP_OPERATION_LOW_OFFSET 21
#define ARP_PACKET_BYTES 42
#define ARP_PROTOCOL_LENGTH_OFFSET 19
#define ARP_PROTOCOL_TYPE_HIGH_OFFSET 16
#define ARP_PROTOCOL_TYPE_LOW_OFFSET 17
#define ARP_REPLY_OPERATION 2
#define ARP_SENDER_IP_OFFSET 28
#define ARP_TARGET_IP_OFFSET 38
#define ARP_TARGET_MAC_OFFSET 32
#define ETHERNET_HEADER_BYTES 14
#define ETHERTYPE_ARP_LOW 6
#define ETHERTYPE_HIGH_OFFSET 12
#define ETHERTYPE_IP_ARP_HIGH 8
#define ETHERTYPE_LOW_OFFSET 13
#define IPV4_ADDRESS_BYTES 4
#define IPV4_DESTINATION_OFFSET 30
#define IPV4_HEADER_LENGTH_MASK 15
#define IPV4_HEADER_WORD_BYTES 4
#define IPV4_MIN_HEADER_WORDS 5
#define IPV4_MIN_PACKET_BYTES 34
#define IPV4_SOURCE_OFFSET 26
#define IPV4_VERSION 4
#define IPV4_VERSION_OFFSET 14
#define IPV4_VERSION_SHIFT 4
#define MAC_ADDRESS_BYTES 6
#define WIFI_INBOUND_ACTIVITY_WINDOW_MS 15000

WifiConnection* WifiConnection::s_inputMonitor = nullptr;

/**
    Connects Wi-Fi state management to photo and video network services.

    @param frameBufferController Shared framebuffer owner and photo upload controller.
    @param videoUpload TCP video upload service.
*/
WifiConnection::WifiConnection (
    FrameBufferController* frameBufferController,
    TcpVideoUpload* videoUpload
    ) : m_frameBufferController(frameBufferController), m_videoUpload(videoUpload)
{
    return;
} // WifiConnection

/**
    Stops network services and releases radio monitoring resources.
*/
WifiConnection::~WifiConnection (
    void
    )
{
    if (m_cyw43Initialized) {
        StopNetworkServices();
    }
} // ~WifiConnection

/**
    Records received network traffic before forwarding the packet.

    Inspects a bounded copy of possibly chained Ethernet headers to recognize fresh ARP replies
    and IPv4 traffic addressed to this device. Matching traffic updates reachability evidence
    before the original interface input handler receives the unchanged packet. The observer
    neither frees nor retains packet storage, so the original lwIP input path keeps packet
    ownership.

    @param packet Received lwIP packet chain; null signals that the peer closed the connection.
    @param interface Network interface receiving this packet.

    @return ERR_VAL - The packet or input-monitor state is invalid.
    @return result - Status returned by the original network-interface input handler.
*/
err_t
WifiConnection::ObserveNetworkInput (
    pbuf* packet,
    netif* interface
    )
{
    WifiConnection* self = nullptr;
    uint8_t header[ARP_PACKET_BYTES];
    size_t copied;
    uint32_t local;
    uint32_t gateway;
    bool addressedToUs;

    self = s_inputMonitor;
    if (!self || self->m_monitoredInterface != interface || !self->m_originalInput) {
        return ERR_VAL;
    }

    // Ethernet and ARP/IPv4 headers may span multiple pbufs. The observer never
    // frees, modifies, or retains them; ownership stays with the original input.
    std::memset(header, 0, sizeof(header));
    copied = pbuf_copy_partial(packet, header, sizeof(header), 0);
    local = ip4_addr_get_u32(netif_ip4_addr(interface));
    gateway = ip4_addr_get_u32(netif_ip4_gw(interface));
    addressedToUs = false;
    if (copied >= ARP_PACKET_BYTES && header[ETHERTYPE_HIGH_OFFSET] == ETHERTYPE_IP_ARP_HIGH &&
        header[ETHERTYPE_LOW_OFFSET] == ETHERTYPE_ARP_LOW && header[ARP_HARDWARE_TYPE_HIGH_OFFSET] == 0 &&
        header[ARP_HARDWARE_TYPE_LOW_OFFSET] == 1 &&
        header[ARP_PROTOCOL_TYPE_HIGH_OFFSET] == ETHERTYPE_IP_ARP_HIGH &&
        header[ARP_PROTOCOL_TYPE_LOW_OFFSET] == 0 &&
        header[ARP_HARDWARE_LENGTH_OFFSET] == MAC_ADDRESS_BYTES &&
        header[ARP_PROTOCOL_LENGTH_OFFSET] == IPV4_ADDRESS_BYTES && header[ARP_OPERATION_HIGH_OFFSET] == 0 &&
        header[ARP_OPERATION_LOW_OFFSET] == ARP_REPLY_OPERATION &&
        std::memcmp(header + ARP_TARGET_IP_OFFSET, &local, IPV4_ADDRESS_BYTES) == 0 &&
        std::memcmp(header + ARP_TARGET_MAC_OFFSET, interface->hwaddr, MAC_ADDRESS_BYTES) == 0) {
        addressedToUs = true;
        if (self->m_arpPending && gateway &&
            std::memcmp(header + ARP_SENDER_IP_OFFSET, &gateway, IPV4_ADDRESS_BYTES) == 0) {
            self->m_arpReply = true;
        }
    } else if (copied >= IPV4_MIN_PACKET_BYTES && header[ETHERTYPE_HIGH_OFFSET] == ETHERTYPE_IP_ARP_HIGH &&
               header[ETHERTYPE_LOW_OFFSET] == 0 &&
               (header[IPV4_VERSION_OFFSET] >> IPV4_VERSION_SHIFT) == IPV4_VERSION &&
               (header[IPV4_VERSION_OFFSET] & IPV4_HEADER_LENGTH_MASK) >= IPV4_MIN_HEADER_WORDS &&
               packet->tot_len >=
                   ETHERNET_HEADER_BYTES +
                       (header[IPV4_VERSION_OFFSET] & IPV4_HEADER_LENGTH_MASK) * IPV4_HEADER_WORD_BYTES &&
               std::memcmp(header + IPV4_DESTINATION_OFFSET, &local, IPV4_ADDRESS_BYTES) == 0 &&
               std::memcmp(header + IPV4_SOURCE_OFFSET, &local, IPV4_ADDRESS_BYTES) != 0) {
        addressedToUs = true;
    }
    if (addressedToUs) {
        self->m_inboundSeen = true;
        self->m_lastInboundMs = to_ms_since_boot(get_absolute_time());
    }
    return self->m_originalInput(packet, interface);
} // ObserveNetworkInput

/**
    Installs the network input monitor used by reachability checks.

    Installs one wrapper around the default interface's original input function while holding the
    lwIP lock. It resets evidence for the new monitoring session and schedules a fresh gateway
    check. With no gateway configured, local service availability is not made dependent on a
    nonexistent remote probe target.
*/
void
WifiConnection::StartInputMonitor (
    void
    )
{
    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (netif_default && !m_monitoredInterface) {
        m_monitoredInterface = netif_default;
        m_originalInput = netif_default->input;
        s_inputMonitor = this;
        netif_default->input = ObserveNetworkInput;
    }
    m_arpPending = m_arpReply = m_inboundSeen = false;
    m_arpFailStreak = 0;
    // Without a gateway there is no remote target to check; keep local services.
    m_reachabilityVerified = netif_default && ip4_addr_isany_val(*netif_ip4_gw(netif_default));
    m_nextArpProbeTime = get_absolute_time();
    cyw43_arch_lwip_end();
} // StartInputMonitor

/**
    Restores the original network input handler.
*/
void
WifiConnection::StopInputMonitor (
    void
    )
{
    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (m_monitoredInterface && m_monitoredInterface->input == ObserveNetworkInput) {
        m_monitoredInterface->input = m_originalInput;
    }
    if (s_inputMonitor == this) {
        s_inputMonitor = nullptr;
    }
    m_monitoredInterface = nullptr;
    m_originalInput = nullptr;
    m_arpPending = false;
    cyw43_arch_lwip_end();
} // StopInputMonitor

/**
    Updates local network reachability from received traffic and probes.

    Combines fresh gateway ARP replies with recent incoming traffic as evidence of a working local
    path. Expired unanswered requests advance a failure streak, while new probes explicitly send
    ARP rather than trusting cached entries. The checks poll deadlines without waiting for
    replies. Only a debounced loss of both forms of evidence requests recovery, after the lwIP
    lock has been released.
*/
void
WifiConnection::CheckReachability (
    void
    )
{
    uint64_t now;
    uint32_t nowMs;
    bool recentTraffic;
    ip4_addr_t gateway;
    err_t result;
    bool lost;

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    now = get_absolute_time();
    nowMs = to_ms_since_boot(now);
    // Actual incoming traffic also proves communication, even if ARP is filtered.
    recentTraffic = m_inboundSeen && nowMs - m_lastInboundMs < WIFI_INBOUND_ACTIVITY_WINDOW_MS;
    if (m_arpReply || recentTraffic) {
        if (!m_reachabilityVerified || m_arpFailStreak) {
            HUB75_DEBUG("[net] packet communication verified\n");
        }
        m_reachabilityVerified = true;
        m_arpFailStreak = 0;
    }
    if (m_arpPending && absolute_time_diff_us(now, m_arpProbeDeadline) <= 0) {
        m_arpPending = false;
        if (!m_arpReply && !recentTraffic) {
            ++m_arpFailStreak;
            HUB75_DEBUG("[net] gateway ARP timeout, failures=%u (link/IP still up)\n", m_arpFailStreak);
        }
    }
    if (!m_arpPending && absolute_time_diff_us(now, m_nextArpProbeTime) <= 0 && netif_default) {
        m_nextArpProbeTime = make_timeout_time_ms(WIFI_GATEWAY_PROBE_INTERVAL_MS);
        gateway = *netif_ip4_gw(netif_default);
        if (!ip4_addr_isany_val(gateway)) {
            m_arpPending = true;
            m_arpReply = false;
            m_arpProbeDeadline = make_timeout_time_ms(WIFI_GATEWAY_PROBE_TIMEOUT_MS);
            // Always send a fresh request; an old ARP cache entry is not evidence.
            result = etharp_request(netif_default, &gateway);
            if (result != ERR_OK) {
                m_arpPending = false;
                if (!recentTraffic) {
                    ++m_arpFailStreak;
                }
                HUB75_DEBUG("[net] gateway ARP send failed, err=%d failures=%u\n", result, m_arpFailStreak);
            }
        }
    }
    lost = m_arpFailStreak >= WIFI_GATEWAY_PROBE_FAIL_DEBOUNCE_COUNT;
    cyw43_arch_lwip_end();
    if (lost) {
        EnterNoWifiState("gateway ARP and incoming traffic lost; restarting radio");
    }
} // CheckReachability

/**
    Returns a readable name for a Wi-Fi driver link state.

    Converts the SDK's link-stage and failure codes into short names for connection logs. Unknown
    numeric values receive a stable fallback instead of being interpreted as a known link state.
    This is a pure formatting lookup and does not query the radio.

    @param status Status snapshot or protocol status code used by this operation.

    @return "DOWN" - Down status.
    @return "JOIN" - Join status.
    @return "NOIP" - Noip status.
    @return "UP" - Up status.
    @return "FAIL" - Fail status.
    @return "NONET" - Nonet status.
    @return "BADAUTH" - Badauth status.
    @return "UNKNOWN" - Unknown status.
*/
const char*
WifiConnection::GetCyw43StatusName (
    int status
    )
{
    switch (status) {
    case CYW43_LINK_DOWN:
        return "DOWN";
    case CYW43_LINK_JOIN:
        return "JOIN";
    case CYW43_LINK_NOIP:
        return "NOIP";
    case CYW43_LINK_UP:
        return "UP";
    case CYW43_LINK_FAIL:
        return "FAIL";
    case CYW43_LINK_NONET:
        return "NONET";
    case CYW43_LINK_BADAUTH:
        return "BADAUTH";
    default:
        return "UNKNOWN";
    }
} // GetCyw43StatusName

/**
    Returns a readable name for a Wi-Fi connection result.

    Separates a successful SDK call, timeout, authentication failure and explicit connection
    failure for serial reporting. Unknown SDK errors retain a generic name rather than being
    mistaken for a missing network. The mapping does not change retry policy or the independently
    queried live link state.

    @param result Result code returned by the SDK or filesystem operation.

    @return "OK" - Ok status.
    @return "TIMEOUT" - Timeout status.
    @return "BADAUTH" - Badauth status.
    @return "CONNECT_FAILED" - Connect failed status.
    @return "SDK_ERROR" - Sdk error status.
*/
const char*
WifiConnection::GetConnectResultName (
    int result
    )
{
    switch (result) {
    case PICO_OK:
        return "OK";
    case PICO_ERROR_TIMEOUT:
        return "TIMEOUT";
    case PICO_ERROR_BADAUTH:
        return "BADAUTH";
    case PICO_ERROR_CONNECT_FAILED:
        return "CONNECT_FAILED";
    default:
        return "SDK_ERROR";
    }
} // GetConnectResultName

/**
    Checks whether the station interface has a usable IPv4 address.

    @return true - The station has a nonzero IPv4 address.
    @return false - The station interface or its IPv4 address is unavailable.
*/
bool
WifiConnection::HasIpv4Address (
    void
    )
{
    ip4_addr_t ipAddr;

    if (netif_default == nullptr) {
        return false;
    }

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    ipAddr = *netif_ip4_addr(netif_default);
    cyw43_arch_lwip_end();
    return !ip4_addr_isany_val(ipAddr);
} // HasIpv4Address

/**
    Checks whether the station link and IPv4 configuration are usable.

    @return true - The Wi-Fi link is up and an IPv4 address is assigned.
    @return false - The link or IPv4 address is unavailable.
*/
bool
WifiConnection::IsWifiUsable (
    void
    ) const
{
    bool linked;

    if (!m_cyw43Initialized) {
        return false;
    }

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    linked = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA) == CYW43_LINK_UP;
    cyw43_arch_lwip_end();
    return linked && HasIpv4Address();
} // IsWifiUsable

/**
    Checks whether the Wi-Fi driver still reports an active radio.

    @return true - The radio returned a valid nonzero RSSI reading.
    @return false - The radio query failed or returned no usable reading.
*/
bool
WifiConnection::IsRadioResponding (
    void
    )
{
    int32_t rssi;
    int ret;

    rssi = 0;
    cyw43_arch_lwip_begin();
    ret = cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    cyw43_arch_lwip_end();
    return ret == 0 && rssi != 0;
} // IsRadioResponding

/**
    Stops network services and schedules a Wi-Fi reconnect attempt.

    @param reason Human-readable reason for the state transition.
*/
void
WifiConnection::EnterNoWifiState (
    const char* reason
    )
{
    printf("[net] -> no-wifi (%s)\n", reason);
    StopNetworkServices();
    m_netState = NetState::NoWifi;
    m_wifiIpText.clear();
    m_rssiProbeFailStreak = 0;
    m_gatewayProbeFailStreak = 0;
    m_forceRadioRestart = true;
    m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
} // EnterNoWifiState

/**
    Records a failed TCP gateway probe and releases its connection.

    Handles a PCB that lwIP has already disposed of and clears the retained probe pointer. A peer
    reset counts as a returned response even when the diagnostic TCP port is closed; local
    failures and retransmission exhaustion increment diagnostic failures. This callback must not
    attempt to close the already destroyed PCB.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param err Error code delivered by lwIP.
*/
void
WifiConnection::OnGatewayProbeErr (
    void* arg,
    err_t err
    )
{
    WifiConnection* self = nullptr;

    self = static_cast<WifiConnection*>(arg);

    if (self == nullptr) {
        return;
    }

    self->m_gatewayProbePcb = nullptr;

    // Only a peer reset proves a response. Local errors (including exhausted
    // retransmissions) do not establish gateway reachability.
    if (err != ERR_RST) {
        self->m_gatewayProbeFailStreak++;
        HUB75_DEBUG("[net] gateway probe failed (err=%d, failures=%u)\n", err,
                    self->m_gatewayProbeFailStreak);
        return;
    }

    // A gateway RST proves a reply came back even though the port did not accept.
    self->m_gatewayProbeFailStreak = 0;
    HUB75_DEBUG("[net] gateway probe response (err=%d)\n", err);
} // OnGatewayProbeErr

/**
    Records a successful gateway probe and closes its connection.

    Detaches probe callbacks once connection completion has been delivered, then resets diagnostic
    failures on success and closes the temporary socket. A failed completion or close aborts the
    PCB and returns the matching lwIP callback status. The probe only measures local response
    behavior; it does not carry upload data.

    @param arg Instance pointer supplied when registering the lwIP callback.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param err Error code delivered by lwIP.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
WifiConnection::OnGatewayProbeConnected (
    void* arg,
    tcp_pcb* pcb,
    err_t err
    )
{
    WifiConnection* self = nullptr;

    self = static_cast<WifiConnection*>(arg);

    if (self == nullptr) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    self->m_gatewayProbePcb = nullptr;
    tcp_arg(pcb, nullptr);
    tcp_err(pcb, nullptr);

    // If the connection attempt succeeded, the gateway is reachable so we still have a valid WiFi link.
    // Close the connection and reset the fail streak.
    if (err == ERR_OK) {
        self->m_gatewayProbeFailStreak = 0;
        HUB75_DEBUG("[net] gateway probe connected\n");
        if (tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
            return ERR_ABRT;
        }
    } else {
        self->m_gatewayProbeFailStreak++;
        HUB75_DEBUG("[net] gateway probe failed (connect err=%d, failures=%u)\n", err,
                    self->m_gatewayProbeFailStreak);
        tcp_abort(pcb);
        return ERR_ABRT;
    }

    return ERR_OK;
} // OnGatewayProbeConnected

/**
    Starts a bounded TCP reachability probe to the local gateway.

    Starts one asynchronous TCP connection to the configured local gateway when there is no probe
    already active. It records a deadline and callback context, cleaning up immediately if
    allocation or connection startup fails. Starting the probe does not wait for a connection and
    does not claim that Internet access exists.
*/
void
WifiConnection::StartGatewayProbe (
    void
    )
{
    ip4_addr_t gateway;
    tcp_pcb* pcb = nullptr;
    err_t connectErr;

    gateway = {};

    if (m_gatewayProbePcb != nullptr || netif_default == nullptr) {
        return;
    }

    gateway = *netif_ip4_gw(netif_default);
    if (ip4_addr_isany_val(gateway)) {
        HUB75_DEBUG("[net] gateway probe skipped (no gateway)\n");
        return;
    }

    pcb = tcp_new();
    if (pcb == nullptr) {
        m_gatewayProbeFailStreak++;
        HUB75_DEBUG("[net] gateway probe allocation failed (failures=%u)\n", m_gatewayProbeFailStreak);
        return;
    }

    tcp_arg(pcb, this);
    tcp_err(pcb, OnGatewayProbeErr);
    m_gatewayProbePcb = pcb;
    m_gatewayProbeDeadline = make_timeout_time_ms(WIFI_GATEWAY_PROBE_TIMEOUT_MS);

    connectErr = tcp_connect(pcb, reinterpret_cast<const ip_addr_t*>(&gateway), WIFI_GATEWAY_PROBE_PORT,
                             OnGatewayProbeConnected);

    if (connectErr != ERR_OK) {
        m_gatewayProbePcb = nullptr;
        m_gatewayProbeFailStreak++;
        HUB75_DEBUG("[net] gateway probe start failed (err=%d, failures=%u)\n", connectErr,
                    m_gatewayProbeFailStreak);
        tcp_arg(pcb, nullptr);
        tcp_err(pcb, nullptr);
        tcp_abort(pcb);
    }
} // StartGatewayProbe

/**
    Advances gateway probing and handles its timeout or retry.

    Polls the active diagnostic probe's deadline and schedules a new attempt when due. Timed-out
    PCBs are detached and aborted, while repeated failures lengthen the retry interval and produce
    one diagnostic warning. These TCP failures alone do not tear down working upload listeners;
    independent reachability checks decide whether the local path needs recovery.
*/
void
WifiConnection::UpdateGatewayProbe (
    void
    )
{
    absolute_time_t now;
    bool gatewayProbeExpired;
    bool gatewayProbeDue;
    uint32_t probeIntervalMs;

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();

    now = get_absolute_time();
    gatewayProbeExpired = absolute_time_diff_us(now, m_gatewayProbeDeadline) <= 0;
    gatewayProbeDue = absolute_time_diff_us(now, m_nextGatewayProbeTime) <= 0;

    // Abort only an active probe whose connection attempt exceeded its deadline.
    if (m_gatewayProbePcb != nullptr && gatewayProbeExpired) {
        HUB75_DEBUG("[net] gateway TCP probe timeout (diagnostic; ARP checked separately)\n");
        m_gatewayProbeFailStreak++;
        tcp_arg(m_gatewayProbePcb, nullptr);
        tcp_abort(m_gatewayProbePcb);
        m_gatewayProbePcb = nullptr;
    }

    // Restart the timer for probe and attempt a new probe if none is active.
    if (gatewayProbeDue) {
        probeIntervalMs = {};
        if (m_gatewayProbeFailStreak >= WIFI_GATEWAY_PROBE_FAIL_DEBOUNCE_COUNT) {
            probeIntervalMs = WIFI_REACHABILITY_GRACE_MS;
        } else {
            probeIntervalMs = WIFI_GATEWAY_PROBE_INTERVAL_MS;
        }
        m_nextGatewayProbeTime = make_timeout_time_ms(probeIntervalMs);
        StartGatewayProbe();
    }

    if (m_gatewayProbeFailStreak >= WIFI_GATEWAY_PROBE_FAIL_DEBOUNCE_COUNT &&
        !m_gatewayProbeWarningReported) {
        HUB75_DEBUG("[net] gateway probe unavailable; keeping upload services (TCP port diagnostic; ARP "
                    "health monitored)\n");
        m_gatewayProbeWarningReported = true;
    } else if (m_gatewayProbeFailStreak == 0) {
        m_gatewayProbeWarningReported = false;
    }

    cyw43_arch_lwip_end();
} // UpdateGatewayProbe

/**
    Initializes the radio and attempts station association and address acquisition.

    Stops local listeners through their lifecycle boundary, publishes radio-restart status, and
    recreates the CYW43 station context before attempting association and DHCP. The SDK connection
    call can block up to the configured timeout; this work runs on the network core while Core 0
    retains SD ownership and playback state.

    After the SDK returns, actual link/address usability determines the next state. A timeout with
    association or DHCP still progressing keeps the radio alive for a grace period so a late
    address can be adopted. Other failures schedule a later retry, while a usable address advances
    to listener startup.
*/
void
WifiConnection::RunWifiConnectSequence (
    void
    )
{
    char macBuffer[MAC_TEXT_BYTES];
    int connectResult;
    const uint8_t* mac = nullptr;
    int initializationResult;
    int link;
    int tcpip;
    bool pending;
    uint32_t retryDelayMs;

    printf("[net] -> connecting\n");

    StopNetworkServices();
    if (m_beforeRadioRestart) {
        m_beforeRadioRestart(m_restartContext);
    }
    m_forceRadioRestart = false;

    // if is already initialized then deinit for fresh state for clean connect sequence
    if (m_cyw43Initialized) {
        cyw43_arch_deinit();
        m_cyw43Initialized = false;
        sleep_ms(WIFI_RADIO_RESTART_SETTLE_MS);
    }

    HUB75_DEBUG("[wifi] initializing radio\n");
    initializationResult = cyw43_arch_init();
    if (initializationResult) {

        printf("[wifi] cyw43_arch_init failed, code=%d\n", initializationResult);
        m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        return;
    }
    m_cyw43Initialized = true;
    HUB75_DEBUG("[wifi] radio initialized; enabling station mode\n");
    // enable client mode and connect to the configured WiFi network
    cyw43_arch_enable_sta_mode();
    // CYW43 owns separate PIO/DMA resources. Core 0 keeps its open SD reader
    // and advances frames during radio initialization and association alike.

    mac = cyw43_state.mac;
    snprintf(macBuffer, sizeof(macBuffer), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1],
             mac[MAC_BYTE_INDEX_2], mac[MAC_BYTE_INDEX_3], mac[MAC_BYTE_INDEX_4], mac[MAC_BYTE_INDEX_5]);

    HUB75_DEBUG("[wifi] MAC %s, connecting to %s...\n", macBuffer, WIFI_SSID);
    connectResult = cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK,
                                                       WIFI_CONNECT_TIMEOUT_MS);

    // check after the blocking wifi connect (non blocking connect is unstable)
    if (!IsWifiUsable()) {
        cyw43_arch_lwip_begin();
        link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        tcpip = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        printf("[wifi] connect incomplete, code=%d (%s), link=%s tcpip=%s\n", connectResult,
               GetConnectResultName(connectResult), GetCyw43StatusName(link), GetCyw43StatusName(tcpip));
        pending = connectResult == PICO_ERROR_TIMEOUT && (tcpip == CYW43_LINK_JOIN || tcpip == CYW43_LINK_NOIP);
        if (pending) {
            printf("[wifi] join/IP still pending; keeping radio alive for %u ms\n", WIFI_JOIN_GRACE_MS);
        }
        retryDelayMs = {};
        if (pending) {
            retryDelayMs = WIFI_JOIN_GRACE_MS;
        } else {
            retryDelayMs = WIFI_RETRY_INTERVAL_MS;
        }
        m_nextWifiAttemptTime = make_timeout_time_ms(retryDelayMs);
        return;
    }

    EnterWifiNoTcpState();
} // RunWifiConnectSequence

/**
    Records the acquired IP and schedules local service startup.
*/
void
WifiConnection::EnterWifiNoTcpState (
    void
    )
{
    RefreshWifiIpText();
    StartInputMonitor();

    printf("[net] -> wifi-no-tcp, ip=%s\n", m_wifiIpText.c_str());
    // WiFi is connected, but the frame upload server is not listening yet.
    m_netState = NetState::WifiNoTcp;
    m_rssiProbeFailStreak = 0;
    m_gatewayProbeFailStreak = 0;
    m_gatewayProbeWarningReported = false;
    m_nextGatewayProbeTime = get_absolute_time();
    m_nextBindAttemptTime = get_absolute_time();
    m_nextHealthCheckTime = make_timeout_time_ms(WIFI_HEALTH_CHECK_INTERVAL_MS);
} // EnterWifiNoTcpState

/**
    Detects lost station links or addresses and schedules recovery.

    Runs only when its health deadline is due, then checks link/address usability, refreshes
    potentially changed DHCP text and debounces radio-response failures. It also advances
    packet/ARP reachability and asynchronous TCP diagnostics. A failed link or debounced
    radio/path fault schedules recovery; a filtered TCP probe alone remains diagnostic so active
    transfers can continue.
*/
void
WifiConnection::CheckWifiHealth (
    void
    )
{
    // to prevent spamming the serial output
    if (absolute_time_diff_us(get_absolute_time(), m_nextHealthCheckTime) > 0) {
        return;
    }
    m_nextHealthCheckTime = make_timeout_time_ms(WIFI_HEALTH_CHECK_INTERVAL_MS);

    if (!IsWifiUsable()) {
        EnterNoWifiState("link/ip lost");
        return;
    }

    // DHCP can change the address without a full Wi-Fi reconnect.
    RefreshWifiIpText();

    // succseful response reset fail counter
    if (IsRadioResponding()) {
        m_rssiProbeFailStreak = 0;
    } else {
        m_rssiProbeFailStreak++;
        if (m_rssiProbeFailStreak >= RSSI_PROBE_FAIL_DEBOUNCE_COUNT) {
            EnterNoWifiState("radio not responding");
            return;
        }
    }

    CheckReachability();
    if (m_netState == NetState::NoWifi) {
        return;
    }
    UpdateGatewayProbe();
    // A router can silently drop TCP probes while local uploads remain usable.
    // Probe failures are diagnostic, never a reason to cancel client transfers.
} // CheckWifiHealth

/**
    Initializes connection, retry, and health-check deadlines.
*/
void
WifiConnection::InitializeNetworkTiming (
    void
    )
{
    m_nextWifiAttemptTime = get_absolute_time();
} // InitializeNetworkTiming

/**
    Advances Wi-Fi connection, local services, and recovery states.

    Advances the local network through no-Wi-Fi, address-acquired and listeners-ready states using
    retry deadlines. The disconnected state can adopt late DHCP success before deciding to
    recreate the radio. Listener creation is bracketed by foreground pause/resume callbacks so it
    cannot race a shared-buffer operation.

    A connect sequence can block inside the SDK and a service handoff can wait for Core 0
    acknowledgement; ordinary deadline checks simply return until due. Ready state continues
    health checks and detects missing listeners. Recovery affects network services without forcing
    the SD manager to remount.
*/
void
WifiConnection::UpdateNetworkStateMachine (
    void
    )
{
    bool rawReady;
    bool webReady;
    bool videoReady;

    switch (m_netState) {
    case NetState::NoWifi:
        // A timed-out blocking call can leave association/DHCP progressing.
        // Adopt a late address instead of throwing it away at the next retry.
        if (!m_forceRadioRestart && IsWifiUsable()) {
            HUB75_DEBUG("[wifi] connection completed; IP address acquired\n");
            EnterWifiNoTcpState();
            break;
        }
        // Attempt to connect to WiFi if the retry interval has elapsed.
        if (absolute_time_diff_us(get_absolute_time(), m_nextWifiAttemptTime) <= 0) {
            RunWifiConnectSequence();
        }
        break;

    case NetState::WifiNoTcp:
        CheckWifiHealth();
        // double check if wifi is still up
        if (m_netState != NetState::WifiNoTcp) {
            break;
        }

        // Attempt to bind the TCP listener if the retry interval has elapsed.
        if (absolute_time_diff_us(get_absolute_time(), m_nextBindAttemptTime) <= 0) {
            m_nextBindAttemptTime = make_timeout_time_ms(TCP_BIND_RETRY_INTERVAL_MS);
            if (m_beginServices) {
                m_beginServices(m_servicesContext);
            }
            rawReady = m_frameBufferController && m_frameBufferController->StartFrameUploadServer(IMAGE_UPLOAD_PORT);
            webReady = m_frameBufferController && WebServer::Start(*m_frameBufferController);
            videoReady = !m_videoUpload || m_videoUpload->Start();
            if (m_endServices) {
                m_endServices(m_servicesContext);
            }
            if (rawReady && webReady && videoReady) {
                printf("[net] -> ready http://%s/ tcp=5001,5002\n", m_wifiIpText.c_str());
                m_netState = NetState::Ready;
            } else {
                printf("[net] upload listeners pending: photo=%u http=%u video=%u\n", unsigned(rawReady),
                       unsigned(webReady), unsigned(videoReady));
            }
            // if failed then just keep the wifinotcp state and retry after the interval
        }
        break;

    case NetState::Ready:
        // check if wifi is still up and healthy, if not then go back to no-wifi state
        CheckWifiHealth();
        if (m_netState != NetState::Ready) {
            break;
        }

        // Return to NoWifi if the upload listener is no longer available.
        if (m_frameBufferController == nullptr || !m_frameBufferController->IsFrameUploadServerListening() ||
            !WebServer::IsListening() || (m_videoUpload && !m_videoUpload->IsListening())) {
            EnterNoWifiState("TCP listener unavailable");
        }
        break;
    }
} // UpdateNetworkStateMachine

/**
    Reports whether local services and configured reachability checks are ready.

    @return true - Local listeners are ready and reachability has been verified.
    @return false - Wi-Fi, listeners or reachability checks are not ready.
*/
bool
WifiConnection::IsNetworkReady (
    void
    ) const
{
    return m_netState == NetState::Ready && m_reachabilityVerified && !m_arpFailStreak;
} // IsNetworkReady

/**
    Returns the readable current network state.

    Combines the lifecycle state with reachability verification when formatting the ready state.
    This distinguishes started local listeners from a path that has already demonstrated working
    packet communication. The returned static text is for status reporting and does not advance
    any connection attempt.

    @return "no-wifi" - No usable Wi-Fi connection is available.
    @return "wifi-no-tcp" - Wi-Fi has an IP address but local listeners are not all ready.
    @return "ready" - The required local services are ready.
    @return "ready-unverified" - Listeners are ready but local reachability is not yet verified.
    @return "unknown" - The stored state value is not recognized.
*/
const char*
WifiConnection::GetNetStateName (
    void
    ) const
{
    switch (m_netState) {
    case NetState::NoWifi:
        return "no-wifi";
    case NetState::WifiNoTcp:
        return "wifi-no-tcp";
    case NetState::Ready: {
        if (IsNetworkReady()) {
            return "ready";
        } else {
            return "ready-unverified";
        }
    }
    }

    return "unknown";
} // GetNetStateName

/**
    Returns the station's current IPv4 address text.

    @return address - Borrowed IPv4 address text, valid until connection state updates it.
    @return "-" - No usable IPv4 address is currently assigned.
*/
const char*
WifiConnection::GetWifiIpText (
    void
    ) const
{
    if (m_netState == NetState::NoWifi || m_wifiIpText.empty()) {
        return "-";
    }
    return m_wifiIpText.c_str();
} // GetWifiIpText

/**
    Stops upload and HTTP services before a network transition.

    Idempotent and safe on first startup / failed CYW43 initialization.
*/
void
WifiConnection::StopNetworkServices (
    void
    )
{
    if (!m_cyw43Initialized) {
        return;
    }
    if (m_beginServices) {
        m_beginServices(m_servicesContext);
    }
    StopInputMonitor();
    if (m_videoUpload) {
        m_videoUpload->Stop();
    }
    WebServer::Stop();
    if (m_frameBufferController) {
        m_frameBufferController->StopFrameUploadServer();
    }

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (m_gatewayProbePcb) {
        tcp_arg(m_gatewayProbePcb, nullptr);
        tcp_err(m_gatewayProbePcb, nullptr);
        tcp_abort(m_gatewayProbePcb);
        m_gatewayProbePcb = nullptr;
    }
    cyw43_arch_lwip_end();
    if (m_endServices) {
        m_endServices(m_servicesContext);
    }
} // StopNetworkServices

/**
    Updates the cached IPv4 address from the station interface.
*/
void
WifiConnection::RefreshWifiIpText (
    void
    )
{
    char address[IPV4_TEXT_BYTES];

    std::memset(address, 0, sizeof(address));

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (netif_default != nullptr) {
        ip4addr_ntoa_r(netif_ip4_addr(netif_default), address, sizeof(address));
    }
    cyw43_arch_lwip_end();
    m_wifiIpText = address;
} // RefreshWifiIpText

/**
    Reports whether the photo transport is currently receiving a frame.

    @return true - A photo upload is currently receiving pixels.
    @return false - No photo upload is currently receiving pixels.
*/
bool
WifiConnection::IsPhotoUploadInProgress (
    void
    ) const
{
    return m_frameBufferController && m_frameBufferController->IsFrameUploadInProgress();
} // IsPhotoUploadInProgress

/**
    Reports whether the Wi-Fi radio has been initialized.

    @return true - The Wi-Fi radio has been initialized.
    @return false - The Wi-Fi radio is not initialized.
*/
bool
WifiConnection::IsRadioInitialized (
    void
    ) const
{
    return m_cyw43Initialized;
} // IsRadioInitialized

/**
    Registers a foreground handoff before radio reinitialization.

    Network-core notification after services stop, before radio deinit/init. Storage remains
    mounted; the worker uses this to publish disconnected status.

    @param callback Notification invoked immediately before the radio is restarted.
    @param context Caller-owned state passed unchanged to the callback.
*/
void
WifiConnection::SetBeforeRadioRestart (
    void (*callback)(void* context),
    void* context
    )
{
    m_beforeRadioRestart = callback;
    m_restartContext = context;
} // SetBeforeRadioRestart

/**
    Registers pause and resume callbacks for network service transitions.

    Lifecycle hooks bracket only listener changes, never the blocking join. They let Core 0
    quiesce foreground transport/storage users before changes.

    @param begin Callback that pauses foreground transport users before listener changes.
    @param end Callback that resumes foreground transport users after listener changes.
    @param context Caller-owned state passed unchanged to the callback.
*/
void
WifiConnection::SetServiceLifecycle (
    void (*begin)(void* context),
    void (*end)(void* context),
    void* context
    )
{
    m_beginServices = begin;
    m_endServices = end;
    m_servicesContext = context;
} // SetServiceLifecycle

/**
    Initializes an unbound Wi-Fi state controller.
*/
WifiConnection::WifiConnection (
    void
    )
{
    return;
} // WifiConnection
