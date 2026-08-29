#include "networkUpload.hpp"

#include <atomic>
#include <cstdio>
#include <cstddef>

#include "pico/cyw43_arch.h"
#include "lwip/tcp.h"

#include "defines/defines.h"
#include "displayHelpers.hpp"

NetworkUpload::NetworkUpload(DisplayHelpers *displayHelpers, unsigned char *frameBuffer)
    : m_displayHelpers(displayHelpers),
      m_frameBuffer(frameBuffer)
{
}

/*
Convert RGB logical pixel over TCP to physical framebuffer pixel,
applying the logical-to-physical panel mapping and the wiring color byte order correction.
*/
void NetworkUpload::DecodeAndStorePixel(const unsigned char *pixelBytes, size_t pixelIndex)
{
    const int logicalX = static_cast<int>(pixelIndex % LOGICAL_IMAGE_WIDTH);
    const int logicalY = static_cast<int>(pixelIndex / LOGICAL_IMAGE_WIDTH);
    int physicalX = 0;
    int physicalY = 0;

    if (m_displayHelpers != nullptr) {
        m_displayHelpers->ConvertLogicalToPhysicalCoordinates(logicalX, logicalY, physicalX, physicalY);
    }

    // Calculate the destination index in the framebuffer for this pixel.
    const size_t destIndex = (physicalY * MATRIX_PANEL_WIDTH + physicalX) * sizeof(uint32_t);

    //Adjust for the physical wirieng issue.
    m_frameBuffer[destIndex + 0] = pixelBytes[IMAGE_BLUE_BYTE_OFFSET];
    m_frameBuffer[destIndex + 1] = pixelBytes[IMAGE_GREEN_BYTE_OFFSET];
    m_frameBuffer[destIndex + 2] = pixelBytes[IMAGE_RED_BYTE_OFFSET];
    m_frameBuffer[destIndex + 3] = 0;
}

/*
    TCP receive callback for the frame upload server.
*/
err_t NetworkUpload::OnTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    auto *self = static_cast<NetworkUpload *>(arg);
    size_t pbufOffset = 0;
    size_t pbufRemaining = 0;
    size_t bytesNeededForCarry = 0;
    size_t bytesToCopy = 0;

    // If p (received packet buffer) is nullptr, the remote side has closed the connection
    if (p == nullptr) {
        if (self != nullptr) {
            self->m_uploadInProgress.store(false, std::memory_order_release);
        }
        tcp_close(pcb);
        return ERR_OK;
    }

    if (err != ERR_OK) {
        printf("[upload] receive error (err=%d)\n", err);
        pbuf_free(p);
       return err;
    }

    if (self == nullptr) {
        pbuf_free(p);
        tcp_close(pcb);
        return ERR_OK;
    }

    // main loop to process received frame data
    pbufRemaining = p->tot_len;
    while ((pbufRemaining > 0) && (self->m_pixelsReceived < LOGICAL_PIXEL_COUNT))
    {
        // Calculate how many bytes are still needed to complete the current pixel.
        bytesNeededForCarry = sizeof(self->m_pixelCarry) - self->m_pixelCarryLen;

        // Copy only the smaller amount: bytes needed to complete this pixel,
        // or bytes still available in the current received pbuf.
        // This ensures we don't read past the end of the pbuf or overrun the carry buffer.
        bytesToCopy = (bytesNeededForCarry < pbufRemaining) ?
                      bytesNeededForCarry : pbufRemaining;

        // Copy this chunk from the pbuf into the unused portion of the carry buffer.
        pbuf_copy_partial(p, self->m_pixelCarry + self->m_pixelCarryLen, bytesToCopy, pbufOffset);

        // adjust how much of buffer is processed and how much is left to process
        self->m_pixelCarryLen += bytesToCopy;
        pbufOffset += bytesToCopy;
        pbufRemaining -= bytesToCopy;

        //if full pixel is now in carry buffer
        if (self->m_pixelCarryLen == sizeof(self->m_pixelCarry))
        {
            // Carry buffer now contains one complete logical RGB pixel.
            // Decode it and store it at its remapped physical framebuffer
            // position, then prepare the carry buffer for the next pixel.
            self->DecodeAndStorePixel(self->m_pixelCarry, self->m_pixelsReceived);
            self->m_pixelsReceived += 1;
            self->m_pixelCarryLen = 0;
        }
    }

    // Tell lwIP that all bytes in this pbuf processed, allowing it to
    // reuse the receive window for additional TCP data.
    tcp_recved(pcb, p->tot_len);
    // Release the pbuf after copying and decoding its payload.
    pbuf_free(p);

    // Recived all data for full frame
    if (self->m_pixelsReceived == LOGICAL_PIXEL_COUNT)
    {
        self->m_pixelsReceived = 0;
        self->m_activeFrame.store(self->m_frameBuffer, std::memory_order_release);
        self->m_uploadInProgress.store(false, std::memory_order_release);
        printf("[upload] finished id=%u\n", self->m_uploadId);

        // ACK confirms full frame decode/store completed on-device.
        err_t ackErr = tcp_write(
            pcb,
            UPLOAD_ACK_MESSAGE,
            sizeof(UPLOAD_ACK_MESSAGE) - 1,
            TCP_WRITE_FLAG_COPY);


        // Send ACK when queued successfully, then close upload connection.
        if (ackErr == ERR_OK) {
            tcp_output(pcb);
        }
        else {
            printf("[upload] ack write failed (err=%d)\n", ackErr);
        }

        tcp_close(pcb);
    }

    return ERR_OK;
}

