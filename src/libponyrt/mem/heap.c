#include "heap.h"
#include "pagemap.h"
#include "../ds/fun.h"
#include "ponyassert.h"
#include <string.h>

#include <platform.h>
#include <dtrace.h>

typedef struct chunk_t
{
  // immutable
  pony_actor_t* actor;
  char* m;
  size_t size;

  // mutable
  uint32_t slots;
  uint32_t shallow;
  uint32_t finalisers;

  struct chunk_t* next;
} chunk_t;

typedef char block_t[POOL_ALIGN];
typedef void (*chunk_fn)(chunk_t* chunk, uint32_t mark);

#define SIZECLASS_SIZE(sizeclass) (HEAP_MIN << (sizeclass))
#define SIZECLASS_MASK(sizeclass) (~(SIZECLASS_SIZE(sizeclass) - 1))

#define EXTERNAL_PTR(p, sizeclass) \
  ((void*)((uintptr_t)p & SIZECLASS_MASK(sizeclass)))

#define FIND_SLOT(ext, base) \
  (1 << ((uintptr_t)((char*)(ext) - (char*)(base)) >> HEAP_MINBITS))

static const uint32_t sizeclass_empty[HEAP_SIZECLASSES] =
{
  0xFFFFFFFF,
  0x55555555,
  0x11111111,
  0x01010101,
  0x00010001
};

static const uint32_t sizeclass_init[HEAP_SIZECLASSES] =
{
  0xFFFFFFFE,
  0x55555554,
  0x11111110,
  0x01010100,
  0x00010000
};

static const uint8_t sizeclass_table[HEAP_MAX / HEAP_MIN] =
{
  0, 1, 2, 2, 3, 3, 3, 3,
  4, 4, 4, 4, 4, 4, 4, 4
};

static size_t heap_initialgc = 1 << 14;
static double heap_nextgc_factor = 2.0;

#ifdef USE_MEMTRACK
/** Get the memory used by the heap.
 */
size_t ponyint_heap_mem_size(heap_t* heap)
{
  // include memory that is in use by the heap but not counted as part of
  // `used` like `chunk_t`. also, don't include "fake used" for purposes of
  // triggering GC.
  return heap->mem_used;
}

/** Get the memory allocated by the heap.
 */
size_t ponyint_heap_alloc_size(heap_t* heap)
{
  return heap->mem_allocated;
}
#endif

static void large_pagemap(char* m, size_t size, chunk_t* chunk)
{
  ponyint_pagemap_set_bulk(m, chunk, size);
}

static void clear_chunk(chunk_t* chunk, uint32_t mark)
{
  chunk->slots = mark;
  chunk->shallow = mark;
}

static void final_small(chunk_t* chunk, uint32_t mark)
{
  // run any finalisers that need to be run
  void* p = NULL;

  uint32_t finalisers = chunk->finalisers;
  uint64_t bit = 0;

  // if there's a finaliser to run for a used slot
  while(finalisers != 0)
  {
    bit = __pony_ctz(finalisers);
    p = chunk->m + (bit << HEAP_MINBITS);

    // run finaliser
    pony_assert((*(pony_type_t**)p)->final != NULL);
    (*(pony_type_t**)p)->final(p);

    // clear finaliser in chunk
    chunk->finalisers &= ~((uint32_t)1 << bit);

    // clear bit just found in our local finaliser map
    finalisers &= (finalisers - 1);
  }
  (void)mark;
}

static void final_small_freed(chunk_t* chunk)
{
  // run any finalisers that need to be run for any newly freed slots
  void* p = NULL;

  uint32_t finalisers = chunk->finalisers & chunk->slots;
  chunk->finalisers = chunk->finalisers & ~chunk->slots;
  uint64_t bit = 0;

  // if there's a finaliser to run for a used slot
  while(finalisers != 0)
  {
    bit = __pony_ctz(finalisers);
    p = chunk->m + (bit << HEAP_MINBITS);

    // run finaliser
    pony_assert((*(pony_type_t**)p)->final != NULL);
    (*(pony_type_t**)p)->final(p);

    // clear bit just found in our local finaliser map
    finalisers &= (finalisers - 1);
  }
}

