// Bounded one-client HTTP transport; both upload paths share FrameBufferController.
#include "http/include/webServer.hpp"
#include "http/include/HttpMediaUploadClass.hpp"
#include "webPage.hpp"
#include "network/include/FrameBufferControllerClass.hpp"
#include "http/include/httpRequest.hpp"
#include "lwip/ip4_addr.h"
#include "pico/cyw43_arch.h"
#include "pico/time.h"
#include "lwip/tcp.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "config/defines.h"

#define HTTP_LISTEN_PORT 80
#define HTTP_RESPONSE_HEADER_BYTES 192
#define HTTP_SEND_CHUNK_BYTES 512
#define PHOTO_ACK_BYTES 4
#define STRING_TERMINATOR_BYTES 1

namespace {
constexpr size_t kHeaderCapacity = HttpRequest::kHeaderCapacity;
static_assert(SOURCE_FRAME_SIZE_BYTES == HttpRequest::kFrameBytes, "Page/firmware dimensions differ");
FrameBufferController* receiver = nullptr;
HttpMediaUpload* httpUpload = nullptr;
bool enabled = false;
constexpr size_t kFlightLimit = 1024; // Payload queued but not acknowledged.
constexpr uint32_t kIdleMs = 10000;
constexpr uint32_t kLifetimeMs = 30000;
tcp_pcb* listener = nullptr;

struct Client {
    tcp_pcb* pcb = nullptr;
    char request[kHeaderCapacity]{};
    char response[HTTP_RESPONSE_HEADER_BYTES]{};
    size_t used = 0;
    size_t headerLength = 0;
    size_t bodyLength = 0;
    size_t queued = 0;
    size_t acknowledged = 0;
    const char* body = nullptr;
    uint32_t started = 0;
    uint32_t progress = 0;
    bool responding = false;
    bool receiving = false;
    bool video = false;
    bool waiting = false;
    size_t expected = 0;
    FrameBufferController::Token owner = 0;
    size_t frameBytes = 0;
} client;

/**
    Returns elapsed boot time in milliseconds for transport deadlines.

    @return value - Monotonic time since boot in milliseconds, wrapping at the counter width.
*/
uint32_t
GetTransportTimeMs (
    void
    )
{
    return to_ms_since_boot(get_absolute_time());
} // GetTransportTimeMs

/**
    Accepts HTTP request bytes and advances request processing.

    @param context Unused caller context supplied by lwIP.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param packet Received lwIP packet chain; null signals that the peer closed the connection.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
ReceiveHttpRequestBytes (
    [[maybe_unused]] void* context,
    [[maybe_unused]] tcp_pcb* pcb,
    pbuf* packet,
    [[maybe_unused]] err_t error
    );

/**
    Advances the HTTP response after TCP acknowledges transmitted bytes.

    @param context Unused caller context supplied by lwIP.
    @param pcb Connection whose response or timeout is being serviced.
    @param length Number of bytes to process.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
HandleHttpResponseAcknowledgement (
    [[maybe_unused]] void* context,
    [[maybe_unused]] tcp_pcb* pcb,
    u16_t length
    );

/**
    Services HTTP progress and expires inactive connections.

    @param context Unused caller context supplied by lwIP.
    @param pcb Connection whose response or timeout is being serviced.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
PollHttpClient (
    [[maybe_unused]] void* context,
    [[maybe_unused]] tcp_pcb* pcb
    );

/**
    Releases HTTP client state after a TCP error.

    @param context Unused caller context supplied by lwIP.
    @param error Transport error reported after lwIP has freed the connection.
*/
void
HandleHttpClientError (
    [[maybe_unused]] void* context,
    [[maybe_unused]] err_t error
    );

/**
    Removes TCP callbacks before closing or replacing a connection.

    @param pcb lwIP TCP connection associated with this callback or operation.
*/
void
DetachHttpClientCallbacks (
    tcp_pcb* pcb
    )
{
    tcp_arg(pcb, nullptr);
    tcp_recv(pcb, nullptr);
    tcp_sent(pcb, nullptr);
    tcp_err(pcb, nullptr);
    tcp_poll(pcb, nullptr, 0);
} // DetachHttpClientCallbacks

/**
    Installs the HTTP client's TCP callbacks.

    @param pcb lwIP TCP connection associated with this callback or operation.
*/
void
AttachHttpClientCallbacks (
    tcp_pcb* pcb
    )
{
    tcp_arg(pcb, &client);
    tcp_recv(pcb, ReceiveHttpRequestBytes);
    tcp_sent(pcb, HandleHttpResponseAcknowledgement);
    tcp_err(pcb, HandleHttpClientError);
    tcp_poll(pcb, PollHttpClient, TCP_POLL_INTERVAL_TICKS); // Approximately one second.
} // AttachHttpClientCallbacks

/**
    Aborts an HTTP client and releases its pending request.

    Detaches callbacks before aborting the PCB so lwIP cannot call into reset client state. An
    owned raw-photo receive token is canceled before the single-client record is reused.
    A media-session request is abandoned without canceling confirmed SD progress;
    service shutdown requests that cancellation separately.

    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
AbortHttpClient (
    void
    )
{
    tcp_pcb* pcb = nullptr;

    if (receiver) {
        receiver->AbortFrame(client.owner);
    }

    if (client.video && httpUpload) {
        if (client.responding) {
            httpUpload->Release();
        } else {
            httpUpload->AbandonRequest();
        }
    }
    pcb = client.pcb;
    if (pcb) {
        DetachHttpClientCallbacks(pcb);
        tcp_abort(pcb);
    }
    client = Client{};
    return ERR_ABRT;
} // AbortHttpClient

/**
    Closes an HTTP connection after its response has been sent.

    Detaches callbacks before attempting to close a fully acknowledged response. A temporary
    memory shortage restores the callbacks and leaves the client alive for a later retry.
    Successful closure resets the request record; fatal errors
    use the abort path. Callers must propagate an abort result without reusing the PCB.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
CloseHttpClient (
    void
    )
{
    tcp_pcb* pcb = nullptr;
    err_t result;

    pcb = client.pcb;
    DetachHttpClientCallbacks(pcb);
    result = tcp_close(pcb);
    if (result == ERR_OK) {

        if (client.video && httpUpload) {
            httpUpload->Release();
        }
        // PCB must never be accessed again after successful tcp_close.
        client = Client{};
        return ERR_OK;
    }
    if (result == ERR_MEM) {
        AttachHttpClientCallbacks(pcb);
        return ERR_OK;
    }
    return AbortHttpClient();
} // CloseHttpClient

/**
    Queues pending HTTP response bytes while TCP has send capacity.

    Queues header and body fragments within the TCP send-space and in-flight limits. When lwIP
    temporarily runs out of memory, offsets are retained for a later sent or poll callback; the
    function does not wait for the peer to acknowledge data. The client closes only after the
    whole response is acknowledged, and fatal transport errors abort it and release its request
    ownership.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
SendPendingHttpResponse (
    void
    )
{
    size_t total;
    bool inHeader;
    const char* data = nullptr;
    size_t remaining;
    size_t amount;
    err_t result;
    err_t nextResult;

    if (!client.responding) {
        return ERR_OK;
    }
    total = client.headerLength + client.bodyLength;
    if (client.acknowledged == total) {
        return CloseHttpClient();
    }

    // Limit the next response fragment to the available TCP send space.
    while (client.queued < total && client.queued - client.acknowledged < kFlightLimit) {
        inHeader = client.queued < client.headerLength;
        if (inHeader) {
            data = client.response + client.queued;
        } else {
            data = client.body + (client.queued - client.headerLength);
        }
        if (inHeader) {
            remaining = client.headerLength - client.queued;
        } else {
            remaining = total - client.queued;
        }

        // Limit the next response fragment to the available TCP send space.
        amount = std::min({remaining, size_t(HTTP_SEND_CHUNK_BYTES), size_t(tcp_sndbuf(client.pcb)),
                      kFlightLimit - (client.queued - client.acknowledged)});
        if (!amount) {
            break;
        }

        // Queue only this fragment; progress advances after lwIP accepts it.
        result = tcp_write(client.pcb, data, static_cast<u16_t>(amount), TCP_WRITE_FLAG_COPY);
        if (result == ERR_MEM) {
            break; // Retry on sent/poll; don't advance offset.
        }
        if (result != ERR_OK) {
            return AbortHttpClient();
        }
        client.queued += amount;
    }

    // Ask lwIP to transmit the queued response bytes.
    nextResult = tcp_output(client.pcb);
    if (nextResult != ERR_OK && nextResult != ERR_MEM) {
        return AbortHttpClient();
    }
    return ERR_OK;
} // SendPendingHttpResponse

/**
    Builds and queues an HTTP response with its content headers.

    Creates the bounded HTTP header while borrowing the supplied response body for later TCP
    transmission. A successful return only marks the response ready to send; it does not copy the
    body or wait for delivery. The body must therefore remain valid until the active client is
    closed or aborted.

    @param code Status or result code to classify or format.
    @param type HTTP response content type.
    @param body Response payload; keep it valid while the transport sends it.
    @param length Number of bytes to process.

    @return true - A response or deferred request was prepared successfully.
    @return false - The request could not be queued or the response exceeded its buffer.
*/
bool
BuildHttpResponse (
    const char* code,
    const char* type,
    const char* body,
    size_t length
    )
{
    int count;

    count = std::snprintf(client.response, sizeof(client.response),
                                    "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                                    "Connection: close\r\nCache-Control: no-store\r\n\r\n",
                                    code, type, static_cast<unsigned long>(length));
    if (count < 0 || static_cast<size_t>(count) >= sizeof(client.response)) {
        return false;
    }
    client.headerLength = static_cast<size_t>(count);
    client.body = body;
    client.bodyLength = length;
    client.responding = true;
    return true;
} // BuildHttpResponse

/**
    Rejects the current HTTP operation and prepares its error response.

    @param status Status snapshot or protocol status code used by this operation.

    @return true - A response or deferred request was prepared successfully.
    @return false - The request could not be queued or the response exceeded its buffer.
*/
bool
RejectHttpRequest (
    int status
    )
{
    const char* text = nullptr;

    receiver->AbortFrame(client.owner);
    client.owner = 0;
    client.receiving = false;

    if (client.video && httpUpload) {
        httpUpload->AbandonRequest();
    }
    text = HttpRequest::GetHttpStatusText(status);
    return BuildHttpResponse(text, "text/plain; charset=utf-8", text, std::strlen(text));
} // RejectHttpRequest

/**
    Routes an HTTP header to the page, raw-photo receiver or video mailbox.

    Checks the request against the PCB's local address before borrowing a page response
    or claiming the shared photo receiver or video mailbox. Conflicting claims are rejected.
    Routing performs no SD I/O; completed frames are saved later by the main loop.

    @return true - A page response, photo transaction or bounded video message was prepared.
    @return false - The response header or local address could not be prepared.
*/
bool
RouteHttpRequest (
    void
    )
{
    char localHost[IPV4_TEXT_BYTES];
    HttpRequest::Result result;

    std::memset(localHost, 0, sizeof(localHost));
    if (!ip4addr_ntoa_r(ip_2_ip4(&client.pcb->local_ip), localHost, sizeof(localHost))) {
        return false;
    }
    result = HttpRequest::ParseRequestHeader(client.request, client.used, localHost);
    if (result.route == HttpRequest::Route::Reject) {
        return RejectHttpRequest(result.status);
    }
    if (result.route == HttpRequest::Route::Page) {
        return BuildHttpResponse("200 OK", "text/html; charset=utf-8", WebPage::kHtml, WebPage::kLength);
    }

    // Reserve one protocol message; the foreground coordinator performs SD work later.
    if (result.route == HttpRequest::Route::Video) {
        if (!httpUpload) {
            return RejectHttpRequest(HTTP_STATUS_NOT_FOUND);
        }
        if (!httpUpload->Reserve()) {
            return RejectHttpRequest(HTTP_STATUS_CONFLICT);
        }
        client.video = true;
        client.receiving = true;
        client.expected = result.bodyBytes;
        return true;
    }

    // Hold the same token used by TCP photos until the complete HTTP body arrives.
    client.expected = HttpRequest::kFrameBytes;
    client.owner = receiver->BeginFrame(client.expected);
    if (!client.owner) {
        return RejectHttpRequest(HTTP_STATUS_CONFLICT);
    }
    client.receiving = true;
    return true;
} // RouteHttpRequest

/**
    Accepts HTTP request bytes and advances request processing.

    Consumes chained and fragmented lwIP buffers, building a bounded header before copying exactly
    the declared request body into its reserved destination. Photo bytes go through their
    ownership token; video messages are copied into the reserved mailbox. SD work is
    deferred to the foreground coordinator after the complete message arrives.

    The callback acknowledges and frees received packet buffers after consumption. Malformed
    input, excess body bytes or transport errors release the client through the abort path.
    Completed pixels are saved by the main loop; this callback never waits on SD I/O.

    @param context Unused caller context supplied by lwIP.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param packet Received lwIP packet chain; null signals that the peer closed the connection.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
ReceiveHttpRequestBytes (
    [[maybe_unused]] void* context,
    tcp_pcb* pcb,
    pbuf* packet,
    err_t error
    )
{
    bool valid;
    pbuf* part = nullptr;
    const uint8_t* bytes = nullptr;
    size_t i;
    size_t amount;
    uint8_t byte;

    if (error != ERR_OK) {
        if (packet) {
            pbuf_free(packet);
        }
        return AbortHttpClient();
    }
    if (!packet) {
        if (client.responding || client.waiting) {
            return SendPendingHttpResponse();
        } else {
            return AbortHttpClient();
        }
    }

    if (client.waiting) {
        pbuf_free(packet);
        return AbortHttpClient();
    }
    valid = true;
    for (part = packet; part && !client.responding && !client.waiting && valid; part = part->next) {
        bytes = static_cast<const uint8_t*>(part->payload);
        i = 0;
        while (i < part->len && !client.responding && !client.waiting && valid) {
            if (client.receiving) {
                amount = part->len - i;
                if (amount > client.expected - client.frameBytes) {
                    valid = RejectHttpRequest(HTTP_STATUS_BAD_REQUEST);
                    break;
                }
                if (client.video) {
                    std::memcpy(httpUpload->Input() + client.frameBytes, bytes + i, amount);
                } else {
                    valid = receiver->FeedFrame(client.owner, bytes + i, amount);
                }
                client.frameBytes += amount;
                i += amount;
            } else {
                byte = bytes[i++];
                if (!byte || byte == ASCII_DELETE ||
                    (byte < ASCII_SPACE && byte != '\r' && byte != '\n' && byte != '\t')) {
                    valid = RejectHttpRequest(HTTP_STATUS_BAD_REQUEST);
                    break;
                }
                if (client.used == sizeof(client.request) - 1) {
                    valid = RejectHttpRequest(HTTP_STATUS_HEADERS_TOO_LARGE);
                    break;
                }
                client.request[client.used++] = static_cast<char>(byte);
                client.request[client.used] = '\0';
                if (client.used >= HTTP_HEADER_TERMINATOR_BYTES &&
                    std::memcmp(client.request + client.used - HTTP_HEADER_TERMINATOR_BYTES, "\r\n\r\n",
                                HTTP_HEADER_TERMINATOR_BYTES) == 0) {
                    valid = RouteHttpRequest();
                }
            }
        }
    }

    tcp_recved(pcb, packet->tot_len);

    // Return the receive buffer to lwIP after its bytes have been consumed.
    pbuf_free(packet);
    client.progress = GetTransportTimeMs();
    if (!valid) {
        return AbortHttpClient();
    }
    if (client.receiving && client.frameBytes == client.expected) {
        if (client.video) {
            httpUpload->Submit(client.frameBytes);
            client.receiving = false;
            client.waiting = true;
            return ERR_OK;
        }

        if (!receiver->FinishFrame(client.owner)) {
            return AbortHttpClient();
        }
        client.owner = 0;
        client.receiving = false;
        if (!BuildHttpResponse("200 OK", "text/plain", "ACK\n", PHOTO_ACK_BYTES)) {
            return AbortHttpClient();
        }
    }
    // Exactly one response per connection. Subsequent pipelined data is discarded.
    return SendPendingHttpResponse();
} // ReceiveHttpRequestBytes

/**
    Advances the HTTP response after TCP acknowledges transmitted bytes.

    @param context Unused caller context supplied by lwIP.
    @param pcb Connection whose response or timeout is being serviced.
    @param length Number of bytes to process.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
HandleHttpResponseAcknowledgement (
    [[maybe_unused]] void* context,
    [[maybe_unused]] tcp_pcb* pcb,
    u16_t length
    )
{
    if (length > client.queued - client.acknowledged) {
        return AbortHttpClient();
    }
    client.acknowledged += length;
    client.progress = GetTransportTimeMs();
    return SendPendingHttpResponse();
} // HandleHttpResponseAcknowledgement

/**
    Services HTTP progress and expires inactive connections.

    @param context Unused caller context supplied by lwIP.
    @param pcb Connection whose response or timeout is being serviced.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
*/
err_t
PollHttpClient (
    [[maybe_unused]] void* context,
    [[maybe_unused]] tcp_pcb* pcb
    )
{
    uint32_t now;

    now = GetTransportTimeMs();
    if (now - client.progress >= kIdleMs || now - client.started >= kLifetimeMs) {
        if (client.used || client.receiving || client.responding) {
            std::printf("[web] request timed out\n");
        }
        return AbortHttpClient();
    }
    return SendPendingHttpResponse();
} // PollHttpClient

/**
    Releases HTTP client state after a TCP error.

    @param context Unused caller context supplied by lwIP.
    @param error Transport error reported after lwIP has freed the connection.
*/
void
HandleHttpClientError (
    [[maybe_unused]] void* context,
    [[maybe_unused]] err_t error
    )
{
    // lwIP has already freed the PCB. Never dereference it here.
    receiver->AbortFrame(client.owner);
    if (client.video && httpUpload) {
        if (client.responding) {
            httpUpload->Release();
        } else {
            httpUpload->AbandonRequest();
        }
    }

    client = Client{};
} // HandleHttpClientError

/**
    Accepts a new HTTP client when a request slot is available.

    Allows one active HTTP client, initializes its bounded parsing state and installs receive,
    sent, poll and error callbacks. Competing clients are refused rather than sharing the single
    request/response storage. Successful acceptance starts an idle deadline; it does not reserve
    an upload transaction until routing identifies one.

    @param context Unused caller context supplied by lwIP.
    @param pcb lwIP TCP connection associated with this callback or operation.
    @param error Error code or diagnostic associated with this operation.

    @return ERR_OK - The callback completed without aborting the connection.
    @return ERR_ABRT - The TCP connection was aborted; lwIP must not access it again.
    @return ERR_VAL - The callback state, incoming connection or stack result is invalid.
*/
err_t
AcceptHttpClient (
    [[maybe_unused]] void* context,
    tcp_pcb* pcb,
    err_t error
    )
{
    if (!pcb) {
        return ERR_VAL;
    }
    // Browsers sometimes preconnect and send nothing. Never let an expired
    // connection keep the single HTTP slot when the foreground/poll is delayed.
    if (client.pcb && (GetTransportTimeMs() - client.progress >= kIdleMs ||
                       GetTransportTimeMs() - client.started >= kLifetimeMs)) {
        if (client.used || client.receiving || client.responding) {
            std::printf("[web] replacing timed-out request\n");
        }
        AbortHttpClient(); // Only the old PCB was aborted; the new callback returns ERR_OK.
    }
    if (error != ERR_OK || client.pcb) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    client = Client{};
    client.pcb = pcb;
    client.started = client.progress = GetTransportTimeMs();
    AttachHttpClientCallbacks(pcb);
    return ERR_OK;
} // AcceptHttpClient

} // namespace

