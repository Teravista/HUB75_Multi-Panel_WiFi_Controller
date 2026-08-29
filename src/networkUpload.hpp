#pragma once

#include <atomic>
#include <cstdint>
#include <cstddef>

#include "lwip/tcp.h"
#include "defines/defines.h"
#include "displayHelpers.hpp"

class NetworkUpload {
public:
    NetworkUpload() = default;
    explicit NetworkUpload(DisplayHelpers *displayHelpers, unsigned char *frameBuffer = nullptr);

    /*
        Initializes TCP frame receiver state.
    */
    void InitFrameUploadState();

    /*
      Reports whether a TCP upload is currently writing pixels into the shared
      framebuffer. The render loop uses this to avoid reading a partially written
      frame.
    */
    bool IsFrameUploadInProgress();

    /*
        For clean shutdown and state change we need to close the listener and reset all state so we can start again later so no network issue occur later on
    */
    void StopFrameUploadServer();

    /*
        Starts the TCP frame upload listener on the specified port.
    */
    bool StartFrameUploadServer(
        uint16_t port
    );

    /*
        Reports whether the local TCP listener still exists and is LISTEN state
    */
    bool IsFrameUploadServerListening();

    /*
        stores a pointer to the latest fully uploaded frame,
    */
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

    /*
    Convert RGB logical pixel over TCP to physical framebuffer pixel,
    applying the logical-to-physical panel mapping and the wiring color byte order correction.
    */
    void DecodeAndStorePixel(const unsigned char *pixelBytes, size_t pixelIndex);

    /*
        TCP receive callback for the frame upload server.
    */
    static err_t OnTcpRecv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err);

    /*
        Callback for TCP client error during frame upload.
    */
    static void OnTcpErr(void *arg, err_t err);

    /*
        Accepts a new TCP connection for frame upload.
    */
    static err_t OnTcpAccept(void *arg, struct tcp_pcb *client, err_t err);

    /*
        Resets only in-flight receive state, used when (re)starting the TCP
        listener. Unlike InitFrameUploadState, this must NOT clear m_activeFrame
        or the lifetime counters, otherwise a WiFi reconnect wipes the last
        successfully uploaded frame back to the default flashed image.
    */
    void ResetPixelReceiveState();
};
