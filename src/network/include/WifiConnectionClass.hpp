#pragma once
class TcpVideoUpload;

#include <string>
#include "pico/types.h"
#include "lwip/tcp.h"
#include "lwip/netif.h"

#include "network/include/FrameBufferControllerClass.hpp"
#include "network/include/PicoHotspotClass.hpp"

/**
    Maintains local Wi-Fi connectivity and the availability of controller services.

    It runs the direct Pico hotspot alongside asynchronous home Wi-Fi association,
    address acquisition and recovery. Both interfaces share one HTTP/photo/video
    listener set. Station health observes only the home interface; normal home
    retries retain AP clients and DHCP leases. Lifecycle callbacks coordinate
    listener changes with Core 0, while status feeds the panel overlay. Internet
    forwarding is not provided, and network recovery leaves the SD session intact.
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

    // Home-interface lifecycle; the hotspot and shared listeners are independent.
    //   NoWifi     - home association or address is unavailable
    //   WifiNoTcp  - home address acquired, shared listeners pending
    //   Ready      - home address and shared listeners available
    enum class NetState {
        NoWifi,
        WifiNoTcp,
        Ready,
    };

    // Initializes connection, retry, and health-check deadlines.
    void InitializeNetworkTiming(void);

    // Advances home Wi-Fi, the direct hotspot and shared listener recovery.
    void UpdateNetworkStateMachine(void);

    // Reports whether the shared controller services are locally accessible.
    bool IsNetworkReady(void) const;

    // Returns the readable current network state.
    const char* GetNetStateName(void) const;

    // Returns the home Wi-Fi state independently of hotspot availability.
    const char* GetHomeWifiStateName(void) const;

    // Returns the hosted network state independently of home Wi-Fi availability.
    const char* GetHotspotStateName(void) const;

    // Reports whether verified home Wi-Fi and its shared listeners are ready.
    bool IsHomeWifiReady(void) const;

    // Reports whether the hosted network and its shared listeners are ready.
    bool IsHotspotReady(void) const;

    // Copies the hotspot's stable per-device display states.
    void CopyHotspotClientStates(uint8_t* states) const;

    // Returns only the home Wi-Fi address without a hotspot fallback.
    const char* GetHomeWifiIpText(void) const;

    // Returns only the hosted network address without a home Wi-Fi fallback.
    const char* GetHotspotIpText(void) const;

    // Returns the preferred local controller address.
    const char* GetWifiIpText(void) const;

private:
    FrameBufferController* m_frameBufferController = nullptr;
    TcpVideoUpload* m_videoUpload = nullptr;

    NetState m_netState = NetState::NoWifi;
    std::string m_wifiIpText;
    bool m_cyw43Initialized = false;
    bool m_servicesReady = false;
    bool m_stationConnecting = false;
    PicoHotspot m_hotspot;
    absolute_time_t m_stationDeadline{};
    absolute_time_t m_nextHotspotAttemptTime{};
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

    // Stops station monitoring and schedules a home Wi-Fi reconnect attempt.
    void EnterNoWifiState(const char* reason, bool restartRadio = false);

    // Stops upload and HTTP services before a network transition.
    void StopNetworkServices(void);

    // Returns the home Wi-Fi interface independently of lwIP's default route.
    static netif* StationInterface(void);

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

    // Initializes the radio and starts a nonblocking home Wi-Fi association.
    void RunWifiConnectSequence(void);

    // Starts missing shared listeners while either network interface is available.
    void EnsureNetworkServices(void);

    // Cancels station-only probes without stopping shared client listeners.
    void StopStationMonitoring(void);

    // Checks for active clients before a disruptive station retry or radio reset.
    bool HasActiveNetworkClient(void) const;

    // Detects overlapping home and hotspot subnets before publishing a home address.
    bool HasSubnetConflict(void) const;

    // Detects lost station links or addresses and schedules recovery.
    void CheckWifiHealth(void);

};