/**
    Connects the HTTP server to the browser video upload coordinator.

    @param upload Foreground browser video upload coordinator; null disables its route.
*/
void
WebServer::SetMediaUpload (
    HttpMediaUpload* upload
    )
{
    httpUpload = upload;
} // SetMediaUpload

/**
    Advances foreground media work, client deadlines and pending responses.

    Single-core callers run storage before transport. The application separates
    these entry points so filesystem access cannot block the network worker.
*/
void
WebServer::Update (
    void
    )
{
    UpdateStorage();
    UpdateTransport();
} // Update

/**
    Processes queued HTTP requests that need foreground storage access.

    Runs on Core 0 outside the lwIP lock. Listener lifecycle handoffs pause this
    entry before Core 1 changes or tears down its radio/transport context.
*/
void
WebServer::UpdateStorage (
    void
    )
{
    if (httpUpload) {
        httpUpload->Update(enabled);
    }
} // UpdateStorage

/**
    Maintains HTTP client deadlines and pending responses.

    Polls the retained client for a completed foreground response and attempts to queue pending
    TCP output. It only moves transport state and response bytes; SD processing is advanced
    separately by the foreground update path. Backpressure keeps the client pending until a later
    update or callback can make progress.
*/
void
WebServer::UpdateTransport (
    void
    )
{
    if (!enabled) {
        return;
    }

    // Serialize TCP state changes with lwIP; keep storage work outside this lock.
    cyw43_arch_lwip_begin();
    if (client.pcb) {
        PollHttpClient(nullptr, client.pcb);
    }
    if (client.pcb && client.waiting && httpUpload->ResponseReady()) {
        client.waiting = false;
        if (!BuildHttpResponse("200 OK", "application/json", httpUpload->Response(),
                               std::strlen(httpUpload->Response()))) {
            AbortHttpClient();
        } else {
            SendPendingHttpResponse();
        }
    }
    cyw43_arch_lwip_end();
} // UpdateTransport

