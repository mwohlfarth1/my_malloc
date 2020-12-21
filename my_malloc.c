#include <my_malloc.h>
#include <printing.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

/* Pointer to the location of the heap prior to any sbrk calls */
void *g_base = NULL;

/* Pointer to the head of the free list */
header *g_freelist_head = NULL;

/* Mutex to ensure thread safety for the freelist */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Pointer to the second fencepost in the most recently allocated chunk from
 * the OS. Used for coalescing chunks
 */

header *g_last_fence_post = NULL;

/*
 * Pointer to the next block in the freelist after the block that was last
 * allocated. If the block pointed to is removed by coalescing, this shouuld be
 * updated to point to the next block after the removed block.
 */
header *g_next_allocate = NULL;

/*
 * Direct the compiler to run the init function before running main
 * this allows initialization of required globals
 */

static void init(void) __attribute__((constructor));

/*
 * Direct the compiler to ignore unused static functions.
 */
static void set_fenceposts(void *mem, size_t size) __attribute__((unused));
static void insert_free_block(header *h) __attribute__((unused));
static header *find_header(size_t size) __attribute__((unused));

/*
 * Allocate the first available block able to satisfy the request
 * (starting the search at g_freelist_head)
 */

static header *first_fit(size_t size_requested) {
  /* try to find the block. if found, 
  return a pointer to the header of that block. if not found,
  return NULL */
  if (g_freelist_head == NULL) {
    return NULL;
  }
  
  header *current_header = g_freelist_head;
  bool block_found = false;
  while (!block_found) {
    
    /* if the size is exactly correct, simply remove this block from the free list */
    if (TRUE_SIZE(current_header) == size_requested) {
      if ((current_header->size & 0b111) == ((state) ALLOCATED)) {
        if (current_header->next == NULL) {
          return NULL;
        }
        else {
          current_header = current_header->next;
          continue;
        }
      }
      /* play with pointers to maintain the integrity of the list */
      /* if this is the head, check if there is a next block */
      if (current_header == g_freelist_head) {
        if (current_header->next != NULL) {
          g_freelist_head = current_header->next;
          current_header->next = NULL;
          current_header->next->prev = NULL;
        }
      }
      /* if this isn't the head block, we know the prev isn't null */
      else {
        /* if the next block is NULL, this takes care of that case as well */
        (current_header->prev)->next = current_header->next;
        if (current_header->next != NULL) {
          (current_header->next)->prev = current_header->prev;
        }
      }
         
      /* now that we've fixed the pointers, */
      /* take the block out of the free list */
      current_header->size |= (state) ALLOCATED;

      return current_header;
    }
    else if (TRUE_SIZE(current_header) > size_requested) {

      /* determine what must be left over to split the block */
      int size_leftover_to_split = 2 * ALLOC_HEADER_SIZE; 
      if (MIN_ALLOCATION >= ALLOC_HEADER_SIZE) {
        size_leftover_to_split += ALLOC_HEADER_SIZE;
      }

      /* if the leftover data is large enough for another block, */
      /* split the block */
      if ((TRUE_SIZE(current_header) - size_requested - ALLOC_HEADER_SIZE) >=
         size_leftover_to_split) {
        
        /* split off the leftover block from the newly allocated block */
        header *new_block = ((header *)(((char *) current_header) + 
                            size_requested + ALLOC_HEADER_SIZE));
        new_block->size = current_header->size - size_requested - ALLOC_HEADER_SIZE;
        new_block->size |= (state) UNALLOCATED;

        current_header->size -= (TRUE_SIZE(new_block) + ALLOC_HEADER_SIZE); 
        current_header->size |= (state) ALLOCATED;

        new_block->left_size = TRUE_SIZE(current_header);

        header *to_return = current_header;

        /* set the left_size of the block after the new block correctly */
        header *new_blocks_right_neighbor = (header *) (((char *) new_block) + ALLOC_HEADER_SIZE + TRUE_SIZE(new_block));
        new_blocks_right_neighbor->left_size = TRUE_SIZE(new_block);

        /* remove the current block from the freelist */
        if (current_header == g_freelist_head) {
          if (current_header->next == NULL) {
            g_freelist_head = NULL;
          }
          else {
            g_freelist_head = current_header->next;
            g_freelist_head->prev = NULL;
          }
        }

        insert_free_block(new_block);
        /* return this newly allocated block */
        return to_return;
      }
      /* if the leftover data is not large enough, just allocate this */
      /* whole block */
      else {
        /* play with pointers to maintain the free list */

        /* if this is the head, check if there is a next block */
        if (current_header == g_freelist_head) {
          if (current_header->next != NULL) {
            g_freelist_head = current_header->next;
          }
        }
        /* if this isn't the head block, we know the prev isn't null */
        else {
          /* if the next block is NULL, this takes care of that case as well */
          (current_header->prev)->next = current_header->next;
        }

        /* allocate the whole block */
        current_header->size |= (state) ALLOCATED;

        return current_header;
      }
    }
    else if (TRUE_SIZE(current_header) < size_requested) {
      /* look for the next block since this isn't big enough */

      /* if the next block is NULL, just return NULL */
      if (current_header->next == NULL) {
        return NULL;
      }
      /* otherwise, increment to the next block and check that */
      else {
        current_header = current_header->next;
      }
    }

  }

  return NULL;
  
} /* first_fit() */

