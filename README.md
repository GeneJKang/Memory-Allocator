# Memory Allocator

## MM Memory Allocator
This is a continuation from my malloc lab for the CS 351 Class at IIT. I decided to continue and improve upon it, now as a personal project.

MM is a malloc, free, realloc, and calloc manager for small embedded systems (such as microcontrollers) written in C. It manages one block of RAM that you hand it at startup, so it needs no operating system. It is a modified version of the TLSF (Two-Level Segregated Fit) design. It has O(1) memory allocation and deallocation during normal use.

It uses a TLSF look up and a small best-fit look up that reuses freed blocks of the right size before splitting bigger blocks. On a 64 KB heap, this lets it fit more data than newlib-nano (the default memory manager on the STM32) and the TLSF that the ESP-IDF's heap (heap_caps_malloc) is built on, while having matching or even faster speeds than the two allocators.

The 8-byte aligned version has two versions built from the same file:
+ Standard (default): Every pointer is 8-byte aligned
+ Hybrid ( -DMM_SIZE_ALIGN 32-bit targets): Requests whose sizes are a multiple of 8 still get a 8-byte-aligned memory allocation. All other requests get a 4-byte-aligned memory allocation. This saves 4 bytes on many blocks, so more data fits at the cost of some runtime speed.

## General Statistics
All numbers are for a 64 KB heap with mixed request sizes (8 to 1024 bytes) unless noted.

| | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| % Memory allocated when malloc first fails | 91.3% | <mark> **92.2%** </mark> | 87.3% | 86.1% | 86.7% |
| % Memory allocated when malloc first fails (16 KB heap) | 86.9% | <mark> **87.2%** </mark> | 69.9% | 83.1% | 83.1% |
| % Failed mallocs with heap at 80% full | 1.8% | <mark> **1.6%** </mark> | 7.3% | 98.6% | 97.3% |
| Malloc runtime with 4,000 fragments (by CPU cycles) | 122 | 128 |<mark> **120** </mark> | 14,728 | 14,734 |
| Throughput (millon calls per second) | <mark> **24.8** </mark> | 22.4 | 22.8 | 13.4 | 10.2 |
| Instructions per call (Cortex-M4) | <mark> **66** </mark> | 75 | 93 | 146 | 244 |

ESP32's TLSF uses the TLSF by Matthew Conte, which the ESP-IDF's heap is built on. In this 32-bit test build it returns 4-byte aligned pointers, which saves a little memory per block, but malloc on a Cortex-M has to return a 8-byte aligned pointers. The hybrid version only uses 4-byte alignment for sizes that cannot hold such a value so it can still run on the Arm ABI.

## Current Capabilities
+ Finding a free block takes two bitmap lookups and a short list check. This makes timing predictable and runtime does not grow as the heap fragments.
+ Freed blocks of the right size are reused before larger blocks are split, so the heap fills closer to 100% before failures start to occur. This allows a higher memory usage % than others.
+ The program runs on a fixed buffer instead of a dynamic buffer. Meaning it can be declared at the beginning
+ If requests are too large, the program handles the overflow the best it can instead of corrupting memory.
+ (Optional) With hybrid alignment, the malloc packs blocks on a 4-byte grid if the C rules allow it.

## How It Works

### Block Layout

The heap is one contiguous region split into blocks. Every block starts with a 4-byte header that stores its size and two flag bits. In the standard version sizes are always multiples of 8, so the bottom three bits of the size are free to hold flags.

| Field | Allocated Block | Free Block |
| --- | --- | --- |
|Header (4 B) | Size, "allocated" Bit, and "previous block allocated" Bit | Same |
| Payload | Allocated Data | Pointers to the next free block and a back pointer (see below) |
| Footer (4 B) | None | Copy of the size |

Because each header records whether the block before it is allocated, an allocated block never needs a footer. Only free blocks need a footer, so a neighbor that is being freed can find the start of the free block in front of it and merge with it. On a 32-bit microcontroller, the smallest block is 16 bytes.

The back pointer does not point at the previous block. It points at the pointer that leads to this block (pointer to a pointer) which can either be the list's head slot or the previous block's "next" field. Every list ends at a shared sentinel block (NIL) whose size reads as 0, so it never fits a request and the list code never has to check for NULL.

### Size Classes

