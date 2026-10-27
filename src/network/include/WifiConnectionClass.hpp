#pragma once
#include <string>
#include "pico/types.h"
#include "lwip/tcp.h"
#include "network/include/FrameBufferControllerClass.hpp"

class TcpVideoUpload;

/**
    Maintains the local Wi-Fi connection and the controller's upload listeners.

    Core 1 owns radio association, address acquisition, health checks and transport
    lifecycle. Callbacks bracket listener changes and radio initialization so Core 0
    can finish shared storage/frame operations at safe boundaries. Application code
    uses NetworkServices snapshots rather than reading this mutable state across cores.
*/
class WifiConnection {
public:
    enum class NetState { NoWifi, WifiNoTcp, Ready };

    // Initializes an unbound Wi-Fi state controller.
    WifiConnection(void);

    // Connects Wi-Fi state management to photo and video network services.
    explicit WifiConnection(FrameBufferController* frameBufferController, TcpVideoUpload* videoUpload = nullptr);

    // Stops local listeners before the connection controller is destroyed.
    ~WifiConnection(void);

    // Initializes connection, retry, and health-check deadlines.
    void InitializeNetworkTiming(void);

    // Advances Wi-Fi connection, local services, and recovery states.
    void UpdateNetworkStateMachine(void);

    // Reports whether the local Wi-Fi link and upload services are ready.
    bool IsNetworkReady(void) const;

    // Returns the readable current network state.
    const char* GetNetStateName(void) const;

    // Returns the station's current IPv4 address text.
    const char* GetWifiIpText(void) const;

    // Reports whether the Wi-Fi radio has been initialized.
    bool IsRadioInitialized(void) const;

    // Reports whether the photo transport is currently receiving a frame.
    bool IsPhotoUploadInProgress(void) const;

    // Registers a foreground handoff before radio reinitialization.
    void SetBeforeRadioRestart(void (*callback)(void* context), void* context);

    // Registers a foreground handoff after radio initialization and before association.
    void SetAfterRadioInitialization(void (*callback)(void* context, bool initialized), void* context);

    // Registers pause and resume callbacks for network service transitions.
    void SetServiceLifecycle(void (*begin)(void* context), void (*end)(void* context), void* context);

private:
    // Returns a readable name for a Wi-Fi driver link state.
    static const char* GetCyw43StatusName(int status);

    // Returns a readable name for a Wi-Fi connection result.
    static const char* GetConnectResultName(int result);

    // Checks whether the station interface has a usable IPv4 address.
    static bool HasIpv4Address(void);

    // Checks whether the station link and IPv4 configuration are usable.
    bool IsWifiUsable(void) const;

    // Checks whether the Wi-Fi driver still reports an active radio.
    static bool IsRadioResponding(void);

    // Stops network services and schedules a Wi-Fi reconnect attempt.
    void EnterNoWifiState(const char* reason);

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

    // Records the acquired IP and schedules local service startup.
    void EnterWifiNoTcpState(void);

    // Detects lost station links or addresses and schedules recovery.
    void CheckWifiHealth(void);

    // Stops upload and HTTP services before a network transition.
    void StopNetworkServices(void);

    // Updates the cached IPv4 address from the station interface.
    void RefreshWifiIpText(void);

    FrameBufferController* m_frameBufferController = nullptr;
    TcpVideoUpload* m_videoUpload = nullptr;
    NetState m_netState = NetState::NoWifi;
    std::string m_wifiIpText;
    bool m_cyw43Initialized = false;
    void (*m_beforeRadioRestart)(void* context) = nullptr;
    void* m_restartContext = nullptr;
    void (*m_afterRadioInitialization)(void* context, bool initialized) = nullptr;
    void* m_radioContext = nullptr;
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
    bool m_forceRadioRestart = false;
};
