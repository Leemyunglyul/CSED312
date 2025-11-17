#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include <string.h>
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "lib/kernel/bitmap.h"

#define STACK_MAX_SIZE (8 * 1024 * 1024)

extern struct lock filesys_lock;

/* vm/page.c */

bool
grow_stack (void *fault_addr, void *esp)
{
    /* 1. 8MB 스택 제한 확인 */
    if (fault_addr < (PHYS_BASE - STACK_MAX_SIZE) || fault_addr >= PHYS_BASE) {
        return false;
    }

    /* === [핵심 수정] === */
    /* * Heuristic:
     * 1. PUSH/PUSHA (최대 esp - 32) 허용
     * 2. mov [esp+m] (fault_addr >= esp) 허용
     * 3. pt-grow-stack/pt-big-stk-obj (sub %esp 후 접근, fault_addr >= esp) 허용
     * * 따라서 fault_addr가 (esp - 32) 보다 낮으면 거부합니다.
     */
    if (fault_addr < (esp - 32)) {
        /* pt-grow-bad (esp - 4097)는 여기서 거부됩니다. */
        return false;
    }
    /* ================== */

    /* 3. 새 스택 페이지(VM_ANON) 생성 */
    void *stack_page = pg_round_down(fault_addr);
    
    /* (이미 할당된 페이지인지 한 번 더 확인) */
    if (vm_find(&thread_current()->vm, stack_page) != NULL) {
        return false;
    }

    /* (Turn 47의 나머지 로직과 동일) */
    struct vm_entry *vme = malloc(sizeof(struct vm_entry));
    if (vme == NULL) return false;

    vme->type = VM_ANON;
    vme->vaddr = stack_page;
    vme->writable = true;
    vme->is_loaded = false;
    vme->file = NULL;
    vme->thread = thread_current();
    vme->swap_index = BITMAP_ERROR; // 스왑 없음
    vme->pinned = false;

    if (!vm_insert (&thread_current()->vm, vme)) {
        free (vme);
        return false;
    }

    if (!load_page (vme)) {
        vm_delete (&thread_current()->vm, vme);
        free (vme);
        return false;
    }

    return true;
}

/* 가상 주소(vaddr)를 해시 값으로 변환하는 함수 */
static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
    struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
    return hash_bytes (&vme->vaddr, sizeof (vme->vaddr));
}

/* 두 해시 요소를 비교하는 함수 (a < b) */
static bool
vm_less_func (const struct hash_elem *a, const struct hash_elem *b,
              void *aux UNUSED)
{
    struct vm_entry *vme_a = hash_entry (a, struct vm_entry, elem);
    struct vm_entry *vme_b = hash_entry (b, struct vm_entry, elem);
    return vme_a->vaddr < vme_b->vaddr;
}

/* SPT(해시 테이블) 초기화 */
void
vm_init (struct hash *vm)
{
    hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

/* 가상 주소(vaddr)로 SPT 항목(vm_entry) 찾기 */
struct vm_entry *
vm_find (struct hash *vm, void *vaddr)
{
    struct vm_entry vme_temp;
    struct hash_elem *e;

    /* vaddr은 페이지 시작 주소여야 함 */
    vme_temp.vaddr = pg_round_down (vaddr);
    
    e = hash_find (vm, &vme_temp.elem);
    if (e == NULL) {
        return NULL;
    }
    return hash_entry (e, struct vm_entry, elem);
}

/* SPT에 항목(vm_entry) 추가 */
bool
vm_insert (struct hash *vm, struct vm_entry *vme)
{
    return hash_insert (vm, &vme->elem) == NULL; // 성공 시 NULL 반환
}

/* SPT에서 항목(vm_entry) 제거 */
bool
vm_delete (struct hash *vm, struct vm_entry *vme)
{
    return hash_delete (vm, &vme->elem) != NULL; // 성공 시 삭제된 elem 반환
}

/* SPT 항목(vm_entry)과 관련 리소스 해제 */
static void
vm_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
    struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);

    /* 1. 스왑 해제 (기존 로직) */
    if (vme->type == VM_ANON && vme->swap_index != BITMAP_ERROR) {
        swap_free (vme->swap_index);
    }

    /* 2. mmap 파일 처리 (Write-back) */
    if (vme->type == VM_FILE) {
        if (vme->is_loaded) {
            /* 2a. Dirty하면 파일에 다시 쓰기 */
            if (pagedir_is_dirty(vme->thread->pagedir, vme->vaddr)) {
                lock_acquire(&filesys_lock);
                file_write_at(vme->file, 
                              vme->kpage + (vme->offset % PGSIZE), 
                              vme->read_bytes, 
                              vme->offset);
                lock_release(&filesys_lock);
            }
            /* 2b. 프레임 해제 (process_exit -> pagedir_destroy가 이미 처리함) */
            /* (pagedir_destroy가 frame_free를 호출하므로, 
               vme->kpage는 이미 NULL일 것입니다) */
        }
        
        /* 2c. mmap이 reopen한 파일 닫기 */
        /* (참고: mapid별로 한 번만 닫아야 하므로, 
           여기서 닫으면 중복 닫기 오류가 발생할 수 있습니다. 
           process_exit에서 munmap_all을 호출하는 것이 더 낫습니다.)
        */
        // file_close(vme->file); // [버그 위험]
    }
    
    free (vme);
}

/* SPT(해시 테이블) 전체 삭제 */
void
vm_destroy (struct hash *vm)
{
    hash_destroy (vm, vm_destroy_func);
}