Free blocks are sorted into lists by size. The first level splits sizes by powers of 2 (16 - 31 B, 32 - 63 B, 64 - 127 B, and so on). The second level splits each of those ranges into 16 equal parts. So a 300-byte block lands in the 256 - 511 B row, in the list for 288 - 303 B.

Two bitmaps record which lists are non-empty: one bit per row, and one bit per list within each row. Finding the smallest non-empty list that is big enough is two "find first set bit" instructions, meaning the timing is generally static.

### Finding a block for malloc

+ Check the request's own list.
    + Look at the first two blocks (or N blocks depending on the settings) in the list the request maps to and take the smallest one that fits, and stop early on an exact fit. This is similar to a best fit N search and is the main difference from a standard TLSF (though there are other differences as well), and it is where the extra memory efficiency comes from.
+ Use bitmaps
    + If nothing in its own list fits, it rounds the request up to the next list, where every block is guaranteed to fit, and uses the bitmaps to jump straight to the first non-empty list at or above that.
+ Last resort
    + If the bitmaps find nothing, there is a persistent last resort search of the request's own list before that malloc fails.

The chosen block is split if the leftover is big enough to be a block of its own, and the leftover goes back into the right list.

### free and realloc

free marks the block as free and immediately merges it with a free neighbor on either side, so two free blocks are never left next to each other. This allows for less fragmentation. realloc shrinks a block in place and returns the tail to the heap. To grow, it first tries to absorb a free block right after it. It only ever allocates a new block and copies the data when neither works.

### Hybrid Version

A type's alignment always divides its size, so a request whose size is not a multiple of 8 cannot hold a double, uint64_t, or any other type that needs 8-byte alignment thus, the data only needs 4-byte alignment. The hybrid version uses this to put blocks on a 4-byte grid instea of an 8-byte one:
+ Sizes that are not a multiple of 8
    + Rounded up to a multiple of 4 instead of 8. A 13-byte request needs 13 + 4 = 17 bytes with its header, which rounds up to a 20-byte block instead of 24.
+ Sizes that are a multiple of 8
    + This uses the standard block size and alignment, which always leaves 4 spare bytes. If the block happens to start 4 bytes off an 8-byte boundary, the data is moved up into the spare bytes and a marker word is written in front of it. The marker's field reads as 0, which no other header has, so free and realloc can tell taht the block's real header is 4 bytes further back.

Because block sizes are now multiples of 4, only the bottom two bits of the size hold flags (which is still enough for what we need). The hybrid version needs 4-byte pointers, so it is for 32-bit targets only. On a 64-bit build -DMM_SIZE_ALIGN is ignored and only the standard version works.

It fits more data than the standard version on workloads with many small, and/orr odd-sized requests, at the cost of some runtime speed. The C11 standard asks malloc to return memory alignment for any type, but the hybrid version instead returns "any type that fits in the block".

## Using it

### Files
| File | Purpose |
| --- | --- |
| mm.c, mm.h | Has the main allocators: mm_init, mm_malloc, mm_free, mm_realloc, mm_calloc |
| heap.c, heap.h | Hands the allocator its memory region. This is a buffer or a reserved region on a embedded system or Linux |

### Example Usage

```c
#include <stdint.h>
#include "heap.h"
#include "mm.h"

//The heap uses any 8-byte aligned buffer
static _Alignas(8) uint8_t heap_mem[64 * 1024];

int main(void)
{
    if (heap_init(heap_mem, sizeof heap_mem) < 0 || mm_init() < 0) {
        //Buffer is misaligned, or FL_COUNT too small for this heap size
        //Error handling would be here
    }

    char *buf = mm_malloc(128);
    if (buf == NULL) {
        //out of memory
        //Error handling would be here
    }

    char *bigger = mm_realloc(buf, 512);   //on failure buf is left untouched
    if (bigger != NULL) {
        buf = bigger;
    }

    mm_free(buf);
}
```

Compile with -DFL_COUNT=16 for this 64 KB heap (see next section). On Linux you can skip heap_init. The first mm_malloc reserves a 1 GB region automatically.

### Configuration

| Setting | Default | What it does |
| --- | --- | --- |
| FL_COUNT | 32 | Number of size-class rows. It must cover the biggest block the heap can hold, so set it to log2 of the heap size. Smaller values save RAM.
| SL_BITS | 4 | Splits each rows into 2^SL_BITS lists. 4 is the fastest setting and 3 halves the table RAM for a small loss in speed. Any lower or any higher has reduced the speed significantly for no benefit.
| HOME_SCAN | 2 | How many blocks of the request's own list to check before using the bitmaps. Values above 2 gave no further gain in testing. |
| MM_SIZE_ALIGN | off/false | Builds the hybrid version. Only takes effect on 32-bit targets. |