static void final_large(chunk_t* chunk, uint32_t mark)
{
  if(chunk->finalisers == 1)
  {
    // run finaliser
    pony_assert((*(pony_type_t**)chunk->m)->final != NULL);
    (*(pony_type_t**)chunk->m)->final(chunk->m);
    chunk->finalisers = 0;
  }
  (void)mark;
}

static void destroy_small(chunk_t* chunk, uint32_t mark)
{
  (void)mark;

  // run any finalisers that need running
  final_small(chunk, mark);

  ponyint_pagemap_set(chunk->m, NULL);
  POOL_FREE(block_t, chunk->m);
  POOL_FREE(chunk_t, chunk);
}

static void destroy_large(chunk_t* chunk, uint32_t mark)
{

  (void)mark;

  // run any finalisers that need running
  final_large(chunk, mark);

  large_pagemap(chunk->m, chunk->size, NULL);

  if(chunk->m != NULL)
    ponyint_pool_free(chunk->size, chunk->m);

  POOL_FREE(chunk_t, chunk);
}

static size_t sweep_small(chunk_t* chunk, chunk_t** avail, chunk_t** full,
#ifdef USE_MEMTRACK
  uint32_t empty, size_t size, size_t* mem_allocated, size_t* mem_used)
#else
  uint32_t empty, size_t size)
#endif
{
  size_t used = 0;
  chunk_t* next;

  while(chunk != NULL)
  {
    next = chunk->next;
    chunk->slots &= chunk->shallow;

    if(chunk->slots == 0)
    {
#ifdef USE_MEMTRACK
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += POOL_ALLOC_SIZE(block_t);
      *mem_used += sizeof(chunk_t);
      *mem_used += sizeof(block_t);
#endif
      used += sizeof(block_t);
      chunk->next = *full;
      *full = chunk;
    } else if(chunk->slots == empty) {
      destroy_small(chunk, 0);
    } else {
#ifdef USE_MEMTRACK
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += POOL_ALLOC_SIZE(block_t);
      *mem_used += sizeof(chunk_t);
      *mem_used += sizeof(block_t);
#endif
      used += (sizeof(block_t) -
        (__pony_popcount(chunk->slots) * size));

      // run finalisers for freed slots
      final_small_freed(chunk);

      // make chunk available for allocations only after finalisers have been
      // run to prevent premature reuse of memory slots by an allocation
      // required for finaliser execution
      chunk->next = *avail;
      *avail = chunk;
    }

    chunk = next;
  }

  return used;
}

#ifdef USE_MEMTRACK
static chunk_t* sweep_large(chunk_t* chunk, size_t* used, size_t* mem_allocated,
  size_t* mem_used)
#else
static chunk_t* sweep_large(chunk_t* chunk, size_t* used)
#endif
{
  chunk_t* list = NULL;
  chunk_t* next;

  while(chunk != NULL)
  {
    next = chunk->next;
    chunk->slots &= chunk->shallow;

    if(chunk->slots == 0)
    {
      chunk->next = list;
      list = chunk;
#ifdef USE_MEMTRACK
      *mem_allocated += POOL_ALLOC_SIZE(chunk_t);
      *mem_allocated += ponyint_pool_used_size(chunk->size);
      *mem_used += sizeof(chunk_t);
      *mem_used += chunk->size;
#endif
      *used += chunk->size;
    } else {
      destroy_large(chunk, 0);
    }

    chunk = next;
  }

  return list;
}

static void chunk_list(chunk_fn f, chunk_t* current, uint32_t mark)
{
  chunk_t* next;

  while(current != NULL)
  {
    next = current->next;
    f(current, mark);
    current = next;
  }
}

