#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include <string.h>
#include "threads/synch.h"
#include "userprog/syscall.h"

extern struct lock filesys_lock;

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
    return true;
}