| Heap size | FL_COUNT | Size-class table RAM (SL_BITS 4) |
|---|---:|---:|
| 16 KB | 14 | 956 B |
| 32 KB | 15 | 1,024 B |
| 64 KB | 16 | 1,092 B |
| 128 KB | 17 | 1,160 B |
| 256 KB | 18 | 1,228 B |

If FL_COUNT is too small for the buffer, mm_init returns -1 and every mm_malloc returns NULL.

## Performance

### Test Setup

Each allocator manages the same fixed buffer and runs the same random workloads.
+ Small blocks: 8 to 64 bytes.
+ Mixed sizes: 60% small blocks, 30% 65 to 256 B, 10% 257 to 1024 B
+ Packet buffers: Fixed sized blocks of 16, 32, 64, 128, 256, 512, and 1024 B, weighted towards small ones

The comparison allocators are FreeRTOS heap_4, the malloc from newlib-nano (the C library in the STM32CubeIDE and most bare-metal Arm CPU builds) and the reference TLSF (heap_caps_malloc) that the ESP-IDF is built on. Everything is compiled with 4 byte pointers as on a Cortex-M. Every allocated block is filled with a pattern and checked before it is freed.

Times are measured in CPU cycles on an x86 PC, one pinned core. They are good for comparing allocators with each other, but they are not Cortex-M cycle counts. The Cortex-M4 section below counts the instructions each allocator runs on the target instruction set.


### Memory Efficiency

The test allocates and frees at random until the first malloc returns NULL, then measures how much of the heap holds live data at that moment. Higher is better. This uses the median of 60 runs.

<sub><i>All graphs were created using AI by using python scripts</i></sub>

<img width="1760" height="784" alt="memory-efficiency-dark" src="https://github.com/user-attachments/assets/3a24ffc4-81c0-4219-8181-1a5dbe2b0fbd" />

| 64 KB Heap | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| Small Blocks (8 - 64 B) | 79.9% | <mark> **81.7%** </mark> | 78.7% | 71.0% | 74.3% |
| Mixed Sizes (8 - 1024 B) | 91.3% | <mark> **92.2%** </mark> | 87.3% | 86.1% | 86.7% |
| Packet Buffers (16 - 1024 B) | <mark> **92.3%** </mark> | <mark> **92.3%** </mark> | 89.6% | 89.2% | 89.4% |

The hybrid version gains the most benefit on small blocks because of it's ability to save memory on 4-byte alignment. Packet buffers are multiples of 8 so both the hybrid and standard converse on the same value.

The lead lasts across different mixed heap sizes. On a 16 KB heap the reference TLSF loses significantly more because its 3.2 KB control table exists inside the heap, while MM keeps its table in normal RAM.

<img width="1760" height="784" alt="heap-size-dark" src="https://github.com/user-attachments/assets/e9f143f7-a823-4704-8ebc-189a199f902d" />

| Mixed Sizes | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| 16 KB Heap | 86.9% | <mark> **87.2%** </mark> | 69.9% | 83.1% | 83.1% |
| 64 KB Heap | 91.3% | <mark> **92.2%** </mark> | 87.3% | 86.1% | 86.7% |
| 256 KB Heap | 92.9% | <mark> **93.7%** </mark> | 92.8% | 87.7% | 88.9% |

### Running Near Full Usage

The test has the heap at a fixed level for 11 runs x 200,000 calls. It allocates when below the level and frees at a random block when above it. The number is the share of malloc calls that failed. Lower is better. First-fit allocators break the heap into pieces too small to use, so they start to fail much earlier.

<img width="1760" height="784" alt="full-heap-dark" src="https://github.com/user-attachments/assets/12fcc956-0ae6-418f-a9c7-96b30b47dd5d" />