uint32_t ponyint_heap_index(size_t size)
{
  // size is in range 1..HEAP_MAX
  // change to 0..((HEAP_MAX / HEAP_MIN) - 1) and look up in table
  return sizeclass_table[(size - 1) >> HEAP_MINBITS];
}

void ponyint_heap_setinitialgc(size_t size)
{
  heap_initialgc = (size_t)1 << size;
}

void ponyint_heap_setnextgcfactor(double factor)
{
  if(factor < 1.0)
    factor = 1.0;

  DTRACE1(GC_THRESHOLD, factor);
  heap_nextgc_factor = factor;
}

void ponyint_heap_init(heap_t* heap)
{
  memset(heap, 0, sizeof(heap_t));
  heap->next_gc = heap_initialgc;
}

void ponyint_heap_destroy(heap_t* heap)
{
  chunk_list(destroy_large, heap->large, 0);

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_list(destroy_small, heap->small_free[i], 0);
    chunk_list(destroy_small, heap->small_full[i], 0);
  }
}

void ponyint_heap_final(heap_t* heap)
{
  chunk_list(final_large, heap->large, 0);

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_list(final_small, heap->small_free[i], 0);
    chunk_list(final_small, heap->small_full[i], 0);
  }
}

void* ponyint_heap_alloc(pony_actor_t* actor, heap_t* heap, size_t size)
{
  if(size == 0)
  {
    return NULL;
  } else if(size <= HEAP_MAX) {
    return ponyint_heap_alloc_small(actor, heap, ponyint_heap_index(size));
  } else {
    return ponyint_heap_alloc_large(actor, heap, size);
  }
}

void* ponyint_heap_alloc_final(pony_actor_t* actor, heap_t* heap, size_t size)
{
  if(size == 0)
  {
    return NULL;
  } else if(size <= HEAP_MAX) {
    return ponyint_heap_alloc_small_final(actor, heap,
      ponyint_heap_index(size));
  } else {
    return ponyint_heap_alloc_large_final(actor, heap, size);
  }
}

void* ponyint_heap_alloc_small(pony_actor_t* actor, heap_t* heap,
  uint32_t sizeclass)
{
  chunk_t* chunk = heap->small_free[sizeclass];
  void* m;

  // If there are none in this size class, get a new one.
  if(chunk != NULL)
  {
    // Clear and use the first available slot.
    uint32_t slots = chunk->slots;
    uint32_t bit = __pony_ctz(slots);
    slots &= ~(1 << bit);

    m = chunk->m + (bit << HEAP_MINBITS);
    chunk->slots = slots;

    if(slots == 0)
    {
      heap->small_free[sizeclass] = chunk->next;
      chunk->next = heap->small_full[sizeclass];
      heap->small_full[sizeclass] = chunk;
    }
  } else {
    chunk_t* n = (chunk_t*) POOL_ALLOC(chunk_t);
    n->actor = actor;
    n->m = (char*) POOL_ALLOC(block_t);
    n->size = sizeclass;
#ifdef USE_MEMTRACK
    heap->mem_used += sizeof(chunk_t);
    heap->mem_used += POOL_ALLOC_SIZE(block_t);
    heap->mem_used -= SIZECLASS_SIZE(sizeclass);
    heap->mem_allocated += POOL_ALLOC_SIZE(chunk_t);
    heap->mem_allocated += POOL_ALLOC_SIZE(block_t);
#endif

    // note that no finaliser needs to run
    n->finalisers = 0;

    // Clear the first bit.
    n->shallow = n->slots = sizeclass_init[sizeclass];
    n->next = NULL;

    ponyint_pagemap_set(n->m, n);

    heap->small_free[sizeclass] = n;
    chunk = n;

    // Use the first slot.
    m = chunk->m;
  }

#ifdef USE_MEMTRACK
  heap->mem_used += SIZECLASS_SIZE(sizeclass);
#endif
  heap->used += SIZECLASS_SIZE(sizeclass);
  return m;
}

