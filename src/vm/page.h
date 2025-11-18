#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include <list.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "userprog/syscall.h"

/* 페이지 타입 */
enum vm_type {
    VM_BIN,   /* 바이너리 실행 파일 (Code/Data) */
    VM_FILE,  /* 메모리 맵 파일 (mmap) */
    VM_ANON   /* 스택, 힙, 스왑 영역 (Anonymous) */
};

/* 가상 메모리 항목 (Page Table Entry 역할) */
struct vm_entry {
    enum vm_type type;
    void *vaddr;            /* 가상 주소 (Page Aligned) */
    bool writable;          /* 쓰기 가능 여부 */
    
    bool is_loaded;         /* 물리 메모리에 로드되었는지 여부 */
    void *kpage;            /* 매핑된 물리 프레임 주소 (is_loaded=true일 때 유효) */
    
    struct file *file;      /* [VM_FILE/VM_BIN] 연관된 파일 */
    off_t offset;           /* 파일 내 오프셋 */
    size_t read_bytes;      /* 파일에서 읽을 바이트 수 */
    size_t zero_bytes;      /* 0으로 채울 바이트 수 */
    
    size_t swap_index;      /* [VM_ANON] 스왑 슬롯 인덱스 */
    
    struct hash_elem elem;  /* Thread의 vm(해시 테이블) 연결용 */
    struct list_elem f_elem; /* Frame Table 연결용 (Back-pointer) */
    
    struct thread *thread;  /* 소유자 스레드 */
    bool pinned;            /* 교체(Evict) 방지 플래그 */
    mapid_t mapid;          /* [VM_FILE] mmap 식별자 */
};

/* 함수 프로토타입 */
void vm_init (struct hash *vm);
void vm_destroy (struct hash *vm);

struct vm_entry *vm_find (struct hash *vm, void *vaddr);
bool vm_insert (struct hash *vm, struct vm_entry *vme);
bool vm_delete (struct hash *vm, struct vm_entry *vme);

bool load_page (struct vm_entry *vme);
bool grow_stack (void *fault_addr, void *esp);

void vm_munmap_page (struct vm_entry *vme);

#endif /* vm/page.h */