| Heap Usage At | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32)  | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| 75% Full (Small Blocks) | <mark> **0.0%** </mark> | <mark> **0.0%** </mark> | <mark> **0.0%** </mark> | 99.9% | 99.5% |
| 78% Full (Small Blocks) | <mark> **0.0%** </mark> | <mark> **0.0%** </mark> | 97.4% | 100.0% | 99.9% |
| 80% Full (Small Blocks) | 98.8% | <mark> **37.0%** </mark> | 99.5% | 100.0% | 100.0% |
| 75% Full (Mixed Sizes) | <mark> **0.1%** </mark> | <mark> **0.1%** </mark> | 1.0% | 0.8% | 0.4% |
| 80% Full (Mixed Sizes) | 1.8% | <mark> **1.6%** </mark> | 7.3% | 98.6% | 97.3% |
| 85% Full (Mixed Sizes) | 93.3% | <mark> **70.1%** </mark> | 98.6% | 99.6% | 99.3% |
| 80% Full (Packet Buffers) | <mark> **0.2%** </mark> | <mark> **0.2%** </mark> | 0.9% | 1.7% | 1.4% |
| 85% Full (Packet Buffers) | <mark> **3.1%** </mark> | <mark> **3.1%** </mark> | 11.5% | 98.7% | 98.6% |

Every allocator has a fill level where it suddenly stops working, because the data plus the overhead of every block no longer fits in the heap. It is especially worse on the small blocks section. The standard version holds heap at 78% with almost no failures but then fails nearly every request at 80%, which is above the 79.9% it can fit at all in the memory-efficiency test.

### Throughput

The test runs a loop of malloc and free calls and is timed with the heap held at 60% full. Higher is better. This uses the median of 15 runs.

<img width="1760" height="784" alt="throughput-dark" src="https://github.com/user-attachments/assets/779f9bb1-7fd3-4ad2-9461-eb817e8cda91" />

| Million Calls per Second | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| Small Blocks (8 - 64 B) | <mark> **36.7** </mark> | 28.7 | 34.5 | 16.9 | 7.7 |
| Mixed Sizes (8 - 1024 B) | <mark> **24.8** </mark> | 22.4 | 22.8 | 14.7 | 11.2 |
| Packet Buffers (16 - 1024 B) | <mark> **23.3** </mark> | <mark> **23.1** </mark> | 21.2 | 15.8 | 13.8 |

The standard version is 6-10% faster than the ESP TLSF. The hybrid version is 22% slower than the standard version on small blocks and 10% slower on mixed sizes, because odd sizes spread over twice as many lists and exact fits in the request's own list becomes more rare. So it is expected as there are less 4-byte aligned requests, speed increases as seen under packet buffers.

### Latency per Call

This test times every call individually with mixed sizes and the heap half full (50%). Lower is better

<img width="1760" height="784" alt="latency-dark" src="https://github.com/user-attachments/assets/d02afb58-9dac-49fd-bb8d-02ce3706d772" />

| CPU Cycles | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| malloc (median) | <mark> **118** </mark> | 130 | <mark> **118** </mark> | 162 | 224 |
| malloc (99th Percentile) | 192 | 202 | <mark> **188** </mark> | 332 | 408 |
| malloc (99.9th Percentile) | <mark> **216** </mark> | 242 | 236 | 404 | 506 |
| free (median) | <mark> **92** </mark> | <mark>**92**</mark> | <mark> **92** </mark> | 164 | 213 |
| free (99th Percentile) | <mark> **172** </mark> | <mark> **172** </mark> | 206 | 354 | 416 |

### Worst Case Under Fragmentation

This test has the heap filled with small blocks separated by allocated ones, and small blocks are freed. This leaves thousands of free blocks that are all too small for the next request (causing significant fragmentation). First-fit allocators have to iterate through all of them while TLSF can jump straight to a list that fits. Lower is better.

<img width="1520" height="784" alt="worst-case-dark" src="https://github.com/user-attachments/assets/e2bdbce3-e9e9-42ed-a08e-d2c7b2e2b115" />

| Unusable Free Blocks | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| 0 | 126 | 124 | 118 | <mark> **80** </mark> | <mark> **80** </mark> |
| 500 | 122 | 124 | <mark> **116** </mark> | 1,912 | 1,910 |
| 1,000 | 122 | 124 | <mark> **116** </mark> | 3,750 | 3,758 |
| 2,000 | 122 | 124 | <mark> **116** </mark> | 7,434 | 7,444 |
| 4,000 | 122 | 124 | <mark> **120** </mark> | 14,724 | 14,732 |

