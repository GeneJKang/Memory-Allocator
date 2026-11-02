//TLSF segregated free list with bitmap lookup
//smallest fit boundary-tag coalescing
//realloc with trimming
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include "mm.h"
#include "heap.h"

/* Basic constants and macros */
#define WSIZE       4
#define DSIZE       8
#define CHUNKSIZE   (1 << 12)

#define MAX(x, y)   ((x) > (y) ? (x) : (y))
#define ALIGN(size) (((size) + (DSIZE - 1)) & ~0x7)

#define PACK(size, alloc)  ((size) | (alloc))

#define GET(p)       (*(unsigned int *)(p))
#define PUT(p, val)  (*(unsigned int *)(p) = (val))

#define GET_SIZE(p)  (GET(p) & ~0x7)
#define GET_ALLOC(p) (GET(p) & 0x1)

#define HDRP(bp)       ((char *)(bp) - WSIZE)
#define FTRP(bp)       ((char *)(bp) + GET_SIZE(HDRP(bp)) - DSIZE)

#define NEXT_BLKP(bp)  ((char *)(bp) + GET_SIZE(((char *)(bp) - WSIZE)))
#define PREV_BLKP(bp)  ((char *)(bp) - GET_SIZE(((char *)(bp) - DSIZE)))

//Header is 32 bits and low 3 bits are flag aka the biggest block that can be described
#define MAX_BLOCK ((size_t)(UINT32_MAX & ~0x7u))
//Biggest request that still fits after adding header+footer (DSIZE)
#define MAX_REQUEST (MAX_BLOCK - DSIZE)


//Pointers for linked list
//NEXT_FREE is offset 0 from bp and PREV_FREE offset DSIZE from bp
//Minimum size 24 bytes
//4 header + 8 ptr + 8 prev ptr + 4 footer

#define NEXT_FREE(bp) (*(char **)(bp))
#define PREV_FREE(bp) (*(char **)((char *)(bp) + DSIZE))


//List number
#define SL_BITS 2
#define SL_COUNT (1 << SL_BITS) //4 sub lists per power of 2
#define FL_COUNT 32 //one row per bit of a 32-bit size



/* Global pointer */
static char *heap_listp = NULL;



/* Helper function prototypes */
static void *extend_heap(size_t words);
static void *coalesce(void *bp);
static void *find_fit(size_t asize);
static void *place(void *bp, size_t asize);

//Functions for linked list
static void add_to_free_list(void *bp);
static void remove_from_free_list(void *bp);

//Data structures for free lists
static char *free_lists[FL_COUNT][SL_COUNT];    //head of each list
static uint32_t fl_bitmap;  //bit fl set means some list in row fl is non-empty
static uint32_t sl_bitmap[FL_COUNT]; //bit sl set means list (fl, sl) is non-empty


/*
 * mm_init - initialize the malloc package.
 */
int mm_init(void)
{
    if(heap_lo() == NULL && heap_init(NULL, 0) < 0){
        return -1;  //heap init failed
    }

    if ((heap_listp = heap_sbrk(4 * WSIZE)) == (void *)-1) {
        return -1;
    }

    PUT(heap_listp, 0);                              /* Alignment padding */
    PUT(heap_listp + (1 * WSIZE), PACK(DSIZE, 1));  /* Prologue header */
    PUT(heap_listp + (2 * WSIZE), PACK(DSIZE, 1));  /* Prologue footer */
    PUT(heap_listp + (3 * WSIZE), PACK(0, 1));      /* Epilogue header */

    heap_listp += (2 * WSIZE);

    //Empty every free list and clear the bitmaps
    memset(free_lists, 0, sizeof(free_lists));
    memset(sl_bitmap, 0, sizeof(sl_bitmap));
    fl_bitmap = 0;

    return 0;
}

static void mapping(size_t size, int *fl, int *sl){
    *fl = 31 - __builtin_clz((uint32_t)size);   //highest set bit
    *sl = (int)((size >> (*fl - SL_BITS)) & (SL_COUNT - 1));    //next 2 bits
}

static void *extend_heap(size_t words)
{
    char *bp;
    size_t size;

    size = (words % 2) ? (words + 1) * WSIZE : words * WSIZE;

    if ((long)(bp = heap_sbrk(size)) == -1) {
        return NULL;
    }

    PUT(HDRP(bp), PACK(size, 0));
    PUT(FTRP(bp), PACK(size, 0));
    PUT(HDRP(NEXT_BLKP(bp)), PACK(0, 1));

    return coalesce(bp);
}

