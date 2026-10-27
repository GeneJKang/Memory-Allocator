#ifndef HEAP_H
#define HEAP_H
#include <stddef.h>

//mem == NULL (reserved a region from the OS)
//mem != NULL (manage a buffer that is passed in)
int heap_init(void *mem, size_t len);

//Claim the next incr bytes of the region
//Return the start of the claimed bytes
//Returns (void *)-1 on failure
void *heap_sbrk(size_t incr);

//First and last byte of the handed out heap
void *heap_lo(void);
void *heap_hi(void);

#endif