At 4,000 fragments MM (Standard) is 121x faster than FreeRTOS heap_4 and STM32's newlib-nano.

### Instructions per call on Cortex-M4
The throughput test above runs on a x86 PC, where branch prediction and caches give an advantage. On the Cortex-M4 (arm-none-eabi-gcc -mcpu=cortex-m4 -O2 using qemu-arm) it is different. The workload is the same as the throughput test (heap at 60% full). Lower is better.

On a Cortex-M4, most instructions take one cycle, so this is close to a cycle count, but loads, taken branches, and wait states on flash add some cycles on an actual board.

<img width="1760" height="784" alt="cortex-m4-dark" src="https://github.com/user-attachments/assets/fa882802-369c-434b-8a2a-1975ce72757c" />

| Instructions per Call | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| Small Blocks (8 B - 64 B) | <mark> **56** </mark> | 64 | 76 | 150 | 401 |
| Mixed Sizes (8 B - 1024 B) | <mark> **66** </mark> | 75 | 93 | 146 | 244 |
| Packet Buffers (16 B - 1024 B) | <mark> **65** </mark> | 72 | 93 | 126 | 163 |

The standard version runs 27-30% fewer instructions than the ESP TLSF. The hybrid version, however only runs 16-23% fewer instructions than the ESP TLSF. Most of the slowdown on the x86 PC comes from mispredicted branches. A Cortex-M4 CPU has no branch predictor so each branch costs one to three extra cycles. This is why MM is faster on a Arm Computer but slower on a x86 Computer.

### Code Size

Measured on a Cortex-M4 with arm-none-eabi-gcc -Os and unused functions removed for a program that calls malloc and free on a 64 KB heap.

| | **MM (Standard)** | **MM (Hybrid)** | TLSF (ESP32) | heap_4 (FreeRTOS) | newlib-nano (STM32) |
| --- | --- | --- | --- | --- | --- |
| Flash | 1,484 B | 1,544 B | 1,088 B | <mark>**384 B**</mark> | 644 B |
| RAM outside the heap | 1,200 B | 1,200 B | <mark>**0 B**</mark> | 28 B | 96 B |

Most of this allocator's RAM is the size-class table, and setting SL_BITS to 3 cuts the usage in half. The reference TLSF keeps a table too, but it palces its 3,188-byte table inside the heap instead, which is why it shows almost no RAM here and loses memory efficiency on small heaps.

## Extreme Testing

A separate stress test checks correctness, not speed. The heap and every free list after random operations is checked and confirms the heap's structure is correct. The heap is damaged before this to check if the test actually works.

| Test | What It Does | Fails If | Result |
| --- | --- | --- | --- |
| Random Use | 234 million malloc, calloc, realloc, and free operations with random sizes from 16 KB to 1 GB heaps | Any corrupted bytes, or blocks | 0 corrupted bytes or blocks |
| Running Out of Memory | Using small heaps near max capacity | malloc returns anything but NULL | 46 million failed requests and 9.4 million failed reallocs were handled correctly (by returning NULL) |
| Fill and Empty | Allocate until malloc fails and then free everything at a random order (3 times) | The heap does not merge back into one free block | merged back in all 34 runs |
| Edge Cases | Unusual Requests (listed below) | Any wrong return value or corrupted data | All Pass |
| Sanitizers | The tests above compiled with Address Sanitizer and UndefinedBehaviorSanitizer | The allocator reads or writes outside its own memory or does any undefined errors in C | 0 reports after 150 million operations |
| Heap Checker | Break the heap on purpose in multiple different ways | Checker misses any of them | All 8 of 8 caught |

All tests are run in both 32-bit and 64-bit builds (Standard Version). The edge cases are:
+ malloc(0) returns NULL, and free(NULL) does nothing.
+ Requests that could never fit (such as SIZE_MAX, 5 GB, or twice the heap size) return NULL.
+ A calloc whose count times size overflows returns NULL instead of a tiny block.
+ The largest block the heap can hold can be allocated and anything bigger is refused
+ With the heap completely full, even malloc(1) returns NULL.
+ realloc(NULL, n) acts like malloc, realloc(p, 0) frees the block, a realloc to an impossible size keeps the old data, and shrinking a block keeps it in place.
+ If FL_COUNT is too small for the heap, mm_init fails and every later malloc returns NULL.

## 4-byte Alignment Option