/*
 *  Allocate the first available block able to satisfy the request
 *  (starting the search at the next free header after the header that was most
 *  recently allocated)
 */

static header *next_fit(size_t size) {
  /* try to find the block. if found, 
  return a pointer to the header of that block. if not found,
  return NULL */
  if (g_freelist_head == NULL) {
    return NULL;
  }
  
  header *current_header = g_next_allocate;
  if (g_next_allocate == NULL) {
    current_header = g_freelist_head;
  }
  
  /* keeps track of whether or not we've looped to the beginning of the */
  /* list yet */
  bool looped = false;

  bool block_found = false;
  while (!block_found) {
    printf("current_block:\n");
    print_object(current_header);

    /* if the size is exactly correct, simply remove this block from the free list */
    if (TRUE_SIZE(current_header) == size) {
      if ((current_header->size & 0b111) == ((state) ALLOCATED)) {
        if (current_header->next == NULL) {
          return NULL;
        }
        else {
          current_header = current_header->next;
          continue;
        }
      }

      /* set g_next_allocate to the next free block after this one */
      g_next_allocate = current_header->next;

      /* play with pointers to maintain the integrity of the list */
      /* if this is the head, check if there is a next block */
      if (current_header == g_freelist_head) {
        if (current_header->next != NULL) {
          g_freelist_head = current_header->next;
          current_header->next = NULL;
          current_header->next->prev = NULL;
        }
      }
      /* if this isn't the head block, we know the prev isn't null */
      else {
        /* if the next block is NULL, this takes care of that case as well */
        (current_header->prev)->next = current_header->next;
        if (current_header->next != NULL) {
          (current_header->next)->prev = current_header->prev;
        }
      }
         
      /* now that we've fixed the pointers, */
      /* take the block out of the free list */
      current_header->size |= (state) ALLOCATED;

      return current_header;
    }
    else if (TRUE_SIZE(current_header) > size) {

      /* determine what must be left over to split the block */
      int size_leftover_to_split = 2 * ALLOC_HEADER_SIZE; 
      if (MIN_ALLOCATION >= ALLOC_HEADER_SIZE) {
        size_leftover_to_split += ALLOC_HEADER_SIZE;
      }

      /* if the leftover data is large enough for another block, */
      /* split the block */
      if ((TRUE_SIZE(current_header) - size - ALLOC_HEADER_SIZE) >=
         size_leftover_to_split) {
        
        /* split off the leftover block from the newly allocated block */
        header *new_block = ((header *)(((char *) current_header) + 
                            size + ALLOC_HEADER_SIZE));
        new_block->size = current_header->size - size - ALLOC_HEADER_SIZE;
        new_block->size |= (state) UNALLOCATED;

        current_header->size -= (TRUE_SIZE(new_block) + ALLOC_HEADER_SIZE); 
        current_header->size |= (state) ALLOCATED;

        new_block->left_size = TRUE_SIZE(current_header);

        header *to_return = current_header;

        /* set the left_size of the block after the new block correctly */
        header *new_blocks_right_neighbor = (header *) (((char *) new_block) + ALLOC_HEADER_SIZE + TRUE_SIZE(new_block));
        new_blocks_right_neighbor->left_size = TRUE_SIZE(new_block);

        /* remove the current block from the freelist */
        if (current_header == g_freelist_head) {
          if (current_header->next == NULL) {
            g_freelist_head = NULL;
          }
          else {
            g_freelist_head = current_header->next;
            g_freelist_head->prev = NULL;
          }
        }
        else {
          /* if the next pointer of the right block is NULL, then we */
          /* simply have to set the next pointer of the prev block to NULL */
          if (current_header->next == NULL) {
            (current_header->prev)->next = NULL;
            current_header->prev = NULL;
          }
          /* if the next pointer is not NULL, then we need to maintain */
          /* the integrity of the list */
          else {
            current_header->prev->next = current_header->next;
            current_header->next->prev = current_header->prev;
            current_header->prev = NULL;
            current_header->next = NULL;
          }
        }
        
        insert_free_block(new_block);

        /* set g_next_allocate to the new block */
        g_next_allocate = new_block;

        /* return this newly allocated block */
        return to_return;
      }
      /* if the leftover data is not large enough, just allocate this */
      /* whole block */
      else {
        /* play with pointers to maintain the free list */

        /* if this is the head, check if there is a next block */
        if (current_header == g_freelist_head) {
          if (current_header->next != NULL) {
            g_freelist_head = current_header->next;
          }
        }
        /* if this isn't the head block, we know the prev isn't null */
        else {
          /* if the next block is NULL, this takes care of that case as well */
          (current_header->prev)->next = current_header->next;
        }

        /* allocate the whole block */
        current_header->size |= (state) ALLOCATED;

        /* set g_next_allocate appropriately */
        g_next_allocate = current_header->next;

        return current_header;
      }
    }
    else if (TRUE_SIZE(current_header) < size) {
      /* look for the next block since this isn't big enough */

      /* if the next block is NULL, check if we've already looped */
      if (current_header->next == NULL) {
        /* if this is the last block and we've already looped, then there */
        /* is not a big enough block in the list */
        if (looped) {
          return NULL;
        }
        /* if we haven't looped yet, then we should */
        else {
          current_header = g_freelist_head;
          looped = true;
        }
      }
      /* otherwise, increment to the next block and check that */
      else {
        current_header = current_header->next;
      }
    }

  }

  return NULL;
 
} /* next_fit() */

