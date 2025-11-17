#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "filesys/file.h"


/* 페이지의 상태(위치)를 나타내는 타입 */
enum vm_type {
    VM_BIN,  /* 실행 파일 (디스크) */
    VM_ANON, /* 스택, 0으로 채워진 페이지 (Swap 대상) */
    VM_FILE  /* 메모리 맵 파일 (mmap) */
};

struct vm_entry {
    enum vm_type type;
    void *vaddr;
    bool writable;
    bool is_loaded;
    
    struct file *file;
    off_t offset;
    size_t read_bytes;
    size_t zero_bytes;
    
    size_t swap_index;

    struct hash_elem elem;  /* SPT(해시 테이블)용 elem */
    
    /* === [추가: Frame Table 통합] === */
    void *kpage;            /* 프레임 주소 (로드되었을 때만) */
    struct list_elem f_elem; /* 프레임 테이블(list)용 elem */
    /* ============================== */
    struct thread *thread;
    bool pinned;
};



/* SPT(해시 테이블) 초기화 */
void vm_init (struct hash *vm);
/* SPT(해시 테이블) 전체 삭제 */
void vm_destroy (struct hash *vm);

/* 가상 주소(vaddr)로 SPT 항목(vm_entry) 찾기 */
struct vm_entry *vm_find (struct hash *vm, void *vaddr);
/* SPT에 항목(vm_entry) 추가 */
bool vm_insert (struct hash *vm, struct vm_entry *vme);
/* SPT에서 항목(vm_entry) 제거 */
bool vm_delete (struct hash *vm, struct vm_entry *vme);

/* 페이지 폴트 시 호출될 핸들러 (데이터 로딩) */
bool load_page (struct vm_entry *vme);

bool grow_stack (void *fault_addr, void *esp);

#endif /* vm/page.h */