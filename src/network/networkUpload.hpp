#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "lwip/tcp.h"
#include "config/defines.h"
#include "display/include/displayHelpers.hpp"

class NetworkUpload {
public:
    // Initializes an unbound photo upload controller.
    NetworkUpload() = default;

    // Connects frame reception to the CPU framebuffer and display helpers.
    explicit NetworkUpload(DisplayHelpers *displayHelpers, unsigned char *frameBuffer = nullptr);

    // Initializes photo reception and clears the published framebuffer pointer.
    void InitFrameUploadState();

    // Reports whether a photo upload is currently receiving bytes.
    bool IsFrameUploadInProgress();

    // Stops the raw TCP photo upload listener and resets receive state.
    void StopFrameUploadServer();

    // Starts the raw TCP photo upload listener.
    bool StartFrameUploadServer(
        uint16_t port
    );

    // Reports whether the raw TCP photo listener is active.
    bool IsFrameUploadServerListening();

    // Returns the completed logical frame when one is available.
    const unsigned char *GetUploadedFrameData();

private:
    DisplayHelpers *m_displayHelpers = nullptr;

    // Pointer to the shared physical framebuffer owned by main entry.
    unsigned char *m_frameBuffer = nullptr;

    // Wire format is raw logical RGB (IMAGE_BYTES_PER_PIXEL bytes/pixel); decoded
    // and remapped into m_frameBuffer one pixel at a time as bytes arrive, so no
    // full-frame staging buffer is needed.
    unsigned char m_pixelCarry[IMAGE_BYTES_PER_PIXEL]{};
    size_t m_pixelCarryLen = 0;
    size_t m_pixelsReceived = 0;

    //atomic added for safety
    std::atomic<unsigned char *> m_activeFrame{nullptr};
    std::atomic<bool> m_uploadInProgress{false};
    uint32_t m_uploadId = 0;
    struct tcp_pcb *m_listenerPcb = nullptr;
    uint16_t m_listenerPort = 0;

    // Converts one received RGB888 pixel into the CPU framebuffer format.
    void DecodeAndStorePixel(const unsigned char *pixelBytes, size_t pixelIndex);

    // Accepts TCP photo bytes and completes or rejects the uploaded frame.
    static err_t OnTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);

    // Releases photo upload state after a TCP connection error.
    static void OnTcpErr(void *arg, err_t err);

    // Accepts a TCP photo client and initializes its receive state.
    static err_t OnTcpAccept(void *arg, struct tcp_pcb *client, err_t err);

    // Resets incomplete photo reception without clearing the last completed frame.
    void ResetPixelReceiveState();

};
