#pragma once

#include <cstddef>

namespace HeapVisualizer::AllocationHooks
{

void TrackAllocationTarget(void* pointer, std::size_t size);
void TrackAllocationInternal(void* pointer, std::size_t size);
void UntrackAllocation(void* pointer);

}
