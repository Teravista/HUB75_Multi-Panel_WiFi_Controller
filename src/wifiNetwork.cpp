#include "wifiNetwork.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

#include "pico/cyw43_arch.h"
#include "cyw43.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"

#include "defines/defines.h"
#include "networkUpload.hpp"
#include "networkConfig.h"

WifiNetwork::WifiNetwork(NetworkUpload *networkUpload)
    : m_networkUpload(networkUpload)
{
}

/*
    Returns a human-readable name for a CYW43 link status code.
*/
const char *WifiNetwork::GetCyw43StatusName(int status)
{
    switch (status)
    {
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
}

/*
    Returns true if the device has a valid IPv4 address.
*/
bool WifiNetwork::HasIpv4Address()
{
    if (netif_default == nullptr) {
        return false;
    }

    cyw43_arch_lwip_begin();
    const ip4_addr_t ipAddr = *netif_ip4_addr(netif_default);
    cyw43_arch_lwip_end();
    return !ip4_addr_isany_val(ipAddr);
}

/*
    Returns true if the WiFi link is up and we have a valid IPv4 address.
*/
bool WifiNetwork::IsWifiUsable() const
{
    if (!m_cyw43Initialized) {
        return false;
    }

    // use cyw43 API to check if the link is up and we have an IP address
    return cyw43_wifi_link_status(&cyw43_state, CYW43_ITF_STA) > CYW43_LINK_DOWN &&
           HasIpv4Address();
}

/*
    Catches a silently vanished AP when the driver still reports JOIN.
*/
bool WifiNetwork::IsRadioResponding()
{
    int32_t rssi = 0;
    cyw43_arch_lwip_begin();
    const int ret = cyw43_wifi_get_rssi(&cyw43_state, &rssi);
    cyw43_arch_lwip_end();
    return ret == 0 && rssi != 0;
}

/*
    Enter NoWifi state, stopping the upload listener and scheduling a retry.
*/
void WifiNetwork::EnterNoWifiState(const char *reason)
{
    printf("[net] -> no-wifi (%s)\n", reason);
    // Cancel any in-flight gateway probe before tearing down WiFi. The probe
    // owns a lwIP PCB that must not survive CYW43 deinitialization.
    // Probe surviving creates issues later on.
    if (m_gatewayProbePcb != nullptr) {
        cyw43_arch_lwip_begin();
        tcp_arg(m_gatewayProbePcb, nullptr);
        tcp_abort(m_gatewayProbePcb);
        m_gatewayProbePcb = nullptr;
        cyw43_arch_lwip_end();
    }
    if (m_networkUpload != nullptr) {
        m_networkUpload->StopFrameUploadServer();
    }
    m_netState = NetState::NoWifi;
    m_wifiIpText.clear();
    m_rssiProbeFailStreak = 0;
    m_gatewayProbeFailStreak = 0;
    m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
}

/*
    Handles errors reported by the gateway probe.
    An error does not always indicate failure because the gateway may respond with a RST or close the connection.
*/
void WifiNetwork::OnGatewayProbeErr(void *arg, err_t err)
{
    auto *self = static_cast<WifiNetwork *>(arg);

    if (self == nullptr) {
        return;
    }

    self->m_gatewayProbePcb = nullptr;

    // Local abort and missing route provide no proof that the gateway or WiFi path responded.
    if (err == ERR_ABRT || err == ERR_RTE) {
        self->m_gatewayProbeFailStreak++;
        printf("[net] gateway probe failed (err=%d, failures=%u)\n",
               err, self->m_gatewayProbeFailStreak);
        return;
    }

    // A gateway RST or close still proves the TCP request reached the gateway
    // and a reply came back, even though port 53 did not accept the connection.
    self->m_gatewayProbeFailStreak = 0;
    printf("[net] gateway probe response (err=%d)\n", err);
}

/*
    Callback for when the gateway probe successfully connects.
*/
err_t WifiNetwork::OnGatewayProbeConnected(void *arg, tcp_pcb *pcb, err_t err)
{
    auto *self = static_cast<WifiNetwork *>(arg);

    if (self == nullptr) {
        tcp_abort(pcb);
        return ERR_OK;
    }

    self->m_gatewayProbePcb = nullptr;

    // If the connection attempt succeeded, the gateway is reachable so we still have a valid WiFi link.
    // Close the connection and reset the fail streak.
    if (err == ERR_OK) {
        self->m_gatewayProbeFailStreak = 0;
        printf("[net] gateway probe connected\n");
        tcp_close(pcb);
    }
    else {
        self->m_gatewayProbeFailStreak++;
        printf("[net] gateway probe failed (connect err=%d, failures=%u)\n",
               err, self->m_gatewayProbeFailStreak);
        tcp_abort(pcb);
    }

    return ERR_OK;
}

/*
    Begin the process of starting new gateway probe
*/
void WifiNetwork::StartGatewayProbe()
{
    ip4_addr_t gateway;
    tcp_pcb *pcb;
    err_t connectErr;

    if (m_gatewayProbePcb != nullptr || netif_default == nullptr) {
        return;
    }

    gateway = *netif_ip4_gw(netif_default);
    if (ip4_addr_isany_val(gateway)) {
        printf("[net] gateway probe skipped (no gateway)\n");
        return;
    }

    pcb = tcp_new();
    if (pcb == nullptr) {
        m_gatewayProbeFailStreak++;
        printf("[net] gateway probe allocation failed (failures=%u)\n", m_gatewayProbeFailStreak);
        return;
    }

    tcp_arg(pcb, this);
    tcp_err(pcb, OnGatewayProbeErr);
    m_gatewayProbePcb = pcb;
    m_gatewayProbeDeadline = make_timeout_time_ms(WIFI_GATEWAY_PROBE_TIMEOUT_MS);

        connectErr = tcp_connect(pcb, reinterpret_cast<const ip_addr_t *>(&gateway),
            WIFI_GATEWAY_PROBE_PORT, OnGatewayProbeConnected);

    if (connectErr != ERR_OK) {
        m_gatewayProbePcb = nullptr;
        m_gatewayProbeFailStreak++;
        printf("[net] gateway probe start failed (err=%d, failures=%u)\n",
             connectErr, m_gatewayProbeFailStreak);
        tcp_abort(pcb);
    }
}

/*
    Periodically sends traffic to the gateway and checks for a response.
    This both confirms that the WiFi path is alive and may prevent the AP or
    router from silently dropping an otherwise idle connection.
*/
void WifiNetwork::UpdateGatewayProbe()
{
    cyw43_arch_lwip_begin();

    const absolute_time_t now = get_absolute_time();
    const bool gatewayProbeExpired = absolute_time_diff_us(now, m_gatewayProbeDeadline) <= 0;
    const bool gatewayProbeDue = absolute_time_diff_us(now, m_nextGatewayProbeTime) <= 0;

    // Abort only an active probe whose connection attempt exceeded its deadline.
    if (m_gatewayProbePcb != nullptr && gatewayProbeExpired) {
        printf("[net] gateway probe timeout\n");
        m_gatewayProbeFailStreak++;
        tcp_arg(m_gatewayProbePcb, nullptr);
        tcp_abort(m_gatewayProbePcb);
        m_gatewayProbePcb = nullptr;
    }

    // Restart the timer for probe and attempt a new probe if none is active.
    if (gatewayProbeDue) {
        m_nextGatewayProbeTime = make_timeout_time_ms(WIFI_GATEWAY_PROBE_INTERVAL_MS);
        StartGatewayProbe();
    }

    cyw43_arch_lwip_end();
}

/*
    Runs the WiFi connect sequence, attempting to connect to the configured AP.
    Successful sequence enters WifiNoTcp state, where the TCP listener can be bound. Failure no state change so NoWifi state is active.
*/
void WifiNetwork::RunWifiConnectSequence()
{
    char macBuffer[18];
    int connectResult;
    const uint8_t *mac;

    printf("[net] connect sequence start\n");

    // Stop any existing listener and reset state before starting a new connection attempt.
    if (m_networkUpload != nullptr) {
        m_networkUpload->StopFrameUploadServer();
    }

    // if is already initialized then deinit for fresh state for clean connect sequence
    if (m_cyw43Initialized) {
        cyw43_arch_deinit();
        m_cyw43Initialized = false;
        sleep_ms(200);
    }

    if (cyw43_arch_init()) {
        printf("[wifi] cyw43_arch_init failed\n");
        m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        return;
    }
    m_cyw43Initialized = true;
    // enable client mode and connect to the configured WiFi network
    cyw43_arch_enable_sta_mode();

    mac = cyw43_state.mac;
    snprintf(macBuffer, sizeof(macBuffer), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    printf("[wifi] MAC %s, connecting to %s...\n", macBuffer, WIFI_SSID);
    connectResult = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        CYW43_AUTH_WPA2_AES_PSK,
        WIFI_CONNECT_TIMEOUT_MS);

    // check after the blocking wifi connect (non blocking connect is unstable)
    if (connectResult != 0 || !IsWifiUsable()) {
        printf("[wifi] connect failed, code=%d (%s)\n",
               connectResult,
               GetCyw43StatusName(connectResult));
        m_nextWifiAttemptTime = make_timeout_time_ms(WIFI_RETRY_INTERVAL_MS);
        return;
    }

    // Get the IP address for status output.
    cyw43_arch_lwip_begin();
    m_wifiIpText = ip4addr_ntoa(netif_ip4_addr(netif_default));
    cyw43_arch_lwip_end();

    printf("[net] -> wifi-no-tcp, ip=%s\n", m_wifiIpText.c_str());
    // WiFi is connected, but the frame upload server is not listening yet.
    m_netState = NetState::WifiNoTcp;
    m_rssiProbeFailStreak = 0;
    m_gatewayProbeFailStreak = 0;
    m_nextGatewayProbeTime = get_absolute_time();
    m_nextBindAttemptTime = get_absolute_time();
    m_nextHealthCheckTime = make_timeout_time_ms(WIFI_HEALTH_CHECK_INTERVAL_MS);
}

/*
    Checks WiFi health and enters NoWifi State after failures.
*/
void WifiNetwork::CheckWifiHealth()
{
    // to prevent spamming the serial output
    if (absolute_time_diff_us(get_absolute_time(), m_nextHealthCheckTime) < 0) {
        return;
    }
    m_nextHealthCheckTime = make_timeout_time_ms(WIFI_HEALTH_CHECK_INTERVAL_MS);

    if (!IsWifiUsable()) {
        EnterNoWifiState("link/ip lost");
        return;
    }

    // succseful response reset fail counter
    if (IsRadioResponding()) {
        m_rssiProbeFailStreak = 0;
    }
    else {
        m_rssiProbeFailStreak++;
        if (m_rssiProbeFailStreak >= RSSI_PROBE_FAIL_DEBOUNCE_COUNT) {
            EnterNoWifiState("radio not responding");
            return;
        }
    }

    UpdateGatewayProbe();
    if (m_gatewayProbeFailStreak >= WIFI_GATEWAY_PROBE_FAIL_DEBOUNCE_COUNT) {
        EnterNoWifiState("gateway probe failed");
    }
}

/*
    starts the WiFi state machine timing. Must be called before UpdateNetworkStateMachine().
*/
void WifiNetwork::InitializeNetworkTiming()
{
    m_nextWifiAttemptTime = get_absolute_time();
}

/*
    Advances WiFi connection, health, and upload-listener state if possible.
*/
void WifiNetwork::UpdateNetworkStateMachine()
{
    switch (m_netState) {
    case NetState::NoWifi:
        // Attempt to connect to WiFi if the retry interval has elapsed.
        if (absolute_time_diff_us(get_absolute_time(), m_nextWifiAttemptTime) <= 0){
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
            if (m_networkUpload != nullptr && m_networkUpload->StartFrameUploadServer(IMAGE_UPLOAD_PORT)) {
                printf("[net] -> ready\n");
                m_netState = NetState::Ready;
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
        if (m_networkUpload == nullptr || !m_networkUpload->IsFrameUploadServerListening()) {
            EnterNoWifiState("TCP listener unavailable");
        }
        break;
    }
}

/*
    True when WiFi and the upload listener are ready.
*/
bool WifiNetwork::IsNetworkReady() const
{
    return m_netState == NetState::Ready;
}

/*
    get human readable string for debug purposes for the current network state.
*/
const char *WifiNetwork::GetNetStateName() const
{
    switch (m_netState)
    {
    case NetState::NoWifi:
        return "no-wifi";
    case NetState::WifiNoTcp:
        return "wifi-no-tcp";
    case NetState::Ready:
        return "ready";
    }

    return "unknown";
}

/*
    Returns current WiFi IP text, or "-" when no IP address is available.
*/
const char *WifiNetwork::GetWifiIpText() const
{
    if (m_netState == NetState::NoWifi || m_wifiIpText.empty()) {
        return "-";
    }
    return m_wifiIpText.c_str();
}
