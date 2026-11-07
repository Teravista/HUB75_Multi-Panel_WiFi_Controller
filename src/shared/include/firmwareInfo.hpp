#pragma once

/**
    Identifies the firmware image reported by serial startup and controller status.

    Serial startup and controller status share this immutable release label.
    The entrypoint defines the record once for both consumers.
*/
struct FirmwareInfo {
    const char* id;
};

extern const FirmwareInfo FIRMWARE_INFO;
