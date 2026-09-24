#include "Heap.h"
#include "Net.h"

#include <esp_heap_caps.h>

size_t Heap_free()
{
    return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t Heap_largest()
{
    return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
}

size_t Heap_minFree()
{
    return heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

size_t Heap_freeInternal()
{
    return ESP.getFreeHeap();
}

void Heap_log(const char *stage)
{
    // The trend is the one thing a crash log cannot show after the fact: a
    // steady fall is a leak, "free" holding while "largest" shrinks is
    // fragmentation, and "min" dipping near zero at reconnects is a spike.
    //
    // "internal" is printed last and deliberately labelled, so the number every
    // other ESP32 tool shows is visible without being mistaken for usable
    // memory. The gap between it and "free" is IRAM.
    Serial.printf("[heap] %-14s free=%6u min=%6u largest=%6u dropped=%u (internal=%u)\n",
                  stage, (unsigned)Heap_free(), (unsigned)Heap_minFree(),
                  (unsigned)Heap_largest(), (unsigned)Net_droppedMessages(),
                  (unsigned)Heap_freeInternal());
}