void* ponyint_heap_alloc_small_final(pony_actor_t* actor, heap_t* heap,
  uint32_t sizeclass)
{
  chunk_t* chunk = heap->small_free[sizeclass];
  void* m;

  // If there are none in this size class, get a new one.
  if(chunk != NULL)
  {
    // Clear and use the first available slot.
    uint32_t slots = chunk->slots;
    uint32_t bit = __pony_ctz(slots);
    slots &= ~((uint32_t)1 << bit);

    m = chunk->m + (bit << HEAP_MINBITS);
    chunk->slots = slots;

    // note that a finaliser needs to run
    chunk->finalisers |= ((uint32_t)1 << bit);

    if(slots == 0)
    {
      heap->small_free[sizeclass] = chunk->next;
      chunk->next = heap->small_full[sizeclass];
      heap->small_full[sizeclass] = chunk;
    }
  } else {
    chunk_t* n = (chunk_t*) POOL_ALLOC(chunk_t);
    n->actor = actor;
    n->m = (char*) POOL_ALLOC(block_t);
    n->size = sizeclass;
#ifdef USE_MEMTRACK
    heap->mem_used += sizeof(chunk_t);
    heap->mem_used += POOL_ALLOC_SIZE(block_t);
    heap->mem_used -= SIZECLASS_SIZE(sizeclass);
    heap->mem_allocated += POOL_ALLOC_SIZE(chunk_t);
    heap->mem_allocated += POOL_ALLOC_SIZE(block_t);
#endif

    // note that a finaliser needs to run
    n->finalisers = 1;

    // Clear the first bit.
    n->shallow = n->slots = sizeclass_init[sizeclass];
    n->next = NULL;

    ponyint_pagemap_set(n->m, n);

    heap->small_free[sizeclass] = n;
    chunk = n;

    // Use the first slot.
    m = chunk->m;
  }

#ifdef USE_MEMTRACK
  heap->mem_used += SIZECLASS_SIZE(sizeclass);
#endif
  heap->used += SIZECLASS_SIZE(sizeclass);
  return m;
}

void* ponyint_heap_alloc_large(pony_actor_t* actor, heap_t* heap, size_t size)
{
  size = ponyint_pool_adjust_size(size);

  chunk_t* chunk = (chunk_t*) POOL_ALLOC(chunk_t);
  chunk->actor = actor;
  chunk->size = size;
  chunk->m = (char*) ponyint_pool_alloc(size);
#ifdef USE_MEMTRACK
  heap->mem_used += sizeof(chunk_t);
  heap->mem_used += chunk->size;
  heap->mem_allocated += POOL_ALLOC_SIZE(chunk_t);
  heap->mem_allocated += ponyint_pool_used_size(size);
#endif
  chunk->slots = 0;
  chunk->shallow = 0;

  // note that no finaliser needs to run
  chunk->finalisers = 0;

  large_pagemap(chunk->m, size, chunk);

  chunk->next = heap->large;
  heap->large = chunk;
  heap->used += chunk->size;

  return chunk->m;
}

void* ponyint_heap_alloc_large_final(pony_actor_t* actor, heap_t* heap,
  size_t size)
{
  size = ponyint_pool_adjust_size(size);

  chunk_t* chunk = (chunk_t*) POOL_ALLOC(chunk_t);
  chunk->actor = actor;
  chunk->size = size;
  chunk->m = (char*) ponyint_pool_alloc(size);
#ifdef USE_MEMTRACK
  heap->mem_used += sizeof(chunk_t);
  heap->mem_used += chunk->size;
  heap->mem_allocated += POOL_ALLOC_SIZE(chunk_t);
  heap->mem_allocated += ponyint_pool_used_size(size);
#endif
  chunk->slots = 0;
  chunk->shallow = 0;

  // note that a finaliser needs to run
  chunk->finalisers = 1;

  large_pagemap(chunk->m, size, chunk);

  chunk->next = heap->large;
  heap->large = chunk;
  heap->used += chunk->size;

  return chunk->m;
}

