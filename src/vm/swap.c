#include "vm/swap.h"
#include "devices/block.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include "threads/synch.h"

static struct block *swap_disk;
static struct bitmap *swap_table;
static struct lock swap_lock;

#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

void
swap_init (void) 
{
    swap_disk = block_get_role (BLOCK_SWAP);
    if (swap_disk == NULL) {
        return;
    }

    size_t swap_slots = block_size (swap_disk) / SECTORS_PER_PAGE;
    swap_table = bitmap_create (swap_slots);
    if (swap_table == NULL) {
        PANIC ("Failed to create swap table bitmap.");
    }
    
    lock_init (&swap_lock);
}

size_t
swap_out (void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);
    size_t free_index = bitmap_scan_and_flip (swap_table, 0, 1, false);
    lock_release (&swap_lock); 

    if (free_index == BITMAP_ERROR) {
        return BITMAP_ERROR;
    }

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_write (swap_disk, 
                     (free_index * SECTORS_PER_PAGE) + i, 
                     kpage + (i * BLOCK_SECTOR_SIZE));
    }

    return free_index;
}

void
swap_in (size_t swap_index, void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);
    if (!bitmap_test (swap_table, swap_index)) {
        lock_release (&swap_lock);
        PANIC ("Invalid swap slot index read attempt: %zu", swap_index);
    }
    lock_release (&swap_lock);
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_read (swap_disk, 
                    (swap_index * SECTORS_PER_PAGE) + i, 
                    kpage + (i * BLOCK_SECTOR_SIZE));
    }
}

void
swap_free (size_t swap_index) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);
    
    lock_acquire (&swap_lock);
    if (bitmap_test (swap_table, swap_index)) {
        bitmap_reset (swap_table, swap_index); 
    }
    lock_release (&swap_lock);
}