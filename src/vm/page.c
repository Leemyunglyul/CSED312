#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include <string.h>
#include "threads/synch.h"
#include "userprog/syscall.h"

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
    vme->swap_index = 0;

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
    
    // (나중에 스왑, mmap 해제 로직 추가 필요)
    
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
            if (!lock_already_held) lock_acquire(&filesys_lock);
            
            /* [수정 3] kpage + page_offset 위치에 파일 데이터를 씀 */
            if (file_read_at (vme->file, kpage + page_offset, 
                            vme->read_bytes, vme->offset) 
                != (int)vme->read_bytes) 
            {
                if (!lock_already_held) lock_release(&filesys_lock);
                frame_free (kpage);
                return false;
            }
            
            if (!lock_already_held) lock_release(&filesys_lock);
            break;

        case VM_ANON: 
            if (vme->swap_index != 0) { // 0을 유효하지 않은 인덱스로 가정
                swap_in (vme->swap_index, kpage);
                swap_free (vme->swap_index); // 스왑에서 읽어왔으니 슬롯 해제
            } else {
                /* (기존 로직) 스택 확장 등, PAL_ZERO로 이미 0으로 채워짐 */
            }
            break;
            
        case VM_FILE: // (mmap은 나중에 구현)
            // ...
            break;
    }

    /* 3. 하드웨어 페이지 테이블(pagedir)에 매핑 */
    if (!install_page (vme->vaddr, kpage, vme->writable)) {
        frame_free (kpage);
        return false;
    }

    /* 4. SPT 상태 업데이트 */
    vme->is_loaded = true;
    vme->swap_index = 0;
    return true;
}