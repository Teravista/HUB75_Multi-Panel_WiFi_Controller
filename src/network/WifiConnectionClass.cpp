#include "network/include/WifiConnectionClass.hpp"
#include "network/include/TcpVideoUploadClass.hpp"
#include "http/include/webServer.hpp"
#include "config/defines.h"
#include "networkConfig.h"
#include "pico/cyw43_arch.h"
#include "pico/error.h"
#include "cyw43.h"
#include "lwip/netif.h"
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

#if HUB75_VERBOSE_LOGS
#define HUB75_DEBUG(...) std::printf(__VA_ARGS__)
#else
#define HUB75_DEBUG(...) ((void)0)
#endif

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
    Stops local listeners before the connection controller is destroyed.
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
    link, address and radio-response checks decide whether recovery is needed.
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
        HUB75_DEBUG("[net] gateway TCP probe timeout (diagnostic)\n");
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
        HUB75_DEBUG("[net] gateway probe unavailable; keeping upload services (TCP port diagnostic)\n");
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
    call can block up to the configured timeout; this work runs on the network core after the foreground radio/storage
    handoff has completed.

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
        if (m_afterRadioInitialization) {
            m_afterRadioInitialization(m_radioContext, false);
        }
        m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        return;
    }
    m_cyw43Initialized = true;
    HUB75_DEBUG("[wifi] radio initialized; enabling station mode\n");
    // enable client mode and connect to the configured WiFi network
    cyw43_arch_enable_sta_mode();
    if (m_afterRadioInitialization) {
        m_afterRadioInitialization(m_radioContext, true);
    }

    // Core 0 completes the storage handoff before the blocking association call.

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
    asynchronous TCP diagnostics. A failed link or debounced
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
    health checks and detects missing listeners. Radio lifecycle callbacks coordinate the foreground storage handoff before
    association waits begin.
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
    Reports whether the local Wi-Fi link and upload services are ready.

    @return true - The connection has an address and all local listeners are ready.
    @return false - Wi-Fi or local listeners are not ready.
*/
bool
WifiConnection::IsNetworkReady (
    void
    ) const
{
    return m_netState == NetState::Ready;
} // IsNetworkReady

/**
    Returns the readable current network state.

    Returns static text for the three connection/service states without advancing
    the state machine or borrowing mutable IP storage.

    @return "no-wifi" - No usable Wi-Fi connection is available.
    @return "wifi-no-tcp" - Wi-Fi has an IP address but local listeners are not all ready.
    @return "ready" - The connection and all configured upload listeners are ready.
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
    case NetState::Ready:
        return "ready";
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

    Runs on Core 1 after services stop and before radio deinitialization. The
    worker uses this boundary to request foreground cleanup and publish disconnected status.

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
    Registers a foreground handoff after radio initialization and before association.

    @param callback Callback receiving whether radio initialization succeeded.
    @param context Caller-owned state retained until the callback is detached.
*/
void
WifiConnection::SetAfterRadioInitialization (
    void (*callback)(void* context, bool initialized),
    void* context
    )
{
    m_afterRadioInitialization = callback;
    m_radioContext = context;
} // SetAfterRadioInitialization
