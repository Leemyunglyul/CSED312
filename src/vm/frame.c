#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include <list.h>
#include <stdio.h>
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include "filesys/file.h"

/* 프레임 테이블 관리 변수 */
static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

/* 내부 함수 선언 */
static struct vm_entry *select_victim_frame(void);
static bool frame_do_swap_out(struct vm_entry *vme);
static void *frame_evict(enum palloc_flags flags);

void
frame_init (void) 
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = NULL;
}

void *
frame_alloc (struct vm_entry *vme, enum palloc_flags flags) 
{
    /* 1. 일단 메모리 할당 시도 */
    void *kpage = palloc_get_page (flags);
    
    /* 2. 실패하면 기존 페이지를 쫓아내고(Evict) 공간 확보 */
    if (kpage == NULL) {
        kpage = frame_evict (flags);
    }
    
    /* 3. 성공했다면 프레임 테이블에 등록 */
    if (kpage != NULL) {
        vme->kpage = kpage; 
        
        lock_acquire (&frame_lock);
        list_push_back (&frame_table, &vme->f_elem); 
        lock_release (&frame_lock);
    } else {
        /* 할당 실패 */
        vme->kpage = NULL;
    }

    return kpage; 
}

void
frame_free (void *kpage) 
{
    if (kpage == NULL) return;

    lock_acquire (&frame_lock);
    
    struct list_elem *e = list_begin(&frame_table);
    while (e != list_end(&frame_table)) {
        struct vm_entry *vme = list_entry(e, struct vm_entry, f_elem);
        
        if (vme->kpage == kpage) {
            /* Clock Hand가 삭제될 원소를 가리키면 다음으로 이동 */
            if (clock_hand == e) {
                clock_hand = list_next(e);
            }
            
            list_remove(e);
            
            /* 연결된 vm_entry 정보 초기화 */
            vme->kpage = NULL;
            vme->is_loaded = false;
            break;
        }
        e = list_next(e);
    }
    
    lock_release (&frame_lock);
    
    /* 실제 물리 메모리 해제 */
    palloc_free_page(kpage);
}

/* Clock Algorithm을 사용하여 희생자(Victim) 선정 */
static struct vm_entry *
select_victim_frame (void)
{
    /* 주의: 이 함수 호출 시 frame_lock을 잡고 있어야 함 */
    /* 하지만 여기선 함수 내부에서 Lock을 잡는 구조로 구현함 */
    lock_acquire (&frame_lock);

    if (list_empty(&frame_table)) {
        lock_release(&frame_lock);
        return NULL; 
    }

    /* Clock Hand 초기화 */
    if (clock_hand == NULL || clock_hand == list_end(&frame_table)) {
        clock_hand = list_begin(&frame_table);
    }

    struct vm_entry *victim = NULL;

    /* 한 바퀴 돌면서 찾음 (최대 리스트 사이즈 + 1 만큼 순회) */
    size_t n = list_size(&frame_table);
    for (size_t i = 0; i <= n; i++) {
        struct list_elem *e = clock_hand;
        
        /* 리스트 끝에 도달하면 처음으로 순환 */
        if (e == list_end(&frame_table)) {
            e = list_begin(&frame_table);
        }
        
        struct vm_entry *vme = list_entry(e, struct vm_entry, f_elem);
        
        /* 다음 검사할 위치 미리 저장 */
        clock_hand = list_next(e);

        /* Pinned 페이지는 교체 대상 아님 */
        if (vme->pinned) {
            continue;
        }

        /* Accessed Bit 확인 (Second Chance Algorithm) */
        if (pagedir_is_accessed(vme->thread->pagedir, vme->vaddr)) {
            /* 최근에 참조됨 -> 기회를 한 번 더 줌 (Accessed bit 0으로 설정) */
            pagedir_set_accessed(vme->thread->pagedir, vme->vaddr, false);
        } else {
            /* 참조되지 않음 -> 희생자로 선정 */
            victim = vme;
            list_remove(e); /* 리스트에서 제거 */
            break;
        }
    }

    lock_release(&frame_lock);
    return victim;
}

