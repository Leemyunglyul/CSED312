#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include <list.h>
#include <stdio.h>
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "lib/kernel/bitmap.h"

/* 페이지 교체를 시도하는 함수 (새로 추가) */
static void *frame_evict (enum palloc_flags flags);
static bool frame_do_swap_out (struct vm_entry *vme);
/* 전역 프레임 테이블 (사용 중인 프레임 목록) */
static struct list frame_table;
/* 프레임 테이블 접근을 동기화하기 위한 락 */
static struct lock frame_lock;
static struct list_elem *clock_hand;

/* 프레임 테이블 초기화 */
void
frame_init (void) 
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = NULL;
}

/* * 새 프레임을 할당받고 프레임 테이블에 등록 
 * (palloc_get_page(PAL_USER)를 대체)
 */
void *
frame_alloc (struct vm_entry *vme, enum palloc_flags flags) 
{
    ASSERT (flags & PAL_USER);

    void *kpage = palloc_get_page (flags);
    if (kpage == NULL) {
        /* [핵심 수정] 프레임이 부족하면 교체를 시도 */
        kpage = frame_evict (flags);
        if (kpage == NULL) {
            /* 교체(Eviction)에도 실패하면 정말 메모리가 없는 것 (PANIC) */
            PANIC ("Frame eviction failed, kernel out of memory.");
        }
    }

    /* [수정] malloc 대신 vm_entry를 직접 사용 */
    vme->kpage = kpage; // vm_entry에 프레임 주소 저장

    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &vme->f_elem); // vm_entry의 f_elem 사용
    lock_release (&frame_lock);

    return kpage;
}

static struct vm_entry *
select_victim_frame (void)
{
    lock_acquire (&frame_lock);
    
    if (list_empty(&frame_table)) {
        lock_release (&frame_lock);
        PANIC("Frame table is empty, cannot evict!");
    }

    /* 시계침이 리스트 끝에 도달했거나 NULL이면, 처음으로 되돌림 */
    if (clock_hand == NULL || clock_hand == list_end (&frame_table)) {
        clock_hand = list_begin (&frame_table);
    }

    struct vm_entry *victim_vme = NULL;
    
    /* * 리스트를 안전하게 순회하기 위해 (list_remove 대비)
     * * clock_hand를 먼저 다음으로 이동시킵니다.
     */
    while (true)
    {
        struct list_elem *current_elem = clock_hand;
        struct vm_entry *vme = list_entry (current_elem, struct vm_entry, f_elem);
        
        clock_hand = list_next (clock_hand);
        if (clock_hand == list_end (&frame_table)) {
            clock_hand = list_begin (&frame_table);
        }

        if (vme->pinned || !vme->is_loaded) {
            continue;
        }
        /* * PTE의 Accessed Bit 확인
         * * (pagedir_is_accessed는 pagedir.h/c에 이미 존재)
         */
        if (pagedir_is_accessed (vme->thread->pagedir, vme->vaddr)) {
            /* Bit == 1: 0으로 바꾸고 다음으로 넘어감 (기회 부여) */
            pagedir_set_accessed (vme->thread->pagedir, vme->vaddr, false);
        } else {
            /* Bit == 0: 희생양 발견 */
            victim_vme = vme;
            list_remove (current_elem); // 리스트에서 제거
            break;
        }
    }
    
    lock_release (&frame_lock);
    return victim_vme;
}

static void *
frame_evict (enum palloc_flags flags)
{
    /* 1. [수정] Clock 알고리즘으로 희생양 선택 */
    struct vm_entry *victim_vme = select_victim_frame ();
    
    /* 2. 스왑 아웃 (디스크로 쓰기) */
    if (!frame_do_swap_out (victim_vme)) { 
        /* (스왑 실패 시 희생양을 다시 리스트에 넣는 로직이 필요할 수 있으나,
           우선 PANIC 또는 NULL 반환으로 처리) */
        return NULL; 
    }
    
    /* 3. 새 프레임 할당 */
    void *kpage = palloc_get_page (flags);
    if (kpage == NULL) {
        PANIC ("Eviction succeeded but palloc still fails.");
    }
    return kpage;
}

static bool
frame_do_swap_out (struct vm_entry *vme)
{
    ASSERT (vme->is_loaded == true);
    
    /* [핵심 수정] Dirty Bit 확인 */
    bool is_dirty = pagedir_is_dirty (vme->thread->pagedir, vme->vaddr);
    
    if (vme->type == VM_BIN && !is_dirty) {
        /* * 수정되지 않은 파일 페이지(VM_BIN)는 스왑할 필요 없이
         * * 프레임만 해제 (is_loaded = false, pagedir_clear)
         */
        vme->is_loaded = false;
        pagedir_clear_page (vme->thread->pagedir, vme->vaddr);
        palloc_free_page (vme->kpage);
        vme->kpage = NULL;
        return true; // 스왑 성공 (사실 안 했지만)
    }
    
    /* * Dirty 페이지(ANON 또는 BIN)는 스왑 아웃 */
    size_t swap_index = swap_out (vme->kpage); 
    if (swap_index == BITMAP_ERROR) {
        return false;
    }
    
    vme->is_loaded = false;
    vme->swap_index = swap_index;
    vme->type = VM_ANON; /* 중요: 파일에서 로드했어도, 쫓겨나면 스왑(ANON) 타입 */
    
    pagedir_clear_page (vme->thread->pagedir, vme->vaddr);
    
    palloc_free_page (vme->kpage);
    vme->kpage = NULL;
    
    return true;
}

/* * 프레임을 반환하고 프레임 테이블에서 제거
 * (palloc_free_page를 대체)
 */
void
frame_free (void *kpage) 
{
    lock_acquire (&frame_lock);
    
    struct list_elem *e = list_begin (&frame_table);
    while (e != list_end (&frame_table)) 
    {
        struct vm_entry *vme = list_entry (e, struct vm_entry, f_elem);
        
        if (vme->kpage == kpage) {
            
            if (clock_hand == e) {
                clock_hand = list_next(e);
            }
            /* ================== */

            list_remove (e); // 요소 제거
            palloc_free_page (kpage);
            vme->kpage = NULL;
            break; // 찾았으므로 루프 종료
        }
        
        /* [중요] 'for' 루프 대신 'while'과 수동 증가 사용 */
        e = list_next(e); 
    }
    
    lock_release (&frame_lock);
}