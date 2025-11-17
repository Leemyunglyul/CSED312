#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include <list.h>
#include <stdio.h>

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
        // (나중에 Eviction 로직)
        return NULL;
    }

    /* [수정] malloc 대신 vm_entry를 직접 사용 */
    vme->kpage = kpage; // vm_entry에 프레임 주소 저장

    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &vme->f_elem); // vm_entry의 f_elem 사용
    lock_release (&frame_lock);

    return kpage;
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