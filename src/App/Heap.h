#pragma once
#include <Arduino.h>

// How much memory the panel actually has.
//
// `ESP.getFreeHeap()` and `ESP.getMaxAllocHeap()` report MALLOC_CAP_INTERNAL,
// which counts IRAM -- instruction RAM, addressable only in 32-bit words. No
// malloc() serving a byte buffer can ever hand it out, so on this chip those
// accessors read about 37KB higher than the memory anything here can use, and
// the gap does not move: the panel logged a largest free block of exactly
// 36852 bytes in every sample of every boot while its real one ranged from
// 3188 down to 1588.
//
// That is not a rounding error. It made `Ui_canAllocateRow()` return true
// unconditionally -- 36852 > 18432 always -- so the one guard standing between
// a full device list and an LVGL allocation failure had never once refused a
// row. The same number gated whether the panel was allowed to join the network
// at all.
//
// So every decision and every log here reads MALLOC_CAP_8BIT, the pool that
// malloc, LVGL, lwIP and mbedTLS all draw from. ESP.getFreeHeap() is still
// worth printing beside it, because it is what every other tool reports and
// the difference between the two is the point.

/// @brief Free bytes in the 8-bit-addressable heap.
size_t Heap_free();

/// @brief The largest single allocation the 8-bit heap can currently satisfy.
/// It matters as much as the total: mbedTLS wants its record buffers
/// contiguous, and a fragmented heap fails a handshake while looking roomy.
size_t Heap_largest();

/// @brief The low-water mark of Heap_free() since boot.
size_t Heap_minFree();

/// @brief Free bytes as ESP.getFreeHeap() counts them, IRAM included. For
/// comparison only -- never for a decision.
size_t Heap_freeInternal();

/// @brief One line of heap state on the serial console, labelled with `stage`.
void Heap_log(const char *stage);
