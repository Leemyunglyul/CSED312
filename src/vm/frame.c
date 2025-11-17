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

/* 프레임 테이블 초기화 */
void
frame_init (void) 
{
    list_init (&frame_table);
    lock_init (&frame_lock);
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

static void *
frame_evict (enum palloc_flags flags)
{
    /* 1. 희생양 선택 (지금은 임시로 첫 번째 프레임 선택) */
    /* (나중에 Clock 알고리즘으로 수정해야 함) */
    lock_acquire (&frame_lock);
    struct vm_entry *victim_vme = list_entry(list_pop_front(&frame_table), 
                                             struct vm_entry, f_elem);
    lock_release (&frame_lock);
    
    /* 2. 스왑 아웃 (디스크로 쓰기) */
    if (!frame_do_swap_out (victim_vme)) { 
        return NULL; 
    }
    
    /* 3. 비워진 프레임(kpage)은 palloc_get_page(flags)로 새로 할당
     * (victim_vme->kpage를 재사용하지 *않음*)
     * 왜냐하면 palloc_get_page(PAL_ZERO) 플래그를 적용해야 하기 때문.
     * (참고: victim_vme->kpage는 swap_out -> frame_free_internal에서 해제됨)
     */
    void *kpage = palloc_get_page (flags);
    if (kpage == NULL) {
        PANIC ("Eviction succeeded but palloc still fails.");
    }
    return kpage;
}

/* vm_entry를 스왑 아웃 (디스크로 쓰기) */
static bool
frame_do_swap_out (struct vm_entry *vme)
{
    ASSERT (vme->is_loaded == true);
    
    /* 1. (나중에 구현) Dirty bit 확인. 
     * Dirty 하지 않으면(VM_BIN인데 수정 안 됨) 스왑할 필요 없음.
     */
     
    /* 2. 스왑 슬롯을 할당받고 kpage 데이터를 디스크에 씀 */
    size_t swap_index = swap_out (vme->kpage);
    if (swap_index == BITMAP_ERROR) {
        return false;
    }
    
    /* 3. SPT (vm_entry) 정보 업데이트 */
    vme->is_loaded = false;
    vme->swap_index = swap_index;
    vme->type = VM_ANON; /* 중요: 파일에서 로드했어도, 쫓겨나면 스왑(ANON) 타입 */
    
    /* 4. 하드웨어 페이지 테이블(PTE) 매핑 해제 */
    pagedir_clear_page (vme->thread->pagedir, vme->vaddr);
    
    /* 5. 프레임 리소스 해제 (kpage만 free, vme->f_elem은 이미 제거됨) */
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
    struct list_elem *e;
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e)) {
        /* [수정] f_elem을 기준으로 vm_entry를 찾음 */
        struct vm_entry *vme = list_entry (e, struct vm_entry, f_elem);
        
        if (vme->kpage == kpage) {
            list_remove (&vme->f_elem);
            palloc_free_page (kpage);
            vme->kpage = NULL; // kpage 포인터 초기화
            break;
        }
    }
    lock_release (&frame_lock);
}