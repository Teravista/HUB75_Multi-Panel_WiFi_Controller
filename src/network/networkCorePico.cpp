#include "network/include/networkCore.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "config/defines.h"
#include "hub75.hpp"

static_assert(!HUB75_MULTICORE, "Application networking owns Core 1; HUB75 must use Core 0");

namespace {
NetworkServices* networkServices = nullptr;

/**
    Runs connection management and upload transport maintenance on Core 1.

    Wi-Fi association may wait inside the SDK. Core 0 remains available for storage
    and rendering, apart from acknowledged listener/radio handoff boundaries.
*/
void
RunNetworkCore (
    void
    )
{
    networkServices->Initialize();
    for (;;) {
        networkServices->Update();
        sleep_ms(UPLOAD_POLL_DELAY_1MS);
    }
} // RunNetworkCore

} // namespace

/**
    Launches the network worker on Core 1 exactly once.

    @param worker Long-lived worker whose referenced services outlive the core loop.
*/
void
StartNetworkCore (
    NetworkServices& worker
    )
{
    if (networkServices) {
        panic("Network core already launched");
    }
    networkServices = &worker;
    multicore_launch_core1(RunNetworkCore);
} // StartNetworkCore