/*
 * best_fit
 * Allocate the FIRST instance of the smallest block able to satisfy the
 * request
 */

static header *best_fit(size_t size) {
  printf("size requested: %ld\n", (long int) size);

  header *current_free_block = g_freelist_head;
  header *to_return = NULL;
  while (current_free_block != NULL) {
    /* if the size of this block is exactly the size requested, */
    /* remove it from the list and return it to the user */
    if (TRUE_SIZE(current_free_block) == size) {
      to_return = current_free_block;
      break;
    }
    /* if the size is bigger than requested, keep traversing the list to */
    /* see if there is a smaller block available that still meets the */
    /* user's request */
    else if (TRUE_SIZE(current_free_block) > size) {
      /* if this is the first block we've found that is big enough, set */
      /* to be to_return and check the rest of the list */
      if (to_return == NULL) {
        to_return = current_free_block;
      }
      /* if we already found a block that was big enough */
      else {
        /* if this block is smaller than that block is, then this is */
        /* the block we want to return */
        if (TRUE_SIZE(current_free_block) < TRUE_SIZE(to_return)) {
          to_return = current_free_block;
        }
      }
    }

    /* now check the next block */
    current_free_block = current_free_block->next;
  }

  /* if we made it through the whole list and still didn't find anything to */
  /* give to the user, then return NULL */
  if (to_return == NULL) {
    return NULL;
  }
  /* otherwise, allocate this block, remove from the list, and give to user */
  else if (TRUE_SIZE(to_return) >= size) {
    printf("the block we want to remove is:\n");
    print_object(to_return);
    /* determine what must be left over to split the block */
    int size_leftover_to_split = 2 * ALLOC_HEADER_SIZE; 
    if (MIN_ALLOCATION >= ALLOC_HEADER_SIZE) {
      size_leftover_to_split += ALLOC_HEADER_SIZE;
    }
    printf("size leftover to split: %d\n", size_leftover_to_split);

    /* if the leftover data is large enough for another block, */
    /* split the block */
    int leftover_space = (TRUE_SIZE(to_return) - size - ALLOC_HEADER_SIZE);
    if (leftover_space >= size_leftover_to_split) {
      printf("splitting the block\n");
      
      /* split off the leftover block from the newly allocated block */
      header *new_block = ((header *)(((char *) to_return) + 
                          size + ALLOC_HEADER_SIZE));
      new_block->size = to_return->size - size - ALLOC_HEADER_SIZE;
      new_block->size |= (state) UNALLOCATED;

      to_return->size -= (TRUE_SIZE(new_block) + ALLOC_HEADER_SIZE); 
      to_return->size |= (state) ALLOCATED;

      new_block->left_size = TRUE_SIZE(to_return);

      /* set the left_size of the block after the new block correctly */
      header *new_blocks_right_neighbor = (header *) (((char *) new_block) + ALLOC_HEADER_SIZE + TRUE_SIZE(new_block));
      new_blocks_right_neighbor->left_size = TRUE_SIZE(new_block);

      /* remove the current block from the freelist */
      if (to_return == g_freelist_head) {
        if (to_return->next == NULL) {
          g_freelist_head = NULL;
        }
        else {
          g_freelist_head = to_return->next;
          g_freelist_head->prev = NULL;
        }
      }
      else {
        /* if the next pointer of the right block is NULL, then we */
        /* simply have to set the next pointer of the prev block to NULL */
        if (to_return->next == NULL) {
          (to_return->prev)->next = NULL;
          to_return->prev = NULL;
        }
        /* if the next pointer is not NULL, then we need to maintain */
        /* the integrity of the list */
        else {
          to_return->prev->next = to_return->next;
          to_return->next->prev = to_return->prev;
          to_return->prev = NULL;
          to_return->next = NULL;
        }
      }

      insert_free_block(new_block);

      /* return this newly allocated block */
      return to_return;
    }
    /* if the leftover data is not large enough to split, just allocate this */
    /* whole block */
    else {
      printf("this block is not large enough to split, just going to allocate it\n");
      /* play with pointers to maintain the free list */

      /* if this is the head, check if there is a next block */
      if (to_return == g_freelist_head) {
        if (to_return->next != NULL) {
          g_freelist_head = to_return->next;
        }
      }
      /* if this isn't the head block, we know the prev isn't null */
      else {
        /* if the next block is NULL, this takes care of that case as well */
        (to_return->prev)->next = to_return->next;
        if (to_return->next != NULL) {
          to_return->next->prev = to_return->prev;
        }
      }

      /* allocate the whole block */
      to_return->size |= (state) ALLOCATED;

      return to_return;
    } 
  }
  /* the largest block was not large enough for the user, so return NULL */
  else {
    printf("the block was not large enough to give to the user at all\n");
    return NULL;
  }
 
} /* best_fit() */

