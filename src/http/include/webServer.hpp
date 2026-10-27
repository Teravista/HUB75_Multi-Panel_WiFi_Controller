#pragma once
class FrameBufferController;
class HttpMediaUpload;

// Foreground lifecycle calls require an initialized radio; stop before radio teardown.
namespace WebServer {
// Starts the media HTTP listener on port 80.
bool Start(FrameBufferController& upload);

// Connects the HTTP server to the browser video upload coordinator.
void SetMediaUpload(HttpMediaUpload* upload);

// Advances foreground media work, client deadlines and pending responses.
void Update(void);

// Closes HTTP clients and stops the listener.
void Stop(void);

// Reports whether the HTTP listener is active.
bool IsListening(void);

} // namespace WebServer
