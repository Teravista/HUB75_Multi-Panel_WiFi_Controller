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
    and library mutations remain on Core 0.
*/
class NetworkServices {
public:
    enum class Command : unsigned { None, PauseServices, ResumeServices, SuspendRadio, RadioReady, RadioFailed };

    // The foreground owns the context until ProcessRequests returns.
    struct Handler {
        void* context;
        void (*handle)(void* context, Command command);
    };

    struct Status {
        bool ready = false;
        char state[NETWORK_STATE_TEXT_BYTES] = "no-wifi";
        char ip[IPV4_TEXT_BYTES] = "-";
    };

    using Yield = void (*)(void);
    // Connects Wi-Fi service transitions to foreground storage handoffs.
    NetworkServices(WifiConnection& wifi, Yield yield, TcpVideoUpload* video = nullptr);

    // Removes lifecycle callbacks after the network worker has stopped.
    ~NetworkServices(void);

    // Initializes Wi-Fi retry timing and publishes the initial network status.
    void Initialize(void);

    // Advances network connection, service handoffs, and transport maintenance.
    void Update(void);

    // Handles and acknowledges a pending storage-side service handoff.
    bool ProcessRequests(Handler handler);

    // Copies a coherent network state and IP snapshot for the display core.
    Status ReadStatus(void);

private:
    WifiConnection& wifi_;
    Yield yield_;
    TcpVideoUpload* video_;
    std::atomic<Command> command_{Command::None};
    std::atomic_flag statusLock_ = ATOMIC_FLAG_INIT;
    Status status_;
    // Publishes a service handoff and waits for foreground acknowledgement.
    void Request(Command command);

    // Publishes the latest network state and IP as one coherent snapshot.
    void PublishStatus(bool suspended = false);

};