static void *coalesce(void *bp)
{
    size_t prev_alloc = GET_ALLOC(FTRP(PREV_BLKP(bp)));
    size_t next_alloc = GET_ALLOC(HDRP(NEXT_BLKP(bp)));
    size_t size = GET_SIZE(HDRP(bp));

    if (prev_alloc && next_alloc) {
        //No merge needed, fall through to add_to_free_list function
    }
    else if (prev_alloc && !next_alloc) {

        remove_from_free_list(NEXT_BLKP(bp));   //Merge with next block

        size += GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(bp), PACK(size, 0));
        PUT(FTRP(bp), PACK(size, 0));
    }
    else if (!prev_alloc && next_alloc) {

        remove_from_free_list(PREV_BLKP(bp));   //Merge with prev block

        size += GET_SIZE(HDRP(PREV_BLKP(bp)));
        PUT(FTRP(bp), PACK(size, 0));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }
    else {

        remove_from_free_list(PREV_BLKP(bp));
        remove_from_free_list(NEXT_BLKP(bp));   //Merge prev and next block

        size += GET_SIZE(HDRP(PREV_BLKP(bp))) +
                GET_SIZE(HDRP(NEXT_BLKP(bp)));
        PUT(HDRP(PREV_BLKP(bp)), PACK(size, 0));
        PUT(FTRP(NEXT_BLKP(bp)), PACK(size, 0));
        bp = PREV_BLKP(bp);
    }

    add_to_free_list(bp);       //Always add resulting free block to free list
    return bp;
}

static void *find_fit(size_t asize)
{
    int fl, sl;
    mapping(asize, &fl, &sl);
    int home_fl = fl, home_sl = sl; //the list asize belongs to

    //round asize up to the start of the next sub range so every
    //block in the list we start from is guaranteed to fit
    size_t rounded = asize + ((size_t)1 << (fl - SL_BITS)) - 1;    
    if (rounded <= MAX_BLOCK){
        mapping(rounded, &fl, &sl);

        //list in row fl at or above sl
        uint32_t sl_map = sl_bitmap[fl] & (~0u << sl);

        if (sl_map == 0){
            //Nothing in the current row so look at rows above it
            uint32_t fl_map = (fl + 1 < 32) ? (fl_bitmap & (~0u << (fl + 1))) : 0;
            if (fl_map != 0){
                fl = __builtin_ctz(fl_map);
                sl_map = sl_bitmap[fl];
            }
        }
        if(sl_map != 0) {
            sl = __builtin_ctz(sl_map);
            return free_lists[fl][sl];   //any block fits here so no checking needed
        }
    }

    //Rounding can skip a block that fits so check asize's own list before giving up
    for (char *bp = free_lists[home_fl][home_sl]; bp != NULL; bp = NEXT_FREE(bp)) {
        if (asize <= GET_SIZE(HDRP(bp))) {
		    return bp;
        }
    }
    return NULL;
}

static size_t adjust_size(size_t size){
    if (size > MAX_REQUEST){    //check before adding overhead
        return 0;
    }
    if (size <= 2 * DSIZE){
        return 3 * DSIZE;   //minimum block
    }
    return ALIGN(size + DSIZE);
}





static void *place(void *bp, size_t asize)
{

    size_t csize = GET_SIZE(HDRP(bp));
    remove_from_free_list(bp);

    if ((csize - asize) >= (3 * DSIZE)) {

        //leftovers is big enough to be its own free block
        //split

        PUT(HDRP(bp), PACK(asize, 1));
        PUT(FTRP(bp), PACK(asize, 1));

        void *remainder = NEXT_BLKP(bp);
        PUT(HDRP(remainder), PACK(csize - asize, 0));            
	    PUT(FTRP(remainder), PACK(csize - asize, 0));
        add_to_free_list(remainder);
    } else {
        //Leftovers under 24 bytes cant hold the overhead
        //so caller gets the whole block
        PUT(HDRP(bp), PACK(csize, 1));
        PUT(FTRP(bp), PACK(csize, 1));
    }
    return bp;
}

//trim an allocated block
static void trim_block(void *bp, size_t asize){
    size_t csize = GET_SIZE(HDRP(bp));
    size_t leftover = csize - asize;

    if (leftover < (3 * DSIZE)) {   //Block is too small to be trimmed
        return;
    }

    // Shrink the block via new header
    PUT(HDRP(bp), PACK(asize, 1));
    PUT(FTRP(bp), PACK(asize, 1));

    //tail (remainder) starts right after the trimmed block
    void *remainder = NEXT_BLKP(bp);
    PUT(HDRP(remainder), PACK(leftover, 0));            
    PUT(FTRP(remainder), PACK(leftover, 0));
    
    //Coalesce the tail with free neighbor (if it exists) and adds to free list
    coalesce(remainder);

}

/*
 * mm_malloc - Allocate a block.
 */
