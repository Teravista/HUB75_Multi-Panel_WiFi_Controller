#pragma once

#include <string>
#include "pico/types.h"
#include "lwip/tcp.h"

#include "networkUpload.hpp"

class WifiNetwork {
public:
    WifiNetwork() = default;
    explicit WifiNetwork(NetworkUpload *networkUpload);

    // Three-state network lifecycle:
    //   NoWifi     - no usable WiFi link; any previous listener has been destroyed
    //   WifiNoTcp  - WiFi up, listener not bound yet (retrying the bind)
    //   Ready      - WiFi up and listener bound
    enum class NetState {
        NoWifi,
        WifiNoTcp,
        Ready,
    };

    /*
        starts the WiFi state machine timing. Must be called before UpdateNetworkStateMachine().
    */
    void InitializeNetworkTiming();

    /*
        Advances WiFi connection, health, and upload-listener state if possible.
    */
    void UpdateNetworkStateMachine();

    /*
        True when WiFi and the upload listener are ready.
    */
    bool IsNetworkReady() const;

    /*
        get human readable string for debug purposes for the current network state.
    */
    const char *GetNetStateName() const;

    /*
        Returns current WiFi IP text, or "-" when no IP address is available.
    */
    const char *GetWifiIpText() const;

private:
    NetworkUpload *m_networkUpload = nullptr;

    NetState m_netState = NetState::NoWifi;
    std::string m_wifiIpText;
    bool m_cyw43Initialized = false;

    absolute_time_t m_nextWifiAttemptTime{};
    absolute_time_t m_nextBindAttemptTime{};
    absolute_time_t m_nextHealthCheckTime{};
    uint32_t m_rssiProbeFailStreak = 0;
    tcp_pcb *m_gatewayProbePcb = nullptr;
    absolute_time_t m_gatewayProbeDeadline{};
    absolute_time_t m_nextGatewayProbeTime{};
    uint32_t m_gatewayProbeFailStreak = 0;

    /*
        Returns a human-readable name for a CYW43 link status code.
    */
    static const char *GetCyw43StatusName(int status);

    /*
        Returns true if the device has a valid IPv4 address.
    */
    static bool HasIpv4Address();

    /*
        Returns true if the WiFi link is up and we have a valid IPv4 address.
    */
    bool IsWifiUsable() const;

    /*
        Catches a silently vanished AP when the driver still reports JOIN.
    */
    static bool IsRadioResponding();

    /*
        Enter NoWifi state, stopping the upload listener and scheduling a retry.
    */
    void EnterNoWifiState(const char *reason);

    /*
        Handles errors reported by the gateway probe.
        An error does not always indicate failure because the gateway may respond with a RST or close the connection.
    */
    static void OnGatewayProbeErr(void *arg, err_t err);

    /*
        Callback for when the gateway probe successfully connects.
    */
    static err_t OnGatewayProbeConnected(void *arg, tcp_pcb *pcb, err_t err);

    /*
        Begin the process of starting new gateway probe
    */
    void StartGatewayProbe();

    /*
        Periodically sends traffic to the gateway and checks for a response.
        This both confirms that the WiFi path is alive and may prevent the AP or
        router from silently dropping an otherwise idle connection.
    */
    void UpdateGatewayProbe();

    /*
        Runs the WiFi connect sequence, attempting to connect to the configured AP.
        Successful sequence enters WifiNoTcp state, where the TCP listener can be bound. Failure no state change so NoWifi state is active.
    */
    void RunWifiConnectSequence();

    /*
        Checks WiFi health and enters NoWifi State after failures.
    */
    void CheckWifiHealth();
};
