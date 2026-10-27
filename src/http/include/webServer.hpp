#pragma once
class FrameBufferController;

// Foreground lifecycle calls require an initialized radio; stop before radio teardown.
namespace WebServer {
// Starts the photo HTTP listener on port 80.
bool Start(FrameBufferController& upload);

// Maintains HTTP client deadlines and pending responses.
void Update(void);

// Closes HTTP clients and stops the listener.
void Stop(void);

// Reports whether the HTTP listener is active.
bool IsListening(void);

} // namespace WebServer