/*
    Callback for TCP client error during frame upload.
*/
void NetworkUpload::OnTcpErr(void *arg, err_t err)
{
    auto *self = static_cast<NetworkUpload *>(arg);

    printf("[upload] client TCP error (err=%d)\n", err);
    if (self != nullptr) {
        self->m_uploadInProgress.store(false, std::memory_order_release);
        self->m_pixelsReceived = 0;
        self->m_pixelCarryLen = 0;
    }
}

/*
    Accepts a new TCP connection for frame upload.
*/
err_t NetworkUpload::OnTcpAccept(void *arg, struct tcp_pcb *client, err_t err)
{
    auto *self = static_cast<NetworkUpload *>(arg);

    if (err != ERR_OK || client == nullptr || self == nullptr) {
        return err;
    }

    //set variables for future sanity checks to make sure we are not already receiving a frame or have a carry over from a previous frame
    self->m_pixelsReceived = 0;
    self->m_pixelCarryLen = 0;
    self->m_uploadInProgress.store(true, std::memory_order_release);
    //advance the uplod id counter, suefull for debuging
    ++self->m_uploadId;

    printf("[upload] accept from %s:%u\n",
           ipaddr_ntoa(&client->remote_ip),
           static_cast<unsigned int>(client->remote_port));

    printf("[upload] start receiving id=%u expect=%zu bytes (%dx%d, %d bpp)\n",
           self->m_uploadId,
            SOURCE_FRAME_SIZE_BYTES,
            LOGICAL_IMAGE_WIDTH,
            LOGICAL_IMAGE_HEIGHT,
            IMAGE_BYTES_PER_PIXEL);

    //start callbacks for receiving data and error handling
    tcp_arg(client, self);
    tcp_recv(client, OnTcpRecv);
    tcp_err(client, OnTcpErr);
    return ERR_OK;
}

/*
    Resets only in-flight receive state, used when (re)starting the TCP
    listener. Unlike InitFrameUploadState, this must NOT clear m_activeFrame
    or the lifetime counters, otherwise a WiFi reconnect wipes the last
    successfully uploaded frame back to the default flashed image.
*/
void NetworkUpload::ResetPixelReceiveState()
{
    m_uploadInProgress.store(false, std::memory_order_relaxed);
    m_pixelsReceived = 0;
    m_pixelCarryLen = 0;
}

/*
    Initializes TCP frame receiver state.
*/
void NetworkUpload::InitFrameUploadState()
{
    m_activeFrame.store(nullptr, std::memory_order_relaxed);
    m_uploadInProgress.store(false, std::memory_order_relaxed);
    m_pixelsReceived = 0;
    m_pixelCarryLen = 0;
    m_listenerPcb = nullptr;
    m_listenerPort = 0;
}

/*
    For clean shutdown and state change we need to close the listener and reset all state so we can start again later so no network issue occur later on
*/
void NetworkUpload::StopFrameUploadServer()
{
    if (m_listenerPcb != nullptr)
    {
        cyw43_arch_lwip_begin();
        tcp_accept(m_listenerPcb, nullptr);
        tcp_close(m_listenerPcb);
        cyw43_arch_lwip_end();
        printf("[upload] listener closed on port %u\n", m_listenerPort);
    }

    m_listenerPcb = nullptr;
    m_listenerPort = 0;
    ResetPixelReceiveState();
}

/*
  Reports whether a TCP upload is currently writing pixels into the shared
  framebuffer. The render loop uses this to avoid reading a partially written
  frame.
*/
bool NetworkUpload::IsFrameUploadInProgress()
{
    return m_uploadInProgress.load(std::memory_order_acquire);
}

/*
    Starts the TCP frame upload listener on the specified port.
*/
bool NetworkUpload::StartFrameUploadServer(
    uint16_t port)
{
    ResetPixelReceiveState();
    struct tcp_pcb *pcb;
    err_t bindErr;

    // Reuse an existing listener instead of creating a duplicate on this port.
    if (m_listenerPcb != nullptr && m_listenerPort == port) {
        printf("[upload] TCP already listening on port %u\n", port);
        return true;
    }

    cyw43_arch_lwip_begin();

    // Allocate a TCP control block for the upload listener.
    pcb = tcp_new();
    if (pcb == nullptr) {
        // Allocation failed, so the listener cannot be created.
        printf("[upload] tcp_new failed\n");
        cyw43_arch_lwip_end();
        return false;
    }

    bindErr = tcp_bind(pcb, IP_ADDR_ANY, port);
    if (bindErr != ERR_OK) {
        //most common error so print a more specific message for it
        if (bindErr == ERR_USE) {
            printf("[upload] port %u already in use\n", port);
        }

        printf("[upload] tcp_bind failed on port %u (err=%d)\n", port, bindErr);
        tcp_close(pcb);
        cyw43_arch_lwip_end();
        return false;
    }

    pcb = tcp_listen(pcb);
    if (pcb == nullptr) {
        printf("[upload] tcp_listen failed\n");
        cyw43_arch_lwip_end();
        return false;
    }

    // Listener setup complete; register callback and mark it ready for uploads.
    tcp_arg(pcb, this);
    tcp_accept(pcb, OnTcpAccept);
    m_listenerPcb = pcb;
    m_listenerPort = port;
    cyw43_arch_lwip_end();
    printf("[upload] TCP listening on port %u\n", port);
    return true;
}

/*
    Reports whether the local TCP listener still exists and is LISTEN state
*/
bool NetworkUpload::IsFrameUploadServerListening()
{
    bool listenerReady = false;

    cyw43_arch_lwip_begin();
    if (m_listenerPcb != nullptr) {
        listenerReady = m_listenerPcb->state == LISTEN;
    }
    cyw43_arch_lwip_end();

    return listenerReady;
}

/*
    stores a pointer to the latest fully uploaded frame,
*/
const unsigned char *NetworkUpload::GetUploadedFrameData()
{
    return m_activeFrame.load(std::memory_order_acquire);
}
