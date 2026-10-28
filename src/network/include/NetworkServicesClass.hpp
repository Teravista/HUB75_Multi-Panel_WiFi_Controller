#pragma once
#include <atomic>
#include "network/include/WifiConnectionClass.hpp"
#include "config/defines.h"


/**
    Coordinates Wi-Fi lifecycle and upload transports across the two cores.

    Core 1 advances the connection state and pumps TCP/HTTP transport work. When listeners must
    pause or resume, a command mailbox asks Core 0 to finish its current foreground operation and
    acknowledge a safe boundary. A small synchronized status snapshot supplies the display with
    coherent connection and IP data. Core launch belongs to networkCorePico.cpp; SD I/O
    and library mutations remain on Core 0. Network recovery never changes the SD
    session. Idle passes also report whether the
    shared scheduler may accept a decoder reservation.
*/
class NetworkServices {
public:
    enum class Command : unsigned { None, PauseServices, ResumeServices };

    // The foreground owns the context until ProcessRequests returns.
    struct Handler {
        void* context;
        void (*handle)(void* context, Command command);
    };

    struct Status {
        bool ready = false;
        bool homeReady = false;
        bool hotspotReady = false;
        uint8_t hotspotDevices[WIFI_HOTSPOT_MAX_CLIENTS]{};
        char homeState[NETWORK_STATE_TEXT_BYTES] = "no-wifi";
        char hotspotState[NETWORK_STATE_TEXT_BYTES] = "unavailable";
        char homeIp[IPV4_TEXT_BYTES] = "-";
        char hotspotIp[IPV4_TEXT_BYTES] = "-";
        char state[NETWORK_STATE_TEXT_BYTES] = "no-wifi";
        char ip[IPV4_TEXT_BYTES] = "-";
    };

    using Yield = void (*)(void);
    // Connects Wi-Fi service transitions to foreground transport handoffs.
    NetworkServices(WifiConnection& wifi, Yield yield, TcpVideoUpload* video = nullptr);

    // Removes lifecycle callbacks after the network worker has stopped.
    ~NetworkServices(void);

    // Initializes Wi-Fi retry timing and publishes the initial network status.
    void Initialize(void);

    // Advances network connection, service handoffs, and transport maintenance.
    void Update(void);

    // Handles and acknowledges a pending transport lifecycle handoff.
    bool ProcessRequests(Handler handler);

    // Copies a coherent network state and IP snapshot for the display core.
    Status ReadStatus(void);

    // Checks whether network work permits an idle Core 1 decode job.
    bool CanDecodeOnCore1(void);


private:
    WifiConnection& wifi_;
    Yield yield_;
    TcpVideoUpload* video_;
    std::atomic<Command> command_{Command::None};
    std::atomic_flag statusLock_ = ATOMIC_FLAG_INIT;
    Status status_;
    // Publishes a service handoff and waits for foreground acknowledgement.
    void Request(Command command);

    // Publishes both network interfaces and aggregate status as one coherent snapshot.
    void PublishStatus(bool suspended = false);

};
