#pragma once
class TcpVideoUpload;

#include <string>
#include "pico/types.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#include "network/include/FrameBufferControllerClass.hpp"

/**
    Maintains local Wi-Fi connectivity and the availability of controller services.

    It advances association, address acquisition, listener startup and recovery states using
    configured retry deadlines. Link and radio checks are combined with local traffic and gateway
    reachability observations so diagnostic TCP probe failures do not unnecessarily interrupt
    working clients. Lifecycle callbacks coordinate listener changes with foreground code, and
    connection status feeds the panel icon and web Summary. It requires a usable local network,
    not Internet access, and Wi-Fi recovery leaves the independently managed SD session intact.
*/
class WifiConnection {
public:
    // Initializes an unbound Wi-Fi state controller.
    WifiConnection(void);

    // Connects Wi-Fi state management to photo and video network services.
    explicit WifiConnection(FrameBufferController* frameBufferController, TcpVideoUpload* videoUpload = nullptr);

    // Stops network services and releases radio monitoring resources.
    ~WifiConnection(void);

    // Disables copying so resource ownership cannot be duplicated.
    WifiConnection(const WifiConnection& other) = delete;

    // Disables copying of radio and network service ownership.
    WifiConnection& operator=(const WifiConnection& other) = delete;

    // Reports whether the photo transport is currently receiving a frame.
    bool IsPhotoUploadInProgress(void) const;

    // Reports whether the Wi-Fi radio has been initialized.
    bool IsRadioInitialized(void) const;

    // Registers a foreground handoff before radio reinitialization.
    void SetBeforeRadioRestart(void (*callback)(void* context), void* context);

    // Registers pause and resume callbacks for network service transitions.
    void SetServiceLifecycle(void (*begin)(void* context), void (*end)(void* context), void* context);

    // Three-state network lifecycle:
    //   NoWifi     - no usable WiFi link; any previous listener has been destroyed
    //   WifiNoTcp  - WiFi up, upload listeners not all bound yet
    //   Ready      - WiFi up and photo/HTTP/video listeners bound
    enum class NetState {
        NoWifi,
        WifiNoTcp,
        Ready,
    };

    // Initializes connection, retry, and health-check deadlines.
    void InitializeNetworkTiming(void);

    // Advances Wi-Fi connection, local services, and recovery states.
    void UpdateNetworkStateMachine(void);

    // Reports whether local services and configured reachability checks are ready.
    bool IsNetworkReady(void) const;

    // Returns the readable current network state.
    const char* GetNetStateName(void) const;

    // Returns the station's current IPv4 address text.
    const char* GetWifiIpText(void) const;

private:
    FrameBufferController* m_frameBufferController = nullptr;
    TcpVideoUpload* m_videoUpload = nullptr;

    NetState m_netState = NetState::NoWifi;
    std::string m_wifiIpText;
    bool m_cyw43Initialized = false;
    void (*m_beforeRadioRestart)(void* context) = nullptr;
    void* m_restartContext = nullptr;

    void (*m_beginServices)(void* context) = nullptr;
    void (*m_endServices)(void* context) = nullptr;
    void* m_servicesContext = nullptr;

    absolute_time_t m_nextWifiAttemptTime{};
    absolute_time_t m_nextBindAttemptTime{};
    absolute_time_t m_nextHealthCheckTime{};
    uint32_t m_rssiProbeFailStreak = 0;
    tcp_pcb* m_gatewayProbePcb = nullptr;
    absolute_time_t m_gatewayProbeDeadline{};
    absolute_time_t m_nextGatewayProbeTime{};
    uint32_t m_gatewayProbeFailStreak = 0;
    bool m_gatewayProbeWarningReported = false;
    // Observe replies on the real Ethernet input path, without consuming packets.
    static WifiConnection* s_inputMonitor;
    netif* m_monitoredInterface = nullptr;
    netif_input_fn m_originalInput = nullptr;
    absolute_time_t m_nextArpProbeTime{};
    absolute_time_t m_arpProbeDeadline{};
    uint32_t m_lastInboundMs = 0;
    uint32_t m_arpFailStreak = 0;
    bool m_arpPending = false;
    bool m_arpReply = false;
    bool m_inboundSeen = false;
    bool m_reachabilityVerified = false;
    bool m_forceRadioRestart = false;
    // Records received network traffic before forwarding the packet.
    static err_t ObserveNetworkInput(pbuf* packet, netif* interface);

    // Installs the network input monitor used by reachability checks.
    void StartInputMonitor(void);

    // Restores the original network input handler.
    void StopInputMonitor(void);

    // Updates local network reachability from received traffic and probes.
    void CheckReachability(void);

    // Returns a readable name for a Wi-Fi driver link state.
    static const char* GetCyw43StatusName(int status);

    // Returns a readable name for a Wi-Fi connection result.
    static const char* GetConnectResultName(int result);

    // Records the acquired IP and schedules local service startup.
    void EnterWifiNoTcpState(void);

    // Checks whether the station interface has a usable IPv4 address.
    static bool HasIpv4Address(void);

    // Checks whether the station link and IPv4 configuration are usable.
    bool IsWifiUsable(void) const;

    // Checks whether the Wi-Fi driver still reports an active radio.
    static bool IsRadioResponding(void);

    // Stops network services and schedules a Wi-Fi reconnect attempt.
    void EnterNoWifiState(const char* reason);

    // Stops upload and HTTP services before a network transition.
    void StopNetworkServices(void);

    // Updates the cached IPv4 address from the station interface.
    void RefreshWifiIpText(void);

    // Records a failed TCP gateway probe and releases its connection.
    static void OnGatewayProbeErr(void* arg, err_t err);

    // Records a successful gateway probe and closes its connection.
    static err_t OnGatewayProbeConnected(void* arg, tcp_pcb* pcb, err_t err);

    // Starts a bounded TCP reachability probe to the local gateway.
    void StartGatewayProbe(void);

    // Advances gateway probing and handles its timeout or retry.
    void UpdateGatewayProbe(void);

    // Initializes the radio and attempts station association and address acquisition.
    void RunWifiConnectSequence(void);

    // Detects lost station links or addresses and schedules recovery.
    void CheckWifiHealth(void);

};