/*
 * worst_fit
 */

static header *worst_fit(size_t size) {

  header *biggest_block = NULL;
  int biggest_block_size = 0;
  header *current_free_block = g_freelist_head;

  while (current_free_block != NULL) {
    if (TRUE_SIZE(current_free_block) > biggest_block_size) {
      biggest_block = current_free_block;
      biggest_block_size = TRUE_SIZE(current_free_block);
    }
    current_free_block = current_free_block->next;
  }

  /* if the biggest_block is NULL, then the list was empty */
  if (biggest_block == NULL) {
    return NULL;
  }
  /* if the biggest block is big enough to satisfy the request, return it */
  else if (((size_t)biggest_block_size) >= size) {
    /* determine what must be left over to split the block */
    int size_leftover_to_split = 2 * ALLOC_HEADER_SIZE; 
    if (MIN_ALLOCATION >= ALLOC_HEADER_SIZE) {
      size_leftover_to_split += ALLOC_HEADER_SIZE;
    }

    /* if the leftover data is large enough for another block, */
    /* split the block */
    if ((TRUE_SIZE(biggest_block) - size - ALLOC_HEADER_SIZE) >=
       size_leftover_to_split) {
      
      /* split off the leftover block from the newly allocated block */
      header *new_block = ((header *)(((char *) biggest_block) + 
                          size + ALLOC_HEADER_SIZE));
      new_block->size = biggest_block->size - size - ALLOC_HEADER_SIZE;
      new_block->size |= (state) UNALLOCATED;

      biggest_block->size -= (TRUE_SIZE(new_block) + ALLOC_HEADER_SIZE); 
      biggest_block->size |= (state) ALLOCATED;

      new_block->left_size = TRUE_SIZE(biggest_block);

      /* set the left_size of the block after the new block correctly */
      header *new_blocks_right_neighbor = (header *) (((char *) new_block) + ALLOC_HEADER_SIZE + TRUE_SIZE(new_block));
      new_blocks_right_neighbor->left_size = TRUE_SIZE(new_block);

      /* remove the current block from the freelist */
      if (biggest_block == g_freelist_head) {
        if (biggest_block->next == NULL) {
          g_freelist_head = NULL;
        }
        else {
          g_freelist_head = biggest_block->next;
          g_freelist_head->prev = NULL;
        }
      }
      else {
        /* if the next pointer of the right block is NULL, then we */
        /* simply have to set the next pointer of the prev block to NULL */
        if (biggest_block->next == NULL) {
          (biggest_block->prev)->next = NULL;
          biggest_block->prev = NULL;
        }
        /* if the next pointer is not NULL, then we need to maintain */
        /* the integrity of the list */
        else {
          biggest_block->prev->next = biggest_block->next;
          biggest_block->next->prev = biggest_block->prev;
          biggest_block->prev = NULL;
          biggest_block->next = NULL;
        }
      }

      insert_free_block(new_block);

      /* return this newly allocated block */
      return biggest_block;
    }
    /* if the leftover data is not large enough to split, just allocate this */
    /* whole block */
    else {
      /* play with pointers to maintain the free list */

      /* if this is the head, check if there is a next block */
      if (biggest_block == g_freelist_head) {
        if (biggest_block->next != NULL) {
          g_freelist_head = biggest_block->next;
        }
      }
      /* if this isn't the head block, we know the prev isn't null */
      else {
        /* if the next block is NULL, this takes care of that case as well */
        (biggest_block->prev)->next = biggest_block->next;
      }

      /* allocate the whole block */
      biggest_block->size |= (state) ALLOCATED;

      return biggest_block;
    } 
  }
  /* the largest block was not large enough for the user, so return NULL */
  else {
    return NULL;
  }

} /* worst_fit() */

