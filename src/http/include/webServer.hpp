#pragma once
class FrameBufferController;
class HttpMediaUpload;

// Core 1 owns listener lifecycle and transport; Core 0 processes storage requests.
namespace WebServer {
// Starts the media HTTP listener on port 80.
bool Start(FrameBufferController& upload);

// Connects the HTTP server to the browser video upload coordinator.
void SetMediaUpload(HttpMediaUpload* upload);

// Advances foreground media work, client deadlines and pending responses.
void Update(void);

// Processes queued HTTP requests that need foreground storage access.
void UpdateStorage(void);

// Maintains HTTP client deadlines and pending responses.
void UpdateTransport(void);

// Closes HTTP clients and stops the listener.
void Stop(void);

// Reports whether the HTTP listener is active.
bool IsListening(void);

} // namespace WebServer
