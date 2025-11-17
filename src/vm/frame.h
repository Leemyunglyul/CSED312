#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include "threads/palloc.h"
#include "threads/thread.h"
#include "vm/page.h"

void frame_init (void);
void *frame_alloc (struct vm_entry *vme, enum palloc_flags flags);
/* 프레임(kpage)에 연결된 vm_entry를 테이블에서 제거하고 프레임 반환 */
void frame_free (void *kpage);

#endif /* vm/frame.h */