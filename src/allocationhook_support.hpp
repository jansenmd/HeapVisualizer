#pragma once

#include <cstddef>

namespace HeapVisualizer::AllocationHooks
{

class HookScope
{
public:
    HookScope();
    ~HookScope();

private:
    bool mPreviousValue = false;
};

[[nodiscard]] bool IsInsideHook();
void TrackAllocation(void* pointer, std::size_t size, bool internalAllocation);
void UntrackAllocation(void* pointer);
void* CallLibcMalloc(std::size_t size);
void* CallLibcCalloc(std::size_t count, std::size_t size);
void* CallLibcRealloc(void* pointer, std::size_t size);
void CallLibcFree(void* pointer);
void* CallLibcMemalign(std::size_t alignment, std::size_t size);
[[nodiscard]] bool HasMultiplicationOverflow(std::size_t count, std::size_t size);
[[nodiscard]] std::size_t AllocationSizeProduct(std::size_t count, std::size_t size);
[[nodiscard]] bool IsValidAlignment(std::size_t alignment);
[[nodiscard]] std::size_t PageSize();
[[nodiscard]] std::size_t RoundUpToPageSize(std::size_t size);
void RecordMalloc();
void RecordCalloc();
void RecordRealloc();
void RecordReallocArray();
void RecordFree();
void RecordPosixMemalign();
void RecordAlignedAlloc();
void RecordMemalign();
void RecordValloc();
void RecordPvalloc();

}
