#include "media/jpeg/include/jpegWorker.hpp"
#include "core/include/sharedCore1.hpp"
#include "pico/stdlib.h"

namespace {
Core1Scheduler scheduler;
}

/**
    Returns the scheduler shared by network work and JPEG decoding.

    @return reference - Process-lifetime scheduler shared by networking and JPEG decoding.
*/
Core1Scheduler&
SharedCore1Scheduler (
    void
    )
{
    return scheduler;
} // SharedCore1Scheduler

/**
    Reserves idle Core 1 capacity for a JPEG decode job.

    Reserve before consuming any parallel input. Failure means decode on Core 0. Cancel if input
    preparation fails before Start; never cancel a running job.

    @return true - Core 1 was reserved for decoding.
    @return false - Core 1 is busy or unavailable; the caller may decode locally.
*/
bool
ReserveJpegDecodeWorker (
    void
    )
{
    return scheduler.TryReserveDecode();
} // ReserveJpegDecodeWorker

/**
    Releases an unused Core 1 JPEG decode reservation.
*/
void
CancelJpegDecodeWorkerReservation (
    void
    )
{
    scheduler.CancelReservation();
} // CancelJpegDecodeWorkerReservation

/**
    Submits a JPEG decode callback to the reserved worker.

    One decoder job at a time. Start/Finished are core0-only; Finished has acquire semantics and
    guarantees all worker memory accesses have ended before reuse.

    @param work Borrowed decoder callback executed on the reserved worker.
    @param context Caller-owned state passed unchanged to the callback.

    @return true - The callback was queued on the reserved worker.
    @return false - The callback is missing or no valid decode reservation is held.
*/
bool
StartJpegDecodeWorker (
    void (*work)(void* context),
    void* context
    )
{
    return scheduler.SubmitDecode(work, context);
} // StartJpegDecodeWorker

/**
    Reports whether the Core 1 JPEG decode job has completed.

    @return true - The submitted worker job has finished.
    @return false - The submitted worker job has not finished.
*/
bool
JpegDecodeWorkerFinished (
    void
    )
{
    return scheduler.DecodeFinished();
} // JpegDecodeWorkerFinished

/**
    Yields briefly while waiting for JPEG pipeline progress.
*/
void
JpegPipelineYield (
    void
    )
{
    tight_loop_contents();
} // JpegPipelineYield
