#include "vm/swap.h"
#include "devices/block.h"    // block_get_role, block_read, block_write
#include "lib/kernel/bitmap.h"  // bitmap_create, bitmap_scan_and_flip, ...
#include "threads/vaddr.h"    // PGSIZE
#include "threads/synch.h"    // lock

/* 스왑 디스크를 나타내는 블록 디바이스 */
static struct block *swap_disk;
/* 사용 중인 스왑 슬롯을 추적하는 비트맵 */
static struct bitmap *swap_table;
/* 스왑 테이블 접근을 동기화하기 위한 락 */
static struct lock swap_lock;

/* 한 페이지(4KB)는 디스크 섹터(512B) 8개에 해당합니다. */
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE) // 4096 / 512 = 8

/* 스왑 공간 초기화 */
void
swap_init (void) 
{
    swap_disk = block_get_role (BLOCK_SWAP);
    if (swap_disk == NULL) {
        /* [수정] 조용히 리턴하는 대신 패닉 */
        PANIC ("Swap disk not found, use --swap-size option."); 
    }

    size_t swap_slots = block_size (swap_disk) / SECTORS_PER_PAGE;
    swap_table = bitmap_create (swap_slots);
    if (swap_table == NULL) {
        /* [수정] 조용히 리턴하는 대신 패닉 */
        PANIC ("Failed to create swap table bitmap.");
    }
    
    lock_init (&swap_lock);
}

/* 데이터를 스왑 공간으로 내보내기 (Swap-out) */
size_t
swap_out (void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);

    /* 1. 비어있는 스왑 슬롯 찾기 (0번 비트부터, 1개, 값은 false) */
    size_t free_index = bitmap_scan_and_flip (swap_table, 0, 1, false);

    if (free_index == BITMAP_ERROR) {
        /* 스왑 공간이 꽉 찼음. 커널 패닉! */
        lock_release (&swap_lock);
        PANIC ("Swap partition is full!");
    }

    /* 2. kpage의 데이터를 디스크 슬롯에 쓴다 (섹터 8개) */
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_write (swap_disk, 
                     (free_index * SECTORS_PER_PAGE) + i, 
                     kpage + (i * BLOCK_SECTOR_SIZE));
    }

    lock_release (&swap_lock);
    return free_index;
}

/* 데이터를 스왑 공간에서 가져오기 (Swap-in) */
void
swap_in (size_t swap_index, void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);

    /* 1. 해당 슬롯이 사용 중인지 확인 */
    if (!bitmap_test (swap_table, swap_index)) {
        lock_release (&swap_lock);
        PANIC ("Invalid swap slot index read attempt!");
    }

    /* 2. 디스크 슬롯에서 kpage로 데이터를 읽어온다 (섹터 8개) */
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_read (swap_disk, 
                    (swap_index * SECTORS_PER_PAGE) + i, 
                    kpage + (i * BLOCK_SECTOR_SIZE));
    }
    
    /* 3. (참고) 데이터를 읽어왔다고 해서 슬롯을 바로 해제(free)하지 않습니다.
     * 해제는 swap_free()가 명시적으로 호출될 때 수행합니다.
     */

    lock_release (&swap_lock);
}

/* 스왑 슬롯 해제 (e.g., 프로세스가 종료될 때) */
void
swap_free (size_t swap_index) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);
    
    lock_acquire (&swap_lock);
    if (bitmap_test (swap_table, swap_index)) {
        bitmap_reset (swap_table, swap_index); // 비트를 0 (false)으로 되돌림
    }
    lock_release (&swap_lock);
}