#include "heap.h"
#include <errno.h>
#include <stdint.h>

#ifndef HEAP_SPACE
#define HEAP_SPACE ((size_t)1 << 30) //1 GB of virtual address space (pages are only used once)
#endif


static char *start, *pbrk_ptr /*program break*/, *end;

#if defined(__linux__)
#include <sys/mman.h>
//Ask OS for len bytes of addr space and reserve it
static void *os_reserve(size_t len){
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
#else
//Microcontroller must provide a buffer
static void *os_reserve(size_t len) {
    (void)len;
    return NULL;
}
#endif

int heap_init(void *mem, size_t len){
    if (mem == NULL){   //Reserve HEAP_SPACE
        len = HEAP_SPACE;
        mem = os_reserve(len);
    }
    if (mem == NULL){   //Check if a buffer was passed
        return -1;
    }


    //check if memory address region is misaligned
    if ((uintptr_t)mem % 8 != 0){
        return -1;
    }

    start = pbrk_ptr = mem; //No memory allocated yet so set both start and program break as heap start
    end = start + len;  //end of heap

    return 0;
}

void *heap_sbrk(size_t incr){
    //end - pbrk_ptr is how many bytes are left

    //Check if heap was initialized or  if theres enough room left
    if (start == NULL || incr > (size_t)(end - pbrk_ptr)){
        errno = ENOMEM;
        return (void *)-1;
    }

    char *old = pbrk_ptr;    //new bytes starts where the break is now
    pbrk_ptr += incr;
    return old;
}

//return current start of program VM
void *heap_lo(void){
    return start;
}

void *heap_hi(void){
    return pbrk_ptr - 1;    //pbrk_ptr is one past the last used byte
}