/* 페이지 교체 수행 (Retry Loop 포함) */
static void *
frame_evict (enum palloc_flags flags)
{
    struct vm_entry *victim;
    void *kpage;
    
    /* 희생자를 찾을 때까지 계속 시도 (메모리 부족 해결을 위해 필수) */
    while (true) {
        victim = select_victim_frame();
        
        /* 교체할 페이지가 아예 없으면 (전부 pinned 등) 실패 */
        if (victim == NULL) {
            return NULL;
        }

        /* 스왑 아웃 시도 */
        if (frame_do_swap_out(victim)) {
            /* 스왑 성공 -> 빈 공간 확보됨 -> 할당 시도 */
            kpage = palloc_get_page(flags);
            
            if (kpage != NULL) {
                return kpage; 
            }
            /* 스왑은 했는데 palloc이 실패하면 다시 루프 */
        } else {
            /* 스왑 실패 -> 희생자를 다시 리스트에 복구하고 재시도 */
            lock_acquire(&frame_lock);
            list_push_back(&frame_table, &victim->f_elem);
            lock_release(&frame_lock);
        }
    }
}

/* 실제 스왑 아웃 및 파일 쓰기 수행 */
static bool
frame_do_swap_out (struct vm_entry *vme)
{
    //ASSERT (vme != NULL);
    //ASSERT (vme->is_loaded == true);

    if (vme == NULL || !vme->is_loaded) {
        return true; /* 이미 처리되었거나 로드되지 않음 -> 성공으로 간주 */
    }
    
    bool is_dirty = pagedir_is_dirty(vme->thread->pagedir, vme->vaddr);
    
    /* Case 1: 실행 파일(Binary) - 읽기 전용이거나 Dirty가 아니면 그냥 버림 */
    if (vme->type == VM_BIN) {
        /* 실행 파일 세그먼트는 언제든 파일에서 다시 읽을 수 있음 (Dirty가 아닌 경우) */
        /* 만약 스택이 확장되어 VM_BIN 영역인 척 하는 경우가 아니라면 */
        if (!is_dirty) {
            pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
            palloc_free_page(vme->kpage);
            vme->is_loaded = false;
            vme->kpage = NULL;
            return true;
        }
        /* Dirty인 VM_BIN은 스왑 영역으로 가야 함 (이런 경우가 드물지만 스택 확장 등 고려) */
        // VM_BIN이 Dirty면 아래 로직을 타고 Swap으로 감
    }

    /* Case 2: 메모리 매핑 파일 (mmap) */
    if (vme->type == VM_FILE) {
        if (is_dirty) {
            lock_acquire(&filesys_lock);
            
            off_t file_len = file_length(vme->file);
            off_t write_bytes = vme->read_bytes;
            if (vme->offset + write_bytes > file_len) 
                write_bytes = file_len - vme->offset;
                
            file_write_at(vme->file, vme->kpage, write_bytes, vme->offset);
            
            lock_release(&filesys_lock);
        }
        
        pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        palloc_free_page(vme->kpage); // 이미 리스트에서 빠짐
        vme->is_loaded = false;
        vme->kpage = NULL;
        return true;
    }

    /* Case 3: 일반 페이지 (스택, 힙 등) -> 스왑 파티션 사용 */
    /* 또는 Dirty 상태인 VM_BIN */
    size_t swap_index = swap_out(vme->kpage);
    
    if (swap_index == BITMAP_ERROR) {
        /* 스왑 슬롯 꽉 참 -> 교체 실패 */
        return false;
    }

    /* 메타데이터 업데이트 */
    vme->swap_index = swap_index;
    vme->type = VM_ANON; /* 스왑으로 나가면 익명 페이지 취급 */
    vme->is_loaded = false;

    pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
    palloc_free_page(vme->kpage);
    vme->kpage = NULL;

    return true;
}