#include "network/include/NetworkServicesClass.hpp"
#include "http/include/webServer.hpp"
#include "network/include/TcpVideoUploadClass.hpp"
#include <cstring>

/**
    Connects Wi-Fi service transitions to foreground storage handoffs.

    @param wifi Wi-Fi connection and local-service state controller.
    @param yield Callback allowing the worker to yield while waiting for foreground work.
    @param video Optional TCP video transport maintained by the network worker.
*/
NetworkServices::NetworkServices (
    WifiConnection& wifi,
    Yield yield,
    TcpVideoUpload* video
    ) : wifi_(wifi), yield_(yield), video_(video)
{
    wifi_.SetServiceLifecycle(
        [](void* context) { static_cast<NetworkServices*>(context)->Request(Command::PauseServices); },
        [](void* context) { static_cast<NetworkServices*>(context)->Request(Command::ResumeServices); }, this);
    wifi_.SetBeforeRadioRestart(
        [](void* context) {
            NetworkServices* worker = nullptr;
            worker = static_cast<NetworkServices*>(context);
            worker->PublishStatus(true);
            worker->Request(Command::SuspendRadio);
        },
        this);
    wifi_.SetAfterRadioInitialization(
        [](void* context, bool initialized) {
            NetworkServices* worker = nullptr;
            worker = static_cast<NetworkServices*>(context);
            if (initialized) {
                worker->Request(Command::RadioReady);
            } else {
                worker->Request(Command::RadioFailed);
            }
        },
        this);
} // NetworkServices

/**
    Removes lifecycle callbacks after the network worker has stopped.
*/
NetworkServices::~NetworkServices (
    void
    )
{
    wifi_.SetServiceLifecycle(nullptr, nullptr, nullptr);
    wifi_.SetBeforeRadioRestart(nullptr, nullptr);
    wifi_.SetAfterRadioInitialization(nullptr, nullptr);
} // ~NetworkServices

/**
    Initializes Wi-Fi retry timing and publishes the initial network status.
*/
void
NetworkServices::Initialize (
    void
    )
{
    wifi_.InitializeNetworkTiming();
    PublishStatus();
} // Initialize

/**
    Publishes a service handoff and waits for foreground acknowledgement.

    Publishes a pause/resume command and yields until Core 0 acknowledges that it has handled the
    safe foreground boundary. No shared status lock is held during this wait. The handler must
    continue being polled on Core 0; the protocol is for listener lifecycle transitions, not for
    waiting through Wi-Fi association. Radio initialization has explicit
    suspend/ready commands before the blocking association call begins.

    @param command Controller command exchanged with the foreground worker.
*/
void
NetworkServices::Request (
    Command command
    )
{
    command_.store(command, std::memory_order_release);
    // Acknowledge the brief lifecycle boundary before radio or listener work continues.
    while (command_.load(std::memory_order_acquire) != Command::None) {
        yield_();
    }
} // Request

/**
    Publishes the latest network state and IP as one coherent snapshot.

    Captures connection readiness and address text into a local snapshot before acquiring the
    short publication lock. A suspended snapshot clears foreground-facing readiness while radio
    services restart. The lock protects only the bounded copy; radio initialization and filesystem
    operations must remain outside it.

    @param suspended Whether foreground-facing network status should be marked suspended.
*/
void
NetworkServices::PublishStatus (
    bool suspended
    )
{
    Status next;

    next = {};
    if (!suspended) {
        next.ready = wifi_.IsNetworkReady();
        std::strncpy(next.state, wifi_.GetNetStateName(), sizeof(next.state) - 1);
        std::strncpy(next.ip, wifi_.GetWifiIpText(), sizeof(next.ip) - 1);
    }
    while (statusLock_.test_and_set(std::memory_order_acquire)) {
        yield_();
    }

    status_ = next;
    statusLock_.clear(std::memory_order_release);
} // PublishStatus

/**
    Copies a coherent network state and IP snapshot for the display core.

    Waits by yielding only while the short status-copy lock is owned by the network producer, then
    returns a complete snapshot. Readers cannot observe an IP string from a different connection
    state. The caller uses the returned copy without holding a cross-core lock during rendering or
    storage work.

    @return status - Consistent snapshot of the worker-published network status.
*/
NetworkServices::Status
NetworkServices::ReadStatus (
    void
    )
{
    Status result;

    while (statusLock_.test_and_set(std::memory_order_acquire)) {
        yield_();
    }
    result = status_;
    statusLock_.clear(std::memory_order_release);
    return result;
} // ReadStatus



/**
    Advances network connection, service handoffs, and transport maintenance.

    Runs on the shared network core and advances Wi-Fi lifecycle before transport maintenance.
    When the radio is initialized, video and HTTP reply/timeout work can progress; storage-side
    upload writes are handled separately on Core 0. A connection attempt or lifecycle handoff may
    wait inside the called operations, so the foreground must keep acknowledging its bounded mailbox requests.
*/
void
NetworkServices::Update (
    void
    )
{
    wifi_.UpdateNetworkStateMachine();
    if (wifi_.IsRadioInitialized()) {
        if (video_) {
            video_->UpdateTransport();
        }
        WebServer::UpdateTransport();
    }
    PublishStatus();
} // Update

/**
    Handles and acknowledges a pending storage-side service handoff.

    Reads the pending service command with acquire ordering and invokes the supplied foreground
    handler before publishing acknowledgement. Listener changes may resume on Core 1 only after
    that handler has returned and cleared the command. There is no work or wait when the mailbox
    is empty.

    @param handler Foreground callback that handles one queued network command.

    @return true - A pending foreground command was handled.
    @return false - No foreground command was pending.
*/
bool
NetworkServices::ProcessRequests (
    Handler handler
    )
{
    NetworkServices::Command request;

    request = command_.load(std::memory_order_acquire);
    if (request == Command::None) {
        return false;
    }
    handler.handle(handler.context, request);
    command_.store(Command::None, std::memory_order_release);
    return true;
} // ProcessRequests

/**
    Checks whether network work permits an idle Core 1 decode job.

    @return true - Network activity currently allows the shared core to decode.
    @return false - A network client or network lifecycle operation needs the shared core.
*/
bool
NetworkServices::CanDecodeOnCore1 (
    void
    )
{
    if (!wifi_.IsRadioInitialized()) {
        return true; // Idle between failed-radio retries.
    }
    return !wifi_.IsPhotoUploadInProgress() && !WebServer::HasActiveClient() &&
           (!video_ || !video_->HasActiveClient());
} // CanDecodeOnCore1