/*
 * Returns the address of the block to allocate
 * based on the specified algorithm.
 *
 * If no block is available, returns NULL.
 */

static header *find_header(size_t size) {
  if (g_freelist_head == NULL) {
    return NULL;
  }

  switch (FIT_ALGORITHM) {
    case 1:
      return first_fit(size);
    case 2:
      return next_fit(size);
    case 3:
      return best_fit(size);
    case 4:
      return worst_fit(size);
  }
  assert(false);
} /* find_header() */

/*
 * Calculates the location of the left neighbor given a header.
 */

static inline header *left_neighbor(header *h) {
  return (header *) (((char *) h) - h->left_size - ALLOC_HEADER_SIZE);
} /* left_neighbor() */

/*
 * Calculates the location of the right neighbor given a header.
 */

static inline header *right_neighbor(header *h) {
  return (header *) (((char *) h) + ALLOC_HEADER_SIZE + TRUE_SIZE(h));
} /* right_neighbor() */

/*
 * Insert a block at the beginning of the freelist.
 * The block is located after its left header, h.
 */

static void insert_free_block(header *h) {

  h->prev = NULL;

  if (g_freelist_head != NULL) {
    g_freelist_head->prev = h;
  }

  h->next = g_freelist_head;
  g_freelist_head = h;
} /* insert_free_block() */

