#include "network/include/networkCore.hpp"
#include "media/jpeg/include/jpegWorker.hpp"
static_assert(JPEG_PARALLEL_READER == 1, "Shared Core 1 requires the parallel JPEG input path");
#include "core/include/sharedCore1.hpp"
static_assert(!HUB75_MULTICORE, "The display driver must stay on Core 0");
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include <cstdio>

namespace {
NetworkServices* networkServices = nullptr;

/**
    Runs network work and available JPEG decode jobs on Core 1.

    Initializes network timing once, enables the shared scheduler, then repeatedly runs either a
    queued JPEG decode or a permitted network-services update. An idle network pass can offer Core
    1 to the decoder; a reservation suppresses new network foreground work while Core 0 prepares
    input. Short sleeps keep that wait yielding without entering a service handoff that could
    deadlock the foreground.
*/
void
NetworkCore (
    void
    )
{
    Core1Scheduler* scheduler = nullptr;

    networkServices->Initialize();
    scheduler = &(SharedCore1Scheduler());
    if (!scheduler->Enable()) {
        panic("Core 1 scheduler already enabled");
    }
    for (;;) {
        if (scheduler->RunDecode()) {
            continue;
        }
        if (scheduler->TryBeginNetwork()) {
            networkServices->Update();
            scheduler->OfferDecode(networkServices->CanDecodeOnCore1());
        }
        // During a reservation, wait for input preparation/submission on Core 0
        // without entering a network lifecycle that would wait for that same core.
        sleep_ms(UPLOAD_POLL_DELAY_1MS);
    }
} // NetworkCore

} // namespace

/**
    Launches the network worker on Core 1 exactly once.


    @param worker Network worker to launch on the second core.
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
    multicore_launch_core1(NetworkCore);
} // StartNetworkCore