void* ponyint_heap_realloc(pony_actor_t* actor, heap_t* heap, void* p,
  size_t size)
{
  if(p == NULL)
    return ponyint_heap_alloc(actor, heap, size);

  chunk_t* chunk = ponyint_pagemap_get(p);

  // We can't realloc memory that wasn't pony_alloc'ed since we can't know how
  // much to copy from the previous location.
  pony_assert(chunk != NULL);

  size_t oldsize;

  if(chunk->size < HEAP_SIZECLASSES)
  {
    // Previous allocation was a ponyint_heap_alloc_small.
    void* ext = EXTERNAL_PTR(p, chunk->size);

    // If the new allocation is a ponyint_heap_alloc_small and the pointer is
    // not an internal pointer, we may be able to reuse this memory. If it is
    // an internal pointer, we know where the old allocation begins but not
    // where it ends, so we cannot reuse this memory.
    if((size <= HEAP_MAX) && (p == ext))
    {
      uint32_t sizeclass = ponyint_heap_index(size);

      // If the new allocation is the same size or smaller, return the old
      // one.
      if(sizeclass <= chunk->size)
        return p;
    }

    oldsize = SIZECLASS_SIZE(chunk->size) - ((uintptr_t)p - (uintptr_t)ext);
  } else {
    // Previous allocation was a ponyint_heap_alloc_large.
    if((size <= chunk->size) && (p == chunk->m))
    {
      // If the new allocation is the same size or smaller, and this is not an
      // internal pointer, return the old one. We can't reuse internal
      // pointers in large allocs for the same reason as small ones.
      return p;
    }

    oldsize = chunk->size - ((uintptr_t)p - (uintptr_t)chunk->m);
  }

  // Determine how much memory to copy.
  if(oldsize > size)
    oldsize = size;

  // Get new memory and copy from the old memory.
  void* q = ponyint_heap_alloc(actor, heap, size);
  memcpy(q, p, oldsize);
  return q;
}

void ponyint_heap_used(heap_t* heap, size_t size)
{
  heap->used += size;
}

bool ponyint_heap_startgc(heap_t* heap)
{
  if(heap->used <= heap->next_gc)
    return false;

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    uint32_t mark = sizeclass_empty[i];
    chunk_list(clear_chunk, heap->small_free[i], mark);
    chunk_list(clear_chunk, heap->small_full[i], mark);
  }

  chunk_list(clear_chunk, heap->large, 1);

  // reset used to zero
  heap->used = 0;
#ifdef USE_MEMTRACK
  heap->mem_allocated = 0;
  heap->mem_used = 0;
#endif
  return true;
}

bool ponyint_heap_mark(chunk_t* chunk, void* p)
{
  // If it's an internal pointer, we shallow mark it instead. This will
  // preserve the external pointer, but allow us to mark and recurse the
  // external pointer in the same pass.
  bool marked;

  if(chunk->size >= HEAP_SIZECLASSES)
  {
    marked = chunk->slots == 0;

    if(p == chunk->m)
      chunk->slots = 0;
    else
      chunk->shallow = 0;
  } else {
    // Calculate the external pointer.
    void* ext = EXTERNAL_PTR(p, chunk->size);

    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, chunk->m);

    // Check if it was already marked.
    marked = (chunk->slots & slot) == 0;

    // A clear bit is in-use, a set bit is available.
    if(p == ext)
      chunk->slots &= ~slot;
    else
      chunk->shallow &= ~slot;
  }

  return marked;
}

void ponyint_heap_mark_shallow(chunk_t* chunk, void* p)
{
  if(chunk->size >= HEAP_SIZECLASSES)
  {
    chunk->shallow = 0;
  } else {
    // Calculate the external pointer.
    void* ext = EXTERNAL_PTR(p, chunk->size);

    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, chunk->m);

    // A clear bit is in-use, a set bit is available.
    chunk->shallow &= ~slot;
  }
}