/*
 * Instantiates fenceposts at the left and right side of a block.
 */

static void set_fenceposts(void *mem, size_t size) {
  header *left_fence = (header *) mem;
  header *right_fence = (header *) (((char *) mem) +
                         (size - ALLOC_HEADER_SIZE));

  left_fence->size = (state) FENCEPOST;
  right_fence->size = (state) FENCEPOST;

  right_fence->left_size = size - 3 * ALLOC_HEADER_SIZE;

} /* set_fenceposts() */

/*
 * Constructor that runs before main() to initialize the library.
 */

static void init() {
  g_freelist_head = NULL;

  /* Initialize mutex for thread safety */

  pthread_mutex_init(&g_mutex, NULL);

  /* Manually set printf buffer so it won't call malloc */

  setvbuf(stdout, NULL, _IONBF, 0);

  /* Record the starting address of the heap */

  g_base = sbrk(0);
} /* init() */

/*
 * malloc
 */

void *my_malloc(size_t size) {
  pthread_mutex_lock(&g_mutex);
  
  /* if requested size is 0, then return a null pointer */
  if (size == 0) {
    return NULL;
  }

  /* round up the size to the nearest multiple of MIN_ALLOCATION */
  int block_size_needed = ((size / MIN_ALLOCATION) * MIN_ALLOCATION);
  if ((size % MIN_ALLOCATION) != 0) {
    block_size_needed += MIN_ALLOCATION;
  }
  if (size < MIN_ALLOCATION) {
    block_size_needed = MIN_ALLOCATION;
  }
  if (block_size_needed < (2 * sizeof(header *))) {
    block_size_needed = 2 * sizeof(header *);
  }

  /* try to find a block in the free list that's big enough */
  do {
    header *found_block_header = find_header(block_size_needed);

    /* if we found a block */
    if (found_block_header != NULL) {
      return ((void *)(((char *) found_block_header) + ALLOC_HEADER_SIZE));
    }

    /* if we didn't find a block, need to request more memory */
    else {

      /* ask for a new chunk from sbrk() */
      char *ptr_to_new_chunk = sbrk(ARENA_SIZE);
      if (((void *) ptr_to_new_chunk) == ((void *) -1)) {
        errno = ENOMEM;
        return NULL;
      }

      /* set fenceposts on the new chunk of data */
      set_fenceposts(ptr_to_new_chunk, ARENA_SIZE);
      g_last_fence_post = (header *) (ptr_to_new_chunk + ARENA_SIZE - ALLOC_HEADER_SIZE);

      /* check to see if the new chunk is contiguous with the one next to it */
      /* if it is, coalesce the two chunks of data */
      
      header *new_chunk_l_fencepost = (header *) ptr_to_new_chunk;

      /* only need to check for fencepost to the left if */
      /* we've sbrk'd more than one chunk */
      if (new_chunk_l_fencepost != g_base) {

        /* if the left neighbor of this chunk is a fencepost, then we should */
        /* coalesce the fenceposts */
        if ((left_neighbor(new_chunk_l_fencepost)->size & 0b111) == ((state) FENCEPOST)) {

          header * previous_block = left_neighbor(left_neighbor(new_chunk_l_fencepost));
          previous_block->size += (2 * ALLOC_HEADER_SIZE);

          /* if the previous block that we just added the fenceposts to is */
          /* in the free list, then we can just merge that block with the */
          /* block that we just added */
          if ((previous_block->size & 0b111) == ((state) UNALLOCATED)) {
            previous_block->size += (ARENA_SIZE - (2 * ALLOC_HEADER_SIZE));
            right_neighbor((header *)ptr_to_new_chunk)->left_size = TRUE_SIZE(previous_block);
          }
          /* otherwise, we need to create a new entry in the free list */
          /* for our new block */
          else {
            /* add the header to the chunk */
            
            /* the address of the header is the left fencepost + */
            /* ALLOC_HEADER_SIZE */
            header *new_chunk_header = (header *) (ptr_to_new_chunk + ALLOC_HEADER_SIZE);   
            new_chunk_header->size = ARENA_SIZE - (3 * ALLOC_HEADER_SIZE); 
            new_chunk_header->size |= (state) UNALLOCATED;
            new_chunk_header->left_size = previous_block->size;
            new_chunk_header->next = NULL;
            new_chunk_header->prev = NULL;
            
            /* add this new chunk to the free list */
            insert_free_block(new_chunk_header);
          }
        }
        /* if the left neighbor isn't a fencepost, then we need to make */
        /* a new entry in the free list */
        else {
            /* the address of the header is the left fencepost + */
            /* ALLOC_HEADER_SIZE */
            header *new_chunk_header = (header *) (ptr_to_new_chunk + ALLOC_HEADER_SIZE);   
            new_chunk_header->size = ARENA_SIZE - (3 * ALLOC_HEADER_SIZE); 
            new_chunk_header->size |= (state) UNALLOCATED;
            new_chunk_header->left_size = 0;
            new_chunk_header->next = NULL;
            new_chunk_header->prev = NULL;
        
            /* add this new chunk to the free list */
            insert_free_block(new_chunk_header);

        }
      }
      /* if this is the first chunk we've asked for, add to free list */
      else {
        /* add the header to the chunk */
        header *new_chunk_header = (header *) (ptr_to_new_chunk + ALLOC_HEADER_SIZE);   
        new_chunk_header->size = ARENA_SIZE - (3 * ALLOC_HEADER_SIZE); 
        new_chunk_header->size |= (state) UNALLOCATED;
        new_chunk_header->next = NULL;
        new_chunk_header->prev = NULL;
        
        /* add this new chunk to the free list */
        insert_free_block(new_chunk_header);

      }
    }
  } while (true);
 
  pthread_mutex_unlock(&g_mutex);

  return NULL; 
} /* my_malloc() */

