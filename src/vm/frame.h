#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"
#include "threads/thread.h"

/* 프레임 테이블의 각 항목 */
struct frame_entry {
    void *kpage;            /* 프레임의 커널 가상 주소 (물리 프레임) */
    void *upage;            /* 이 프레임에 매핑된 유저 가상 주소 (가상 페이지) */
    struct thread *thread;  /* 이 프레임을 소유한 스레드 */
    struct list_elem elem;  /* 전역 frame_table 리스트를 위한 요소 */
};

void frame_init (void);
void *frame_alloc (void *upage, enum palloc_flags flags);
void frame_free (void *kpage);

#endif /* vm/frame.h */