Building with -DMM_ALIGN4 on a 32-bit target puts every block on the 4-byte grid similar to how the ESP32's TLSF works. Nothing is reserved for 8-byte alignment, so it fits the most data of the three versions, but pointers are only guaranteed to be 4-byte aligned. That is below the 8 bytes that the C standard and the Arm ABI specifies for double, int64_t, and uint64_t. For Cortex-M3, M4, and M7, reads and writes for those types still work, but on 64-bit atonmics (LDREXD / STREXD) fault on a misaligned address (4-byte alignment). This should only be used if the program stores no 8-byte values that need their full alignment. On a 64-bit target, the flag is ignored.

The table compares it with the two main versions with the alignment check reduced to 4 bytes.

% Memory holding data when malloc first fails:

| | MM (Standard) | MM (Hybrid) | **MM (4-byte)** | TLSF (ESP32) |
| --- | --- | --- | --- | --- |
| Small Blocks (16 KB) | 79.2% | 80.7% | <mark> **81.8%** </mark> | 65.8% |
| Mixed Sizes (16 KB) | 86.9% | 87.2% | <mark> **87.8%** </mark> | 69.9% |
| Packet Buffers (16 KB) | 88.0% | 88.0% | <mark> **89.7%** </mark> | 71.9% |
| Small Blocks (64 KB) | 79.9% | 81.7% | <mark> **82.7%** </mark> | 78.7% |
| Mixed Sizes (64 KB) | 91.3% | 92.2% | <mark> **92.5%** </mark> | 87.3% |
| Packet Buffers (64 KB) | 92.3% | 92.3% | <mark> **93.8%** </mark> | 89.6% |
| Small Blocks (256 KB) | 80.2% | 82.2% | <mark> **83.2%** </mark> | 82.1% |
| Mixed Sizes (256 KB) | 92.9% | 93.7% | <mark> **94.0%** </mark> | 92.8% |
| Packet Buffers (256 KB) | 94.1% | 94.1% | <mark> **95.8%** </mark> | 95.0% |
| Small Blocks (512 KB) | 80.2% | 82.3% | <mark> **83.2%** </mark> | 82.7% |
| Mixed Sizes (512 KB) | 93.4% | 94.1% | <mark> **94.6%** </mark> | 93.9% |
| Packet Buffers (512 KB) | 94.5% | 94.5% | <mark> **96.3%** </mark> | 96.0% |

Failed mallocs with 64 KB heap:

| | MM (Standard) | MM (Hybrid) | **MM (4-byte)** | TLSF (ESP32) |
| --- | --- | --- | --- | --- |
| 80% Full (Small Blocks) | 98.8% | 37.0% | <mark> **0.0%** </mark> | 99.5% |
| 85% Full (Mixed Sizes) | 93.3% | 70.1% | <mark> **9.2%** </mark> | 98.7% |
| 85% Full (Packet Buffers) | 3.1% | 3.1% | <mark> **1.2%** </mark> | 11.5% |

Speed:

| | MM (Standard) | MM (Hybrid) | **MM (4-byte)** | TLSF (ESP32) |
| --- | --- | --- | --- | --- |
| Throughput (Small Blocks / Million Calls per Second) | <mark> **36.5** </mark> | 29.3 | 30.2 | 34.3 |
| Throughput (Mixed Sizes / Million Calls per Second) | <mark> **24.7** </mark> | 21.9 | 23.2 | 22.8 |
| Throughput (Packet Buffers / Million Calls per Second) | <mark> **23.1** </mark> | 22.9 | 22.0 | 21.5 |
| malloc (CPU cycles) | <mark> **118** </mark> | 130 | 126 | <mark> **118** </mark> |
| free (CPU cycles) | <mark> **92** </mark> | 94 | <mark> **92** </mark> | <mark> **92** </mark> |
| malloc (4,000 fragments / CPU cycles) | 122 | 124 | 120 | <mark> **118** </mark> |
| Cortex-M4 (Mixed Sizes / Instructions per Call) | <mark> **66** </mark> | 75 | 71 | 93 |

## Future Implementations
In the future I will most likely try to implement a multithreaded capable MM memory allocator version so that it could work under a RTOS. I will also most likely try to expand the block sizes because currently block sizes are stored in 32 bits so a single block can be at most about 4 GB. Lastly, I would like to implement something that allows me to not use GCC or Clang due to the \_\_builtin\_* functions