/*
 * free
 */

void my_free(void *p) {
  pthread_mutex_lock(&g_mutex);

  /* my code starts here */

  /* if a null pointer was given, do nothing */
  if (p == NULL) {
    return;
  }
  else { 
    header *block_to_free = (header *) ((char *)(p - ALLOC_HEADER_SIZE));

    /* if the user is trying to free a block that is already free or was */
    /* never allocated, there is an error */
    if ((block_to_free->size & 0b111) == ((state) UNALLOCATED)) {
    assert(false);
    return;
    }
    /* if the user entered a valid block to free, we might need to coalesce */
    else {
      /* get the right and left neighbors of this block */
      header *right_block = right_neighbor(block_to_free);
      header *left_block = left_neighbor(block_to_free);

      /* set this newly freed block to UNALLOCATED */
      /* don't ask me why "|= (state) UNALLOCATED" didn't work here, */
      /* i had to use TRUE_SIZE instead */
      block_to_free->size = TRUE_SIZE(block_to_free);

      /* if neither the right or left neighbors of this block are unallocated */
      /* then simply insert this new block into the free list */
      if (((right_block->size & 0b111) != ((state) UNALLOCATED)) &&
          ((left_block->size & 0b111) != ((state) UNALLOCATED))) {

        /* insert this block into the free list */
        insert_free_block(block_to_free);
      }
      /* if the right neighbor is also an unallocated block, */
      /* coalesce the blocks */ 
      else if (((right_block->size & 0b111) == ((state) UNALLOCATED)) &&
               ((left_block->size & 0b111) != ((state) UNALLOCATED))) {
        /* remove the right block from the list */
        /* if this block is the head of the list */
        if (right_block == g_freelist_head) {
          if (right_block->next == NULL) {
            g_freelist_head = NULL;
          }
          else {
            g_freelist_head = right_block->next;
            g_freelist_head->prev = NULL;
          }
        }
        else {
          /* if the next pointer of the right block is NULL, then we */
          /* simply have to set the next pointer of the prev block to NULL */
          if (right_block->next == NULL) {
            (right_block->prev)->next = NULL;
            right_block->prev = NULL;
          }
          /* if the next pointer is not NULL, then we need to maintain */
          /* the integrity of the list */
          else {
            right_block->prev->next = right_block->next;
            right_block->next->prev = right_block->prev;
            right_block->prev = NULL;
            right_block->next = NULL;
          }
        }
        
        /* increase this block's size to include the block to the right */
        block_to_free->size += TRUE_SIZE(right_block) + ALLOC_HEADER_SIZE;

        /* update the left_size of block to the right of the right block */
        right_neighbor(right_block)->left_size = TRUE_SIZE(block_to_free);

        /* insert this newly coalesced block into the free list */
        insert_free_block(block_to_free);

        if (g_next_allocate == right_block) {
          /* set g_next_allocate appropriately */
          g_next_allocate = block_to_free;
        }

      }
      /* if the left neighbor is also an unallocated block, */
      /* coalesce the blocks */
      else if (((right_block->size & 0b111) != ((state) UNALLOCATED)) &&
               ((left_block->size & 0b111) == ((state) UNALLOCATED))) {
        /* increase the size of the left block to include the new block */
        left_block->size += TRUE_SIZE(block_to_free) + ALLOC_HEADER_SIZE;

        /* update the left size of the block to the right of the new block */
        (right_neighbor(block_to_free))->left_size = TRUE_SIZE(left_block);
      }
      /* if both neighbors are unallocated, then coalesce all 3 */
      else if (((right_block->size & 0b111) == ((state) UNALLOCATED)) &&
               ((left_block->size & 0b111) == ((state) UNALLOCATED))) {
        /* increase the size of the left block to include both the */
        /* current block and the right block */
        left_block->size += (TRUE_SIZE(block_to_free) + ALLOC_HEADER_SIZE) +
                            (TRUE_SIZE(right_block) + ALLOC_HEADER_SIZE);
                              
        /* update the left size of the block on the right of the right */
        /* block */
        (right_neighbor(right_block))->left_size = TRUE_SIZE(left_block);
        
        /* remove the right neighbor from the free list since it's included */
        /* in the left neighbor's block now */
        if (right_block == g_freelist_head) {
          if (right_block->next == NULL) {
            g_freelist_head = NULL;
          }
          else {
            g_freelist_head = right_block->next;
            g_freelist_head->prev = NULL;
          }
        }
        else {
          /* if the next pointer of the right block is NULL, then we */
          /* simply have to set the next pointer of the prev block to NULL */
          if (right_block->next == NULL) {
            (right_block->prev)->next = NULL;
            right_block->prev = NULL;
          }
          /* if the next pointer is not NULL, then we need to maintain */
          /* the integrity of the list */
          else {
            right_block->prev->next = right_block->next;
            right_block->next->prev = right_block->prev;
            right_block->prev = NULL;
            right_block->next = NULL;
          }
        }

        /* set g_next_allocate appropriately */
        if (g_next_allocate == right_block) {
          g_next_allocate = left_block;
        }
      }
    }
  }
  
  pthread_mutex_unlock(&g_mutex);

  /* after we are done coalescing or adding blocks, return */
  return;
} /* my_free() */

/*
 * Calls malloc and sets each byte of
 * the allocated memory to a value.
 */

void *my_calloc(size_t nmemb, size_t size) {
  return memset(my_malloc(size * nmemb), 0, size * nmemb);
} /* my_calloc() */

/*
 * Reallocates an allocated block to a new size and
 * copies the contents to the new block.
 */

void *my_realloc(void *ptr, size_t size) {
  void *mem = my_malloc(size);
  memcpy(mem, ptr, size);
  my_free(ptr);
  return mem;
} /* my_realloc() */