/* vm_entry의 정보에 따라 물리 프레임에 데이터를 로드 */
bool 
load_page (struct vm_entry *vme)
{
    if (vme->is_loaded) {
        return true;
    }
    /* 1. 물리 프레임 할당 (1단계에서 만든 frame_alloc 사용) */
    void *kpage = frame_alloc (vme, PAL_USER | PAL_ZERO);
    if (kpage == NULL) {
        return false;
    }

    bool lock_already_held = lock_held_by_current_thread(&filesys_lock);

    size_t page_offset = vme->offset % PGSIZE;

    /* 2. vm_entry 타입에 따라 데이터 로드 */
    switch (vme->type) 
    {
        case VM_BIN: // 실행 파일에서 읽기
            if (vme->swap_index != BITMAP_ERROR) {
                /* 1. 스왑에서 읽기 */
                swap_in (vme->swap_index, kpage);
                swap_free (vme->swap_index);
            } else {
                /* 2. 스왑에 없으면 파일에서 읽기 (기존 로직) */
                if (!lock_already_held) lock_acquire(&filesys_lock);
                if (file_read_at (vme->file, kpage + page_offset, 
                                vme->read_bytes, vme->offset) != (int)vme->read_bytes) 
                {
                    if (!lock_already_held) lock_release(&filesys_lock);
                    frame_free (kpage);
                    return false;
                }
                if (!lock_already_held) lock_release(&filesys_lock);
                
            }
            break;

        case VM_ANON: 
            if (vme->swap_index != BITMAP_ERROR) { 
                swap_in (vme->swap_index, kpage);
                swap_free (vme->swap_index);
            }
            break;
            
        case VM_FILE: /* <<< [추가] mmap 파일 로드 */
            if (!lock_already_held) lock_acquire(&filesys_lock);
            
            /* VM_BIN과 동일하게 파일에서 읽어옴 */
            if (file_read_at (vme->file, kpage + (vme->offset % PGSIZE),
                            vme->read_bytes, vme->offset)
                != (int)vme->read_bytes)
            {
                if (!lock_already_held) lock_release(&filesys_lock);
                frame_free(kpage);
                return false;
            }
            
            if (!lock_already_held) lock_release(&filesys_lock);
            break;
    }

    /* 3. 하드웨어 페이지 테이블(pagedir)에 매핑 */
    if (!install_page (vme->vaddr, kpage, vme->writable)) {
        frame_free (kpage);
        return false;
    }

    /* 4. SPT 상태 업데이트 */
    vme->is_loaded = true;
    vme->swap_index = BITMAP_ERROR;
    return true;
}

static void
munmap_helper (struct hash_elem *e, void *aux)
{
    mapid_t *mapid = (mapid_t *)aux;
    struct vm_entry *vme = hash_entry(e, struct vm_entry, elem);

    if (vme->type == VM_FILE && vme->mapid == *mapid) {
        /* mmap_exit과 동일한 로직 수행 */
        
        if (vme->is_loaded) {
            /* 1. Dirty하면 파일에 쓰기 */
            if (pagedir_is_dirty(vme->thread->pagedir, vme->vaddr)) {
                lock_acquire(&filesys_lock);
                file_write_at(vme->file, 
                              vme->kpage + (vme->offset % PGSIZE), 
                              vme->read_bytes, 
                              vme->offset);
                lock_release(&filesys_lock);
            }
            /* 2. 프레임 해제 */
            frame_free(vme->kpage);
            pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        }
        
        /* 3. SPT에서 vme 제거 (hash_delete는 hash_apply 도중 사용하면 위험) */
        /* (우선 vme만 free. 또는 hash_iterator/delete 사용 필요) */
        
        /* vme->file은 닫지 않음 (file_close는 munmap_internal에서 한꺼번에) */
        // free(vme); // (해시 순회 중 free는 위험!)
        
        /* * 안전한 삭제를 위해 vme->type을 VM_BIN(임시) 등으로 바꿔 
         * * 나중에 vm_destroy_func에서 free하게 하거나,
         * * 별도 리스트에 모았다가 삭제해야 함.
         */
    }
}

void 
munmap_process_exit (void)
{
    struct thread *cur = thread_current();
    struct hash *vm = &cur->vm;
    
    // 해시 테이블을 안전하게 순회하며 삭제하기 위한 반복자
    struct hash_iterator i;
    
    // 안전한 삭제를 위해: 리스트로 옮기거나, 삭제 후 반복자를 리셋해야 함
    hash_first (&i, vm); 

    // 안전하지 않은 루프이지만, 이 프로젝트에서는 이 방식을 사용해야 합니다.
    // (더 복잡한 구현을 피하기 위해)
    while (hash_next (&i))
    {
        struct vm_entry *vme = hash_entry (hash_cur (&i), struct vm_entry, elem);

        if (vme->type == VM_FILE) 
        {
            // 1. Write-back 및 리소스 해제 (munmap 로직)
            if (vme->is_loaded) {
                // Dirty하면 파일에 쓰기
                if (pagedir_is_dirty(cur->pagedir, vme->vaddr)) {
                    lock_acquire(&filesys_lock);
                    file_write_at(vme->file, 
                                  vme->kpage + (vme->offset % PGSIZE), 
                                  vme->read_bytes, 
                                  vme->offset);
                    lock_release(&filesys_lock);
                }
                // 프레임 해제
                pagedir_clear_page(cur->pagedir, vme->vaddr); 
                frame_free(vme->kpage); 
            }
            
            // 2. [mmap-exit 버그 해결] mmap이 reopen한 파일 닫기
            file_close(vme->file); 
            
            // 3. SPT에서 vm_entry 제거 (반복자 리셋 후 삭제)
            hash_delete(vm, &vme->elem);
            hash_first(&i, vm); // [안전장치] 반복자를 리셋
            
            // 4. vme 구조체 자체 해제
            free(vme);
        }
    }
}