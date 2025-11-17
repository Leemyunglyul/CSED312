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
frame_alloc (void *upage, enum palloc_flags flags) 
{
    ASSERT (flags & PAL_USER); // 유저 풀에서만 할당해야 함

    /* 1. palloc으로 물리 페이지(프레임) 할당 */
    void *kpage = palloc_get_page (flags);

    if (kpage == NULL) {
        /* * 페이지 할당 실패. 
         * [미래] 1단계에서는 NULL을 반환하지만, 나중에는 여기서 
         * 페이지 교체(eviction) 로직이 실행되어야 합니다.
         */
        return NULL;
    }

    /* 2. 프레임 테이블 항목(entry) 생성 */
    struct frame_entry *fe = malloc(sizeof(struct frame_entry));
    if (fe == NULL) {
        /* 항목 생성 실패 시, 할당받은 페이지 반환 */
        palloc_free_page(kpage);
        return NULL;
    }

    fe->kpage = kpage;
    fe->upage = upage;
    fe->thread = thread_current();

    /* 3. 프레임 테이블(전역 리스트)에 추가 (락 사용) */
    lock_acquire (&frame_lock);
    list_push_back (&frame_table, &fe->elem);
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
        struct frame_entry *fe = list_entry (e, struct frame_entry, elem);
        
        if (fe->kpage == kpage) {
            list_remove (&fe->elem);  /* 1. 테이블에서 항목 제거 */
            palloc_free_page (kpage); /* 2. 물리 페이지 반환 */
            free (fe);                /* 3. frame_entry 구조체 자체를 해제 */
            break;
        }
    }
    
    lock_release (&frame_lock);
}