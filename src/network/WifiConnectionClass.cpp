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
        m_hotspot.Stop();
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

    Installs one wrapper around the station interface's original input function while holding the
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
    if (!m_monitoredInterface) {
        m_monitoredInterface = StationInterface();
        m_originalInput = StationInterface()->input;
        s_inputMonitor = this;
        StationInterface()->input = ObserveNetworkInput;
    }
    m_arpPending = m_arpReply = m_inboundSeen = false;
    m_arpFailStreak = 0;
    // Without a gateway there is no remote target to check; keep local services.
    m_reachabilityVerified = ip4_addr_isany_val(*netif_ip4_gw(StationInterface()));
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
    if (!m_arpPending && absolute_time_diff_us(now, m_nextArpProbeTime) <= 0) {
        m_nextArpProbeTime = make_timeout_time_ms(WIFI_GATEWAY_PROBE_INTERVAL_MS);
        gateway = *netif_ip4_gw(StationInterface());
        if (!ip4_addr_isany_val(gateway)) {
            m_arpPending = true;
            m_arpReply = false;
            m_arpProbeDeadline = make_timeout_time_ms(WIFI_GATEWAY_PROBE_TIMEOUT_MS);
            // Always send a fresh request; an old ARP cache entry is not evidence.
            result = etharp_request(StationInterface(), &gateway);
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
        EnterNoWifiState("home gateway and incoming traffic lost");
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

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    ipAddr = *netif_ip4_addr(StationInterface());
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
    Stops station monitoring and schedules a home Wi-Fi reconnect attempt.

    A station failure does not close shared listeners or the hotspot. Its ARP/TCP
    probes and cached address are released independently. Only a debounced radio
    fault requests full recovery; that reset waits until no client is active.

    @param reason Human-readable reason for the station transition.
    @param restartRadio True requests a deferred reset of the shared radio.
*/
void
WifiConnection::EnterNoWifiState (
    const char* reason,
    bool restartRadio
    )
{
    printf("[net] -> no-wifi (%s)\n", reason);
    StopStationMonitoring();
    cyw43_arch_disable_sta_mode();
    m_netState = NetState::NoWifi;
    m_wifiIpText.clear();
    m_stationConnecting = false;
    m_rssiProbeFailStreak = 0;
    m_forceRadioRestart = restartRadio;
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

    if (m_gatewayProbePcb != nullptr) {
        return;
    }

    gateway = *netif_ip4_gw(StationInterface());
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
    Initializes the radio and starts a nonblocking home Wi-Fi association.

    The hotspot uses the same radio but retains its interface and DHCP leases
    across ordinary home retries. Association and DHCP advance asynchronously;
    subsequent network passes keep HTTP replies and upload acknowledgements
    moving while the station waits. A full radio fault recovery explicitly closes
    clients and the hotspot before deinitialization, then recreates both services.
*/
void
WifiConnection::RunWifiConnectSequence (
    void
    )
{
    int result;

    if (HasActiveNetworkClient()) {
        return;
    }
    if (m_forceRadioRestart && m_cyw43Initialized) {
        StopNetworkServices();
        m_hotspot.Stop();
        if (m_beforeRadioRestart) {
            m_beforeRadioRestart(m_restartContext);
        }
        cyw43_arch_deinit();
        m_cyw43Initialized = false;
    }
    m_forceRadioRestart = false;

    if (!m_cyw43Initialized) {
        result = cyw43_arch_init();
        if (result != PICO_OK) {
            printf("[wifi] radio initialization failed code=%d\n", result);
            m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
            return;
        }
        m_cyw43Initialized = true;
        m_nextHotspotAttemptTime = get_absolute_time();
    }

    // Reset only the station interface; AP clients and all shared listeners stay alive.
    StopStationMonitoring();
    cyw43_arch_disable_sta_mode();
    cyw43_arch_enable_sta_mode();
    cyw43_arch_lwip_begin();
    netif_set_default(StationInterface());
    cyw43_arch_lwip_end();
    printf("[net] -> connecting\n");
    result = cyw43_arch_wifi_connect_async(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK);
    if (result != PICO_OK) {
        printf("[wifi] connection start failed code=%d (%s)\n", result, GetConnectResultName(result));
        EnterNoWifiState("association start failed");
        return;
    }
    m_stationConnecting = true;
    m_stationDeadline = make_timeout_time_ms(WIFI_CONNECT_TIMEOUT_MS + WIFI_JOIN_GRACE_MS);
} // RunWifiConnectSequence

/**
    Records the acquired IP and schedules local service startup.
*/
void
WifiConnection::EnterWifiNoTcpState (
    void
    )
{
    m_stationConnecting = false;
    RefreshWifiIpText();
    StartInputMonitor();

    printf("[net] -> wifi-no-tcp, ip=%s\n", m_wifiIpText.c_str());
    // The shared listeners may already be serving the direct hotspot.
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
    // Bound radio and gateway work to the health-check interval.
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

    // Debounce radio-query failures before requesting a full radio reset.
    if (IsRadioResponding()) {
        m_rssiProbeFailStreak = 0;
    } else {
        m_rssiProbeFailStreak++;
        if (m_rssiProbeFailStreak >= RSSI_PROBE_FAIL_DEBOUNCE_COUNT) {
            EnterNoWifiState("radio not responding", true);
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
    Advances home Wi-Fi, the direct hotspot and shared listener recovery.

    AP availability is independent of station association and router reachability.
    Each pass polls the asynchronous station deadline, starts a missing hotspot
    on its own retry timer and maintains one set of HTTP/photo/video listeners.
    Normal home failures leave those listeners and active AP clients untouched.
    Core 0 retains SD ownership and playback throughout network retries.
*/
void
WifiConnection::UpdateNetworkStateMachine (
    void
    )
{
    int link;

    if (m_cyw43Initialized && !m_forceRadioRestart && IsWifiUsable()) {
        if (m_netState == NetState::NoWifi) {
            if (HasSubnetConflict()) {
                EnterNoWifiState("home network overlaps hotspot subnet");
            } else {
                EnterWifiNoTcpState();
            }
        } else {
            CheckWifiHealth();
        }
    } else if (m_netState != NetState::NoWifi) {
        EnterNoWifiState("link/ip lost");
    }

    if (m_stationConnecting) {
        cyw43_arch_lwip_begin();
        link = cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA);
        cyw43_arch_lwip_end();
        if (link < CYW43_LINK_DOWN) {
            printf("[wifi] association failed link=%s\n", GetCyw43StatusName(link));
            EnterNoWifiState("association failed");
        } else if (absolute_time_diff_us(get_absolute_time(), m_stationDeadline) <= 0) {
            EnterNoWifiState("association/IP timeout");
        }
    } else if (m_netState == NetState::NoWifi &&
        absolute_time_diff_us(get_absolute_time(), m_nextWifiAttemptTime) <= 0) {
        RunWifiConnectSequence();
    }

    if (!m_cyw43Initialized) {
        return;
    }
    if (!m_hotspot.IsReady() &&
        absolute_time_diff_us(get_absolute_time(), m_nextHotspotAttemptTime) <= 0) {
        m_nextHotspotAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        if (!m_hotspot.Start()) {
            printf("[hotspot] unavailable; retry=%u ms\n", WIFI_RETRY_INTERVAL_MS);
        }
        // AP setup changes lwIP's default netif; keep the home interface as the default route.
        cyw43_arch_lwip_begin();
        netif_set_default(StationInterface());
        cyw43_arch_lwip_end();
    }
    EnsureNetworkServices();
    m_hotspot.UpdateClients(to_ms_since_boot(get_absolute_time()));
} // UpdateNetworkStateMachine

/**
    Reports whether the shared controller services are locally accessible.

    @return true - Listeners are ready through the hotspot or verified home Wi-Fi.
    @return false - No usable local interface or complete listener set is available.
*/
bool
WifiConnection::IsNetworkReady (
    void
    ) const
{
    return m_servicesReady && (m_hotspot.IsReady() ||
        (m_netState == NetState::Ready && m_reachabilityVerified && !m_arpFailStreak));
} // IsNetworkReady

/**
    Returns the readable current network state.

    @return "hotspot" - Direct controller access is ready without home Wi-Fi.
    @return "hotspot-connecting" - Direct access is ready while home Wi-Fi connects.
    @return "connecting" - The home association is pending without ready direct access.
    @return "no-wifi" - Neither network path has reached readiness.
    @return "wifi-no-tcp" - Home Wi-Fi is up but listeners are pending.
    @return "ready" - Home Wi-Fi and shared listeners are verified.
    @return "ready-unverified" - Home listeners are up while reachability is checked.
*/
const char*
WifiConnection::GetNetStateName (
    void
    ) const
{
    if (m_netState == NetState::NoWifi && m_servicesReady && m_hotspot.IsReady()) {
        if (m_stationConnecting) {
            return "hotspot-connecting";
        }
        return "hotspot";
    }
    return GetHomeWifiStateName();
} // GetNetStateName

/**
    Returns the home Wi-Fi state independently of hotspot availability.

    Reports station association, listener availability and path verification from
    the network worker's current state. A ready hotspot does not conceal a pending
    or failed home connection. This accessor performs no radio or transport work.

    @return "connecting" - Home association or address acquisition is pending.
    @return "no-wifi" - Home Wi-Fi has no usable association/address.
    @return "wifi-no-tcp" - Home Wi-Fi has an address while listeners are pending.
    @return "ready" - Home Wi-Fi, listeners and path verification are ready.
    @return "ready-unverified" - Home listeners are up while the path is checked.
*/
const char*
WifiConnection::GetHomeWifiStateName (
    void
    ) const
{
    if (m_netState == NetState::NoWifi) {
        if (m_stationConnecting) {
            return "connecting";
        }
        return "no-wifi";
    }
    if (!m_servicesReady) {
        return "wifi-no-tcp";
    }
    if (m_reachabilityVerified && !m_arpFailStreak) {
        return "ready";
    }
    return "ready-unverified";
} // GetHomeWifiStateName

/**
    Returns the hosted network state independently of home Wi-Fi availability.

    @return "unavailable" - The Pico access point or DHCP server is not running.
    @return "services-pending" - The access point is running while listeners start.
    @return "ready" - The hosted interface and shared controller listeners are ready.
*/
const char*
WifiConnection::GetHotspotStateName (
    void
    ) const
{
    if (!m_hotspot.IsReady()) {
        return "unavailable";
    }
    if (!m_servicesReady) {
        return "services-pending";
    }
    return "ready";
} // GetHotspotStateName

/**
    Returns the preferred local controller address.

    @return address - Home Wi-Fi address while its station interface is usable.
    @return WIFI_HOTSPOT_ADDRESS - Direct address when only the hotspot is usable.
    @return "-" - No local interface has an available address.
*/
const char*
WifiConnection::GetWifiIpText (
    void
    ) const
{
    if (m_netState != NetState::NoWifi && !m_wifiIpText.empty()) {
        return m_wifiIpText.c_str();
    }
    if (m_hotspot.IsReady()) {
        return WIFI_HOTSPOT_ADDRESS;
    }
    return "-";
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
    m_servicesReady = false;
    StopStationMonitoring();
    if (m_videoUpload) {
        m_videoUpload->Stop();
    }
    WebServer::Stop();
    if (m_frameBufferController) {
        m_frameBufferController->StopFrameUploadServer();
    }

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
    ip4addr_ntoa_r(netif_ip4_addr(StationInterface()), address, sizeof(address));
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

/**
    Checks for active clients before a disruptive station retry or radio reset.

    @return true - A photo, HTTP or video connection is currently in progress.
    @return false - Retrying the station will not interrupt an active controller client.
*/
bool
WifiConnection::HasActiveNetworkClient (
    void
    ) const
{
    if (!m_cyw43Initialized) {
        return false;
    }
    return (m_frameBufferController && m_frameBufferController->IsFrameUploadInProgress()) ||
        WebServer::HasActiveClient() || (m_videoUpload && m_videoUpload->HasActiveClient());
} // HasActiveNetworkClient

/**
    Cancels station-only probes without stopping shared client listeners.

    Removes the input wrapper before the SDK can destroy the station netif. The
    diagnostic TCP PCB is detached and aborted under the lwIP lock; AP clients,
    DHCP leases and upload listeners retain their own independent lifetimes.
*/
void
WifiConnection::StopStationMonitoring (
    void
    )
{
    if (!m_cyw43Initialized) {
        return;
    }
    StopInputMonitor();
    cyw43_arch_lwip_begin();
    if (m_gatewayProbePcb) {
        tcp_arg(m_gatewayProbePcb, nullptr);
        tcp_err(m_gatewayProbePcb, nullptr);
        tcp_abort(m_gatewayProbePcb);
        m_gatewayProbePcb = nullptr;
    }
    m_arpPending = m_arpReply = m_inboundSeen = false;
    m_arpFailStreak = m_gatewayProbeFailStreak = 0;
    m_reachabilityVerified = false;
    cyw43_arch_lwip_end();
} // StopStationMonitoring

/**
    Detects overlapping home and hotspot subnets before publishing a home address.

    Two local interfaces cannot route the same address range reliably. The AP's
    fixed /24 and the router's DHCP mask are both checked, so a broader home mask
    is detected too. The station is rejected while direct hotspot access remains.

    @return true - The home and direct networks overlap.
    @return false - The two IPv4 address ranges are disjoint.
*/
bool
WifiConnection::HasSubnetConflict (
    void
    ) const
{
    ip4_addr_t address;
    ip4_addr_t mask;
    bool conflict;

    cyw43_arch_lwip_begin();
    ip4addr_aton(WIFI_HOTSPOT_ADDRESS, &address);
    ip4addr_aton(WIFI_HOTSPOT_NETMASK, &mask);
    conflict = ip4_addr_netcmp(&address, netif_ip4_addr(StationInterface()), &mask) ||
        ip4_addr_netcmp(&address, netif_ip4_addr(StationInterface()),
            netif_ip4_netmask(StationInterface()));
    cyw43_arch_lwip_end();
    return conflict;
} // HasSubnetConflict

/**
    Starts missing shared listeners while either network interface is available.

    All services bind to any local address and validate HTTP origins against the
    receiving connection's local IP. Listener creation still uses the existing
    foreground handoff. A home connection appearing or disappearing does not
    recreate an already complete listener set or cancel an AP-side transaction.
*/
void
WifiConnection::EnsureNetworkServices (
    void
    )
{
    bool photoReady;
    bool httpReady;
    bool videoReady;
    bool previouslyReady;

    previouslyReady = m_servicesReady;
    photoReady = m_frameBufferController && m_frameBufferController->IsFrameUploadServerListening();
    httpReady = WebServer::IsListening();
    videoReady = !m_videoUpload || m_videoUpload->IsListening();
    m_servicesReady = photoReady && httpReady && videoReady;

    if (!m_servicesReady && (m_hotspot.IsReady() || m_netState != NetState::NoWifi) &&
        absolute_time_diff_us(get_absolute_time(), m_nextBindAttemptTime) <= 0) {
        m_nextBindAttemptTime = make_timeout_time_ms(TCP_BIND_RETRY_INTERVAL_MS);
        if (m_beginServices) {
            m_beginServices(m_servicesContext);
        }
        if (!photoReady && m_frameBufferController) {
            photoReady = m_frameBufferController->StartFrameUploadServer(IMAGE_UPLOAD_PORT);
        }
        if (!httpReady && m_frameBufferController) {
            httpReady = WebServer::Start(*m_frameBufferController);
        }
        if (!videoReady && m_videoUpload) {
            videoReady = m_videoUpload->Start();
        }
        if (m_endServices) {
            m_endServices(m_servicesContext);
        }
        m_servicesReady = photoReady && httpReady && videoReady;
    }

    if (m_servicesReady && !previouslyReady) {
        printf("[net] listeners ready http=80 tcp=5001,5002\n");
    }
    if (m_netState != NetState::NoWifi && m_servicesReady) {
        m_netState = NetState::Ready;
    }
} // EnsureNetworkServices

/**
    Returns the home Wi-Fi interface independently of lwIP's default route.

    @return interface - Station netif owned by the initialized CYW43 driver.
*/
netif*
WifiConnection::StationInterface (
    void
    )
{
    return &cyw43_state.netif[CYW43_ITF_STA];
} // StationInterface

/**
    Reports whether verified home Wi-Fi and its shared listeners are ready.

    @return true - Home association, IPv4, listeners and path verification are ready.
    @return false - The home path is disconnected, unverified or missing listeners.
*/
bool
WifiConnection::IsHomeWifiReady (
    void
    ) const
{
    return m_servicesReady && m_netState == NetState::Ready &&
        m_reachabilityVerified && !m_arpFailStreak;
} // IsHomeWifiReady

/**
    Reports whether the hosted network and its shared listeners are ready.

    @return true - The hotspot, DHCP server and controller listeners are running.
    @return false - The hotspot or its controller listeners are unavailable.
*/
bool
WifiConnection::IsHotspotReady (
    void
    ) const
{
    return m_servicesReady && m_hotspot.IsReady();
} // IsHotspotReady

/**
    Returns only the home Wi-Fi address without a hotspot fallback.

    @return address - Home interface's assigned IPv4 text while associated.
    @return "-" - The home interface has no usable assigned address.
*/
const char*
WifiConnection::GetHomeWifiIpText (
    void
    ) const
{
    if (m_netState != NetState::NoWifi && !m_wifiIpText.empty()) {
        return m_wifiIpText.c_str();
    }
    return "-";
} // GetHomeWifiIpText

/**
    Returns only the hosted network address without a home Wi-Fi fallback.

    @return WIFI_HOTSPOT_ADDRESS - The hosted interface's fixed IPv4 address.
    @return "-" - The hosted interface has not started or is unavailable.
*/
const char*
WifiConnection::GetHotspotIpText (
    void
    ) const
{
    if (m_hotspot.IsReady()) {
        return WIFI_HOTSPOT_ADDRESS;
    }
    return "-";
} // GetHotspotIpText

/**
    Copies the hotspot's stable per-device display states.

    @param states Destination with WIFI_HOTSPOT_MAX_CLIENTS entries.
*/
void
WifiConnection::CopyHotspotClientStates (
    uint8_t* states
    ) const
{
    m_hotspot.CopyClientStates(states, to_ms_since_boot(get_absolute_time()));
} // CopyHotspotClientStates