bool ponyint_heap_ismarked(chunk_t* chunk, void* p)
{
  if(chunk->size >= HEAP_SIZECLASSES)
    return (chunk->slots & chunk->shallow) == 0;

  // Shift to account for smallest allocation size.
  uint32_t slot = FIND_SLOT(p, chunk->m);

  // Check if the slot is marked or shallow marked.
  return (chunk->slots & chunk->shallow & slot) == 0;
}

void ponyint_heap_free(chunk_t* chunk, void* p)
{
  if(chunk->size >= HEAP_SIZECLASSES)
  {
    if(p == chunk->m)
    {
      // run finaliser if needed
      final_large(chunk, 0);

      ponyint_pool_free(chunk->size, chunk->m);
      chunk->m = NULL;
      chunk->slots = 1;
    }
    return;
  }

  // Calculate the external pointer.
  void* ext = EXTERNAL_PTR(p, chunk->size);

  if(p == ext)
  {
    // Shift to account for smallest allocation size.
    uint32_t slot = FIND_SLOT(ext, chunk->m);

    // check if there's a finaliser to run
    if((chunk->finalisers & slot) != 0)
    {
      // run finaliser
      (*(pony_type_t**)p)->final(p);

      // clear finaliser
      chunk->finalisers &= ~slot;
    }

    // free slot
    chunk->slots |= slot;
  }
}

void ponyint_heap_endgc(heap_t* heap)
{
  size_t used = 0;
#ifdef USE_MEMTRACK
  size_t mem_allocated = 0;
  size_t mem_used = 0;
#endif

  for(int i = 0; i < HEAP_SIZECLASSES; i++)
  {
    chunk_t* list1 = heap->small_free[i];
    chunk_t* list2 = heap->small_full[i];

    heap->small_free[i] = NULL;
    heap->small_full[i] = NULL;

    chunk_t** avail = &heap->small_free[i];
    chunk_t** full = &heap->small_full[i];

    size_t size = SIZECLASS_SIZE(i);
    uint32_t empty = sizeclass_empty[i];

#ifdef USE_MEMTRACK
    used += sweep_small(list1, avail, full, empty, size,
      &mem_allocated, &mem_used);
    used += sweep_small(list2, avail, full, empty, size,
      &mem_allocated, &mem_used);
#else
    used += sweep_small(list1, avail, full, empty, size);
    used += sweep_small(list2, avail, full, empty, size);
#endif
  }

#ifdef USE_MEMTRACK
  heap->large = sweep_large(heap->large, &used, &mem_allocated, &mem_used);
#else
  heap->large = sweep_large(heap->large, &used);
#endif

  // Foreign object sizes will have been added to heap->used already. Here we
  // add local object sizes as well and set the next gc point for when memory
  // usage has increased.
  heap->used += used;
#ifdef USE_MEMTRACK
  heap->mem_allocated += mem_allocated;
  heap->mem_used += mem_used;
#endif
  heap->next_gc = (size_t)((double)heap->used * heap_nextgc_factor);

  if(heap->next_gc < heap_initialgc)
    heap->next_gc = heap_initialgc;
}

pony_actor_t* ponyint_heap_owner(chunk_t* chunk)
{
  // FIX: false sharing
  // reading from something that will never be written
  // but is on a cache line that will often be written
  // called during tracing
  // actual chunk only needed for GC tracing
  // all other tracing only needs the owner
  // so the owner needs the chunk and everyone else just needs the owner
  return chunk->actor;
}

size_t ponyint_heap_size(chunk_t* chunk)
{
  if(chunk->size >= HEAP_SIZECLASSES)
    return chunk->size;

  return SIZECLASS_SIZE(chunk->size);
}

// C99 requires inline symbols to be present in a compilation unit for un-
// optimized code

uint32_t __pony_clzzu(size_t x);
uint32_t __pony_ffszu(size_t x);
uint32_t __pony_ctz(uint32_t x);
uint32_t __pony_ffsll(uint64_t x);
uint32_t __pony_clzll(uint64_t x);
uint32_t __pony_popcount(uint32_t x);