/**
    Starts the media HTTP listener on port 80.

    Creates and binds the HTTP listener in serialized lwIP context, then attaches the supplied
    framebuffer controller to the receive path. An existing healthy listener can be reused;
    allocation, binding or listen failures clean up the partial PCB so a later network-state
    update can retry startup.

    @param upload Shared framebuffer owner and photo upload controller.

    @return true - The required listener is active, including an already matching listener.
    @return false - The service could not allocate, bind or start its listener.
*/
bool
WebServer::Start (
    FrameBufferController& upload
    )
{
    bool same;
    tcp_pcb* pcb = nullptr;
    tcp_pcb* listening = nullptr;

    enabled = true;

    // Foreground API: caller guarantees CYW43 initialization and serial lifecycle.
    cyw43_arch_lwip_begin();
    if (listener) {
        same = receiver == &upload;
        cyw43_arch_lwip_end();
        return same;
    }
    receiver = &upload;
    pcb = tcp_new();
    if (!pcb) {
        cyw43_arch_lwip_end();
        return false;
    }
    if (tcp_bind(pcb, IP_ADDR_ANY, HTTP_LISTEN_PORT) != ERR_OK) {
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    listening = tcp_listen(pcb);
    if (!listening) {
        // Failed listen retains the original PCB: don't lose/leak it.
        tcp_abort(pcb);
        cyw43_arch_lwip_end();
        return false;
    }
    listener = listening;
    tcp_arg(listener, nullptr);
    tcp_accept(listener, AcceptHttpClient);
    cyw43_arch_lwip_end();
    std::printf("[web] HTTP listening on port 80\n");
    return true;
} // Start

/**
    Closes HTTP clients and stops the listener.

    Cancels the active HTTP client before tearing down the listener and clearing its framebuffer
    reference. Callback detachment happens before PCB disposal so late transport events cannot
    access reset state. An incomplete photo token is aborted before the listener closes.
*/
void
WebServer::Stop (
    void
    )
{
    // Must run before deinit, and only with initialized CYW43.
    cyw43_arch_lwip_begin();

    enabled = false;
    if (httpUpload) {
        httpUpload->Disconnect();
    }
    if (client.pcb) {
        AbortHttpClient();
    }
    if (listener) {
        tcp_accept(listener, nullptr);
        // A LISTEN PCB closes synchronously without send-buffer allocation.
        tcp_close(listener);
        listener = nullptr;
    }
    receiver = nullptr;
    cyw43_arch_lwip_end();
} // Stop

/**
    Reports whether the HTTP listener is active.

    @return true - The TCP listener is active.
    @return false - The TCP listener is not active.
*/
bool
WebServer::IsListening (
    void
    )
{
    bool ready;

    cyw43_arch_lwip_begin();
    ready = listener && listener->state == LISTEN;
    cyw43_arch_lwip_end();
    return ready;
} // IsListening

/**
    Reports whether an HTTP request is currently being handled.

    @return true - A client currently owns the service connection.
    @return false - No client connection is active.
*/
bool
WebServer::HasActiveClient (
    void
    )
{
    bool active;

    cyw43_arch_lwip_begin();
    active = client.pcb != nullptr;
    cyw43_arch_lwip_end();
    return active;
} // HasActiveClient
