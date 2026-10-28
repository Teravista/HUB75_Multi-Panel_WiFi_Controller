#include "network/include/clientActivity.hpp"

namespace {
void* observerContext = nullptr;
void (*activityObserver)(void* context, const tcp_pcb* client) = nullptr;
} // namespace

/**
    Installs a borrowed activity observer while the lwIP context is serialized.

    The hotspot registers on startup and unregisters before stopping. Transport
    callbacks share that same serialized context, so no heap allocation or
    cross-core observer lock is required. A null observer makes reporting inert.

    @param observer Callback receiving meaningful traffic notifications, or null to detach.
    @param context Borrowed owner that remains alive while the observer is installed.
*/
void
ClientActivity::SetObserver (
    void (*observer)(void* context, const tcp_pcb* client),
    void* context
    )
{
    activityObserver = observer;
    observerContext = context;
} // SetObserver

/**
    Reports meaningful application traffic from a serialized transport callback.

    @param client Borrowed TCP connection identifying the local interface and remote device.
*/
void
ClientActivity::RecordCommunication (
    const tcp_pcb* client
    )
{
    if (client && activityObserver) {
        activityObserver(observerContext, client);
    }
} // RecordCommunication