void *mm_malloc(size_t size)
{
    size_t asize;
    size_t extendsize;
    char *bp;

    if (heap_listp == NULL && mm_init() < 0){   //init malloc
        return NULL;    //init malloc failed
    }

    if (size == 0) {
        return NULL;
    }
    if ((asize = adjust_size(size)) == 0){
        errno = ENOMEM;
        return NULL;
    }
    if ((bp = find_fit(asize)) != NULL) {
        return place(bp, asize);
    }    



        //No fit found so extend the heap
    extendsize = MAX(asize, CHUNKSIZE);
    if ((bp = extend_heap(extendsize / WSIZE)) == NULL) {
        return NULL;
    }

    return place(bp, asize);
}


//linked list adder/remover
static void add_to_free_list(void *bp){
		
    int fl, sl;
    mapping(GET_SIZE(HDRP(bp)), &fl, &sl);

    char *head = free_lists[fl][sl];    //LIFO insert per list
    NEXT_FREE(bp) = head;     //New block points to old head
    PREV_FREE(bp) = NULL;           //Nothing before the head

    if (head != NULL){
            PREV_FREE(head) = bp;     //Old head points back to new block
    }

    free_lists[fl][sl] = bp;

    fl_bitmap |= 1u << fl;
    sl_bitmap[fl] |= 1u << sl;

}


static void remove_from_free_list(void *bp){


    int fl, sl;
    mapping(GET_SIZE(HDRP(bp)), &fl, &sl);

    char *prev = PREV_FREE(bp);
    char *next = NEXT_FREE(bp);


    if (prev == NULL){
            free_lists[fl][sl] = next;      //bp was the head of the list
    } else{
            NEXT_FREE(prev) = next; //prev points to next
    }

    if (next != NULL){
            PREV_FREE(next) = prev; //next points to prev
    }

    if (free_lists[fl][sl] == NULL) {   //clear its bit because list is empty
        sl_bitmap[fl] &= ~(1u << sl);
        if (sl_bitmap[fl] == 0){ //whole row is empty so clear row bit
            fl_bitmap &= ~(1u << fl);
        }
    }

}






/*
 * mm_free - Free a block.
 */
void mm_free(void *ptr)
{
    if (ptr == NULL) {
        return;
    }

    size_t size = GET_SIZE(HDRP(ptr));
    PUT(HDRP(ptr), PACK(size, 0));
    PUT(FTRP(ptr), PACK(size, 0));
    coalesce(ptr);
}

//realloc
//grows in place when the next block is free or its at the end of the heap
void *mm_realloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return mm_malloc(size);
    }

    if (size == 0) {
        mm_free(ptr);
        return NULL;
    }

    size_t asize = adjust_size(size);
    if (asize == 0){    //too big so fail and leave old block untouched
        errno = ENOMEM;
        return NULL;
    }

    size_t oldsize = GET_SIZE(HDRP(ptr));

    if (oldsize >= asize){      
        trim_block(ptr, asize);                                                  
	    return ptr;
    }

    //Instead of reallocating check if the next block is free and combine
    //Check if next block is free and combine if size is enough
    void *next = NEXT_BLKP(ptr);
    size_t next_size = GET_SIZE(HDRP(next));
    size_t combined = oldsize + next_size;
    
    //Check if next block is free and combined size is big enough to fit
    if (!GET_ALLOC(HDRP(next)) && combined >= asize){
	    //Combine next free block
        remove_from_free_list(next);

        PUT(HDRP(ptr), PACK(combined, 1));
        PUT(FTRP(ptr), PACK(combined, 1));
        trim_block(ptr, asize);
        return ptr;
    }



    //Checks if block is at the end of the heap memory and if so just extend
    if (next_size == 0){
	size_t extend = asize - oldsize;
	if (heap_sbrk(extend) == (void *)-1) {
		return NULL;
	}
	
	PUT(HDRP(ptr), PACK(asize, 1));
	PUT(FTRP(ptr), PACK(asize, 1));
	PUT(HDRP(NEXT_BLKP(ptr)), PACK(0, 1));
	return ptr;
    }



    void *newptr = mm_malloc(size);
    if (newptr == NULL) {
        return NULL;
    }

    oldsize = GET_SIZE(HDRP(ptr)) - DSIZE;
    size_t copySize = (size < oldsize) ? size : oldsize;
    memcpy(newptr, ptr, copySize);
    mm_free(ptr);

    return newptr;
}

//contiguous allocation
//initialize all allocation to 0
void *mm_calloc(size_t n, size_t size){
    size_t total;
    //&total is a pointer to the value n * size
    //returns true if overflowed
    if (__builtin_mul_overflow(n, size, &total)){   //does not fit in size_t
        errno = ENOMEM;
        return NULL;
    }
    void *p = mm_malloc(total);
    if (p != NULL){
        memset(p, 0, total);
    }
    return p;
}



