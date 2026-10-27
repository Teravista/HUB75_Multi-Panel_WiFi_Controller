#pragma once
#ifndef JPEG_PARALLEL_READER
#define JPEG_PARALLEL_READER 0
#endif
// Reserves idle Core 1 capacity for a JPEG decode job.
bool ReserveJpegDecodeWorker(void);

// Releases an unused Core 1 JPEG decode reservation.
void CancelJpegDecodeWorkerReservation(void);

// Submits a JPEG decode callback to the reserved worker.
bool StartJpegDecodeWorker(void (*work)(void* context), void* context);

// Reports whether the Core 1 JPEG decode job has completed.
bool JpegDecodeWorkerFinished(void);

// Yields briefly while waiting for JPEG pipeline progress.
void JpegPipelineYield(void);
