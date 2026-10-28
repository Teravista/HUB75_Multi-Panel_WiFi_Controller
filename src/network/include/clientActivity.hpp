#pragma once
struct tcp_pcb;

namespace ClientActivity {

// Installs a borrowed activity observer while the lwIP context is serialized.
void SetObserver(void (*observer)(void* context, const tcp_pcb* client), void* context);

// Reports meaningful application traffic from a serialized transport callback.
void RecordCommunication(const tcp_pcb* client);

} // namespace ClientActivity
