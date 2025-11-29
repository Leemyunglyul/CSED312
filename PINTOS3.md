## 1. Frame table

### requirements
 
Implement a frame table to manage frames effectively.  

∙ Current implementation: The size of page and frame is 4096 bytes. The management of page tables is 
implemented in “userprog/pagedir.c”, and the allocation/deallocation of page tables are implemented 
in “threads/palloc.c”. 

∙ New implementation (5 points): Create a frame table to manage information of each frame. 

∙ Expected entry: frame number, thread id, possibility of allocation 

∙ Main functions 

✓ Allocate/deallocate frames 

✓ Choose a victim which returns occupying frames when free frame doesn’t exist. 

✓ Search frames used by user process (thread) 

∙ Frames used by user pages must be allocated by “user pool”.  

∙ Modify process loading (the loop in “load_segment()”) in “userprog/process.c” for managing a frame table.

### Implementation

물리 메모리(frame) 상태를 관리하고, 메모리가 부족할 때 누구를 쫓아낼지(eviciotn) 결정할 수 있는 기반을 마련해야 한다.

1. 전역 관리: 물리 메모리는 모든 process가 공유하는 자원이다. 따라서 frame table은 특정 thread에 종속되지 않고 global로 선언되어야 한다.

2. Reverse mapping: 나중에 memory가 부족해서 frame을 디스크로 쫓아낼 때(swap out), 해당 frame이 어떤 process의 어떤 virtual address와 연결되어 있는지 알아야 page table을 업데이트할 수 있다. 기존 palloc은 모르기 때문에 frame_entry를 새로 선언해 이 정보를 저장한다.

3. 동기화: 여러 process가 동시에 memory를 요청할 수 있으므로 lock이 필요하다.

#### Create a frame table to manage information of each frame. Expected entry: frame number, thread id, possibility of allocation 

별도의 frame table entry 구조체를 두는 대신, `struct vm_entry`를 `vm/page.c`에 선언하고 frame table list를 `vm/frame.c`내에서 선언하여 관리하였다.

```c
// vm/frame.c
static struct list frame_table;

// vm/page.c
struct vm_entry {
    enum vm_type type;
    void *vaddr;            /* 가상 주소 (Page Aligned) */
    bool writable;          /* 쓰기 가능 여부 */
    
    bool is_loaded;         /* 물리 메모리에 로드되었는지 여부 */
    void *kpage;            /* 매핑된 물리 프레임 주소 (is_loaded=true일 때 유효) */
    
    ...

    struct thread *thread;  /* 소유자 스레드 */
    bool pinned;            /* 교체(Evict) 방지 플래그 */

    ...
};
```

1. frame number: `void *kpage`. kernel virtual address는 physical address와 1:1 매핑된다. 즉, kpage 포인터가 가리키는 주소가 곧 frame을 나타낸다. is_loaded이 true일 때 해당 virtual page(vaddr)와 연결된 frame의 주소를 담고 있다.

2. thread id: `struct thread *thread`. 정수형 id 대신, 해당 frame을 소유한 thread 구조체의 포인터를 저장했다. 

3. possibility of allocation: `bool pinned`. 이 frame이 eviction 대상이 될 수 있는지 나타낸다. pinned가 true면, 중요한 작업 중이므로 evict불기 이다.

```c
// vm/frame.c
static struct list frame_table;
static struct lock frame_lock;

void
frame_init (void) 
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = NULL;
}
```

frame table을 위처럼 선언해서 frame entry를 전역 관리하고 있으며, frame_lock을 별도로 두어 frame_table에 대해 동시 접근을 제어하고 있다. 또한 frame_init에서 위의 table과 lock을 초기화하고 있다.

#### Allocate/deallocate frames 

```c
void *
frame_alloc (struct vm_entry *vme, enum palloc_flags flags) 
{
    void *kpage = palloc_get_page (flags);
    
    if (kpage == NULL) {
        kpage = frame_evict (flags);
    }
    
    if (kpage != NULL) {
        vme->kpage = kpage; 
        
        lock_acquire (&frame_lock);
        list_push_back (&frame_table, &vme->f_elem); 
        lock_release (&frame_lock);
    } else {
        vme->kpage = NULL;
    }

    return kpage; 
}
```

frame 할당을 요청하는 함수로, 기존 `palloc_get_page`를 감싸는 형태로 구현되었다.

1. 물리 메모리 요청: `palloc_get_page (flags);`를 호출해서 커널로부터 물리 프레임(kpage)를 할당받는다.

2. Eviction: 만약 메모리가 가득차서 palloc이 NULL을 반환하면(메모리가 가득 참.), `frame_evict`를 호출해서 기존 page를 swap out시키고 빈 공간을 확보한다.

3. frame table 등록: 할당에 성공하면 해당 frame 정보를 전역 관리 list인 frame_table에 삽입하고 lock_acquire를 통해 동시 접근을 제어한다.

4. 매핑 정보 저장: vm_entry 구조체에 할당받은 물리 주소(kpage)를 저장해서 가상 주소와 물리 주소를 연결한다.

```c
void
frame_free (void *kpage) 
{
    if (kpage == NULL) return;

    lock_acquire (&frame_lock);
    
    struct list_elem *e = list_begin(&frame_table);
    while (e != list_end(&frame_table)) {
        struct vm_entry *vme = list_entry(e, struct vm_entry, f_elem);
        
        if (vme->kpage == kpage) {
            if (clock_hand == e) {
                clock_hand = list_next(e);
            }
            
            list_remove(e);
            
            vme->kpage = NULL;
            vme->is_loaded = false;
            break;
        }
        e = list_next(e);
    }
    
    lock_release (&frame_lock);
    
    palloc_free_page(kpage);
}
```

사용이 끝난 frame을 반환하고, frame table에서 제거한다.

1. frame table 검색

2. clock hand 조정: 만약 페이지 교체 알고리즘의 포인터인 clock_hand가 현재 삭제하려는 entry를 가리키고 있다면, 다음 칸으로 이동시킨다.

3. frame table에서 제거.

4. frame 해제: palloc_free_page 호출.

#### Choose a victim which returns occupying frames when free frame doesn’t exist. 

```c
static struct vm_entry *
select_victim_frame (void)
{
    lock_acquire (&frame_lock);

    if (list_empty(&frame_table)) {
        lock_release(&frame_lock);
        return NULL; 
    }

    if (clock_hand == NULL || clock_hand == list_end(&frame_table)) {
        clock_hand = list_begin(&frame_table);
    }

    struct vm_entry *victim = NULL;

    size_t n = list_size(&frame_table);
    for (size_t i = 0; i <= n; i++) {
        struct list_elem *e = clock_hand;

        if (e == list_end(&frame_table)) {
            e = list_begin(&frame_table);
        }
        
        struct vm_entry *vme = list_entry(e, struct vm_entry, f_elem);
        
        clock_hand = list_next(e);

        if (vme->pinned) {
            continue;
        }

        if (pagedir_is_accessed(vme->thread->pagedir, vme->vaddr)) {
            pagedir_set_accessed(vme->thread->pagedir, vme->vaddr, false);
        } else {
            victim = vme;
            list_remove(e);
            break;
        }
    }

    lock_release(&frame_lock);
    return victim;
}
```

위 함수는 frame table을 순회하며 victim을 선정한다. clock alogrithm으로 설계되었는데,

1. clock_hand가 가리키는 frame부터 검사를 시작.

2. vme->pinned가 true면 건너뛴다.

3. accessed_bit(pagedir_is_accessed) 확인: 1은 최근 사용됨을 말한다. 0으로 초기화하고 clock_hand를 다음으로 이동시킨다.(한 번 기회를 줌) 0이면 바로 victim으로 선정하고 loop 종료.


#### Search frames used by user process (thread) 

process가 종료될 때, 그 process가 점유하고 있던 모든 frame을 찾아서 반환할 수 있어야 한다. 이는 이미 `frame_free`의 for loop에서 구현되어 있다.

#### Frames used by user pages must be allocated by “user pool”.  

```c
/* vm/frame.c */
void *
frame_alloc (struct vm_entry *vme, enum palloc_flags flags) 
{
    void *kpage = palloc_get_page (flags);

    ...

    return kpage; 
}
```

`frame_alloc`은 직접 `PAL_USER`를 하드코딩하지 않고, 인자로 받은 flags를 바로 `palloc_get_page`에 전달하도록 설계되었다. 함수 자체는 어떤 pool인지 신경 쓰지 않고, 호출할 때 명시적으로 지정하여 준다.


#### Modify process loading (the loop in “load_segment()”) in “userprog/process.c” for managing a frame table.

위 부분은 2. lazy loading과 관련되어 있어 다음에 설명하겠다.

## 2. Lazy loading

### requirements

Implement “lazy loading” for loading page to memory. 

∙ Current implementation: Executable codes needed for process start are directly loaded in memory. When page fault occurs, a program execution always stop by considering this situation as an invalid 
access error. 

∙ New implementation (5 points): Only a stack setup part is loaded during loading procedure for memory allocating when a process starts. (Other parts are not loaded in the memory. Just pages are allocated.) When a page fault is occurred from an allocated page, this page is loaded on memory. A page fault handler should resume a process operation when this procedure is ended. 

∙ Page fault handler modifies the page_fault() in "threads/exception.c". 

∙ For page fault handler, when a situation which needs I/O (lazy loading) and a situation which doesn’t need I/O (wrong memory access) occur simultaneously, the later situation must be handled first without 
waiting the earlier situation. 

### Implementation

기존 PINTOS는 process 생성 시 실행 파일의 모든 세그먼트를 물리 메모리에 즉시 로드했다. 이를 개선해서 초기화 시 stack page만 할당하고, 나머지 코드 및 데이터 세그먼트는 vm_entry(metadata)만 생성하고, 실제 접근이 발생할 때(page fault) 물리 메모리에 로드하는 lazy loading을 구현하였다.

#### load_segment() 수정

```c
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) >= 0);
  ASSERT (pg_ofs (upage) == (ofs % PGSIZE));

  off_t current_ofs = ofs;
  uint8_t *current_upage = upage;
  uint32_t total_bytes_to_process = read_bytes + zero_bytes;

  while (total_bytes_to_process > 0) 
  {
      size_t page_offset = pg_ofs(current_upage);
      size_t page_remaining = PGSIZE - page_offset;
      size_t page_read_bytes = (read_bytes < page_remaining) ? read_bytes : page_remaining;
      size_t page_zero_bytes = 0;
      if (page_read_bytes < page_remaining) {
          page_zero_bytes = (zero_bytes < (page_remaining - page_read_bytes))
                          ? zero_bytes : (page_remaining - page_read_bytes);
      }

      uint8_t *page_vaddr = pg_round_down(current_upage);
      struct vm_entry *vme = vm_find(&thread_current()->vm, page_vaddr);

      if (vme == NULL) {
          vme = malloc(sizeof(struct vm_entry));
          if (vme == NULL) return false;

          vme->type = VM_BIN;
          vme->vaddr = page_vaddr;
          vme->writable = writable;
          vme->is_loaded = false;
          vme->file = file;
          vme->thread = thread_current();

          vme->offset = current_ofs;
          vme->read_bytes = page_read_bytes;
          vme->zero_bytes = page_zero_bytes;
          vme->swap_index = BITMAP_ERROR;
          vme->pinned = false;

          if (!vm_insert(&thread_current()->vm, vme)) {
              free(vme);
              return false;
          }
      } else {
          if (writable && !vme->writable) {
              vme->writable = true;
          }
      }

      size_t bytes_processed_this_page = page_read_bytes + page_zero_bytes;

      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      total_bytes_to_process -= bytes_processed_this_page;

      current_ofs += page_read_bytes;
      current_upage += bytes_processed_this_page;
  }
  return true;
}
```

기존의 Eager Loading 방식을 제거했다. 대신 vm_entry를 생성해 파일 정보(file, offset, read_bytes)를 저장하고, `is_loaded = false`로 설정한 뒤 Supplemental Page Table(다음 chapter에서 설명 예정)에 등록만 수행하고 return한다.

#### setup_stack() 수정

```c
static bool
setup_stack (void **esp) 
{
    bool success = false;
    void *stack_upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
    struct thread *cur = thread_current();
    
    struct vm_entry *vme = vm_find (&cur->vm, stack_upage);
    if (vme == NULL) {
        vme = malloc(sizeof(struct vm_entry));
        if (vme == NULL) return false;

        vme->type = VM_ANON;
        vme->vaddr = stack_upage;
        vme->writable = true;
        vme->is_loaded = false;
        vme->file = NULL;
        vme->thread = cur;
        vme->swap_index = BITMAP_ERROR;
        vme->pinned = true;
        
        if (!vm_insert (&cur->vm, vme)) {
            free(vme);
            return false;
        }
    }

    uint8_t *kpage = frame_alloc (vme, PAL_USER | PAL_ZERO);
    if (kpage == NULL) {
        return false;
    }
    
    success = install_page (stack_upage, kpage, true);
    if (success) {
        vme->is_loaded = true;
        vme->kpage = kpage;
        *esp = PHYS_BASE;
        vme->pinned = false;
    } else {
        frame_free (kpage);
    }
    
    return success;
}
```

요구사항("Only a stack setup part is loaded... when a process starts")에 따라, 스택 페이지는 지연 로딩 없이 즉시 할당한다. `frame_alloc(PAL_USER)`를 호출해서 물리 프레임을 할다앋고, `install_page`로 매핑을 완료해서 프로그램이 시작될 준비를 마친다.


#### page_fault() 수정

```c
static void
page_fault (struct intr_frame *f) 
{
  ...

   if (is_kernel_vaddr(fault_addr)) {
        force_exit(-1);
    }
    
   if (!not_present) {
        force_exit(-1);
    }

    struct thread *cur = thread_current();
    
    void *esp = user ? f->esp : cur->user_esp;
    
    if (fault_addr >= esp - 32) {
        if (grow_stack(fault_addr, esp)) {
            return;
        }
    }

    struct vm_entry *vme = vm_find(&cur->vm, fault_addr);
    
    if (vme != NULL) {
        if (write && !vme->writable) {
           force_exit(-1);
        }
        
        if (load_page(vme)) {
            return; 
        }
    }

    force_exit(-1);
}
```

먼저 접근한 주소(fault_addr)가 커널 영역(is_kernel_addr)인지 확인한다. 또한, 페이지가 존재하는데 접근 권한을 위반한 경우(!not_present)인지 확인한다. 이 경우, lazy loading 대상이 아니며 즉시 프로세스를 종료한다. Lazy Loading 대상을 확인해서 쓰기 권한(writable)을 확인한 후, load_page(vme)를 호출해서 물리 프레임 할당 및 디스크 I/O를 수행한다. load_page가 성공하면 함수를 종료하여, 중단된 시점부터 프로세스를 실행을 재개한다.

#### load_page() 추가

```c
bool 
load_page (struct vm_entry *vme)
{
    if (vme->is_loaded) return true;

    void *kpage = frame_alloc (vme, PAL_USER | PAL_ZERO);
    if (kpage == NULL) return false;

    struct lock *filesys_lock = get_filesys_lock();

    if (vme->type == VM_BIN || vme->type == VM_FILE) {
        if (vme->swap_index == BITMAP_ERROR) {
            lock_acquire(filesys_lock);
             
             int read_bytes = file_read_at(vme->file, kpage + (vme->offset % PGSIZE), 
                                           vme->read_bytes, vme->offset);
             
            lock_release(filesys_lock);

             if (read_bytes != (int)vme->read_bytes) {
                  frame_free(kpage);
                  return false;
             }
        } else {
            swap_in(vme->swap_index, kpage);
            swap_free(vme->swap_index);
        }
    } else if (vme->type == VM_ANON) {
        if (vme->swap_index != BITMAP_ERROR) {
            swap_in(vme->swap_index, kpage);
            swap_free(vme->swap_index);
        }
    }
    if (!install_page(vme->vaddr, kpage, vme->writable)) {
        frame_free(kpage);
        return false;
    }

    vme->is_loaded = true;
    vme->swap_index = BITMAP_ERROR;
    return true;
}
```

실제 로딩을 수행한다.
frame_alloc을 호출해서 물리 프레임을 할당한다. vm_entry에 저장해둔 파일 오프셋 정보를 이용하여 file_read_at을 수행, 데이터를 물리 메모리에 적재한다. install_page를 호출하여 MMU가 인식할 수 있도록 매핑하고, is_loaded = true로 상태를 변경한다.

## 3. Supplemental Page Table

### requirements
 
Implement S-page table which have more functions than the previous page table. 

∙ S-page table (5 points): Implement S-page table and a managing function for S-page table. Lazy 
loading, file memory mapping, swap table must work normally.  

∙ Expected entry: page number, possibility of frame allocation, frame number, possibility of swap out, ... 

∙ Main functions 

✓ Allocate frames for each page by lazy loading.  

✓ Store the modified data in a frame into a file or a swap disk. 

✓ When page is deleted, the related entries should also be deleted by finding them in frame table, swap table, and etc.  

∙ We recommend using a Hash table. 

∙ A page fault handler in "threads/exception.c" should refer an S-page table. 

### Implementation

Pintos의 기본 페이지 테이블(pagedir)은 가상 주소를 물리 주소로 변환하는 기능만 제공한다. 하지만 Page Fault가 발생했을 때 데이터를 어디서 가져와야 하는지(Disk, Swap, All-zero), 그리고 프로세스 종료 시 어떤 자원을 해제해야 하는지 알기 위해 **Supplemental Page Table (SPT)**을 구현하였다.

#### Data Structure: struct vm_entry

```c
/* 페이지 타입 */
enum vm_type {
    VM_BIN,   /* 바이너리 실행 파일 (Code/Data) */
    VM_FILE,  /* 메모리 맵 파일 (mmap) */
    VM_ANON   /* 스택, 힙, 스왑 영역 (Anonymous) */
};
```

먼저 페이지 타입이다. 

+ `VM_BIN`(Binary): ELF 실행 파일(Binary)의 코드(.text)나 초기화된 데이터(.data) 세그먼트에서 유래한 페이지.Lazy Loading 시 파일에서 읽어와야 함을 나타낸다.

+ `VM_FILE`(File): `mmap()` 시스템 콜을 통해 특정 파일이 메모리에 매핑된 페이지. Swap out(Eviction) 시 변경 사항이 있다면 스왑 디스크가 아닌 **원본 파일 시스템에 기록(Write-back)**해야 한다.

+ `VM_ANON`(Anonymous): 파일과 직접적인 연관이 없는 메모리. (예: 스택(Stack), 힙(Heap)). 스왑 아웃 시 반드시 **스왑 디스크(Swap Partition)**로 이동해야 한다.

```c
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
```

**Supplemental Page Table Entry (SPTE)**로서, 하드웨어 페이지 테이블(pagedir)이 담지 못하는 추가 정보를 저장한다.

A. 기본 식별 정보

+ type: 위에서 정의한 페이지의 종류. Page Fault 처리 및 Eviction 정책 결정의 기준.

+ vaddr: 해당 페이지의 가상 주소입니다. 해시 테이블의 Key 역할.

+ writable: 페이지에 쓸 수 있는지 권한.

B. 물리 메모리 상태

+ is_loaded: 현재 이 가상 페이지가 물리 메모리(Frame)에 올라와 있는지 나타내는 플래그.

    + false: Lazy Loading 대기 중이거나 스왑 아웃된 상태.

    + true: 메모리에 존재함.

+ kpage: 물리 메모리에 로드된 경우, 해당 프레임의 **커널 가상 주소(Kernel Virtual Address)**. 물리 주소와 1:1 매핑되므로 프레임 번호 역할.

C. Lazy loading metadata

+ file, offset, read_bytes, zero_bytes: VM_BIN이나 VM_FILE 타입인 경우 사용됨. 아직 로드되지 않은 페이지를 디스크에서 읽어오기 위해 필요한 파일 포인터, 읽을 위치, 크기 정보를 보관함. load_page() 함수가 이 정보를 사용하여 디스크 I/O를 수행.

D. 스왑 관리

+ swap_index: VM_ANON 페이지가 메모리에서 쫓겨나(Evict) 스왑 디스크로 갔을 때, 스왑 파티션 내의 **슬롯 번호(인덱스)**를 저장한다. 나중에 다시 불러올 때(Swap In) 이 번호를 보고 찾는다.

E. 자료구조 연결

+ hash_elem elem: 스레드별 **Supplemental Page Table (Hash Table)**에 이 구조체를 담기 위한 요소.

+ list_elem f_elem: 전역 **Frame Table (List)**에 이 구조체를 연결하기 위한 요소.

*별도의 frame_entry를 만들지 않고, vm_entry 자체가 프레임 테이블의 노드 역할을 겸하도록 설계됨.*

F. Reverse Mapping & Sync

+ thread: 이 페이지를 소유한 스레드. 프레임 회수 시 페이지 테이블(pagedir)을 찾아가기 위해 필요.

+ pinned: 현재 디스크 I/O(read/write) 등이 진행 중이라 절대 쫓아내면 안 되는(Eviction 금지) 페이지임을 표시.

+ mapid: VM_FILE인 경우, munmap(mapid) 호출 시 같은 ID를 가진 페이지들을 한꺼번에 해제하기 위한 식별자.

#### vm/page.c 핵심 함수

A. `vm_init (struct hash *vm)`: 스레드가 생성될 때(start_process), 해당 스레드의 Supplemental Page Table(Hash Table)을 초기화. hash_init()을 호출하여 테이블을 생성하고, 해시 함수인 **vm_hash_func**와 비교 함수인 **vm_less_func**를 등록하여 vaddr을 기준으로 데이터를 관리할 준비를 마친다.

```c
void
vm_init (struct hash *vm)
{
    hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
    struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
    return hash_bytes (&vme->vaddr, sizeof (vme->vaddr));
}

static bool
vm_less_func (const struct hash_elem *a, const struct hash_elem *b,
              void *aux UNUSED)
{
    struct vm_entry *vme_a = hash_entry (a, struct vm_entry, elem);
    struct vm_entry *vme_b = hash_entry (b, struct vm_entry, elem);
    return vme_a->vaddr < vme_b->vaddr;
}
```

B. `vm_find (struct hash *vm, void *vaddr)`: Page Fault가 발생했을 때(page_fault), 해당 가상 주소(vaddr)에 대한 메타데이터(vm_entry)를 검색한다. 동시성 제어를 위해 **thread_current()->spt_lock**을 획득한 후 hash_find()를 호출한다. $O(1)$의 속도로 검색하여 vm_entry를 반환하거나, 존재하지 않으면 NULL을 반환하여 잘못된 접근임을 알린다.

```c
struct vm_entry *
vm_find (struct hash *vm, void *vaddr)
{
    struct vm_entry vme_temp;
    struct hash_elem *e;
    vme_temp.vaddr = pg_round_down (vaddr);
    
    lock_acquire(&thread_current()->spt_lock);
    e = hash_find (vm, &vme_temp.elem);
    lock_release(&thread_current()->spt_lock); 
    if (e == NULL) {
        return NULL;
    }
    return hash_entry (e, struct vm_entry, elem);
}
```

C. `vm_insert (struct hash *vm, struct vm_entry *vme)`: 새로운 가상 페이지 영역이 필요할 때(예: load_segment의 Lazy Loading 설정, setup_stack의 스택 초기화), 새로운 vm_entry를 생성하여 SPT에 등록한다. spt_lock을 사용하여 안전하게 hash_insert()를 수행합니다. 이를 통해 중복된 주소가 등록되는 것을 방지한다.

```c
bool
vm_insert (struct hash *vm, struct vm_entry *vme)
{
    lock_acquire(&thread_current()->spt_lock); 
    bool success = hash_insert (vm, &vme->elem) == NULL;
    lock_release(&thread_current()->spt_lock); 
    return success;
}
```

D. `vm_destroy (struct hash *vm)`: 프로세스가 종료될 때(process_exit), 할당받았던 모든 자원(프레임, 스왑 슬롯, 파일 매핑 등)을 일괄 해제합니다. hash_destroy()를 호출하며, 각 항목의 제거를 담당하는 **vm_destroy_func**를 실행한다. vm_destroy_func은 vm_munmap_page()를 호출하여 VM_FILE 타입인 경우 변경 사항을 디스크에 기록(Write-back)하고, 물리 프레임(frame_free)과 스왑 슬롯(swap_free)을 반환한다.

```c
static void
vm_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
    struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);

    /*if (vme->is_loaded || vme->swap_index != BITMAP_ERROR) {
        vm_munmap_page(vme);
    }*/

    vm_munmap_page(vme);
    
    free (vme);
}

void
vm_destroy (struct hash *vm)
{
    lock_acquire(&thread_current()->spt_lock);
    hash_destroy (vm, vm_destroy_func);
    lock_release(&thread_current()->spt_lock);
}
```

## 4. Stack growth

### requirements

Make it available to increase a size of a stack.  

∙ Current implementation: The size is fixed as 1 page. 

∙ New implementation (5 points): First, check whether a page fault handler needs a stack growth or 
not. If it needs, get a stack pointer from an interrupt frame (Calculation of the number of necessary 
pages is needed). Load pages, and modify the information of a frame table and S-page table.  

∙ Decide early the maximum size of a stack by considering stack/heap collision.  

### Implementation

기존 Pintos는 프로세스 생성 시 스택 크기가 1페이지(4KB)로 고정되어 있어, 깊은 재귀 호출이나 큰 지역 변수 배열을 사용할 경우 Stack Overflow가 발생했다. 이를 해결하기 위해 Page Fault가 발생했을 때, 해당 접근이 유효한 스택 확장 요청인지 판단하고 동적으로 페이지를 할당하는 Stack Growth 기능을 구현하였다.

#### 스택 확장 감지

```c
static void
page_fault (struct intr_frame *f) {
    ...

    void *esp = user ? f->esp : cur->user_esp;
    
    if (fault_addr >= esp - 32) {
        if (grow_stack(fault_addr, esp)) {
            return;
        }
    }
    ...
}
```

Page Fault 핸들러는 단순한 잘못된 메모리 접근과 스택 확장이 필요한 경우를 구분해야 한다. 접근하려는 주소(fault_addr)가 현재 스택 포인터(esp)보다 위에 있거나, 혹은 esp보다 약간 아래(esp - 32 bytes)에 있어야 한다. x86의 PUSHA 명령어는 스택 포인터를 갱신하기 전에 32바이트를 한꺼번에 푸시할 수 있으므로, 이 여유분을 허용해야 한다. Page Fault가 유저 모드에서 발생했다면 f->esp를 사용하지만, 커널 모드(시스템 콜 처리 중)에서 발생했다면 thread_current()->user_esp를 기준으로 판단한다. 조건이 만족되면 grow_stack() 함수를 호출한다.

#### 스택 페이지 할당

```c
#define STACK_MAX_SIZE (8 * 1024 * 1024)

bool
grow_stack (void *fault_addr, void *esp)
{
    if (fault_addr < (PHYS_BASE - STACK_MAX_SIZE) || fault_addr >= PHYS_BASE) {
        return false;
    }

    if (fault_addr < (esp - 32)) {
        return false;
    }
    void *stack_page = pg_round_down(fault_addr);
    
    if (vm_find(&thread_current()->vm, stack_page) != NULL) {
        return false;
    }

    struct vm_entry *vme = malloc(sizeof(struct vm_entry));
    if (vme == NULL) return false;

    vme->type = VM_ANON;
    vme->vaddr = stack_page;
    vme->writable = true;
    vme->is_loaded = false;
    vme->file = NULL;
    vme->thread = thread_current();
    vme->swap_index = BITMAP_ERROR; 
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
```

실제로 스택을 늘려주는 함수다.

STACK_MAX_SIZE (8MB) 제한을 두어, 무한 재귀 등으로 인해 스택이 힙(Heap) 영역을 침범하거나 메모리를 고갈시키는 것을 방지한다. (PHYS_BASE - 8MB 이상인지 확인) fault_addr를 페이지 단위로 내림(pg_round_down)하여 가상 페이지 주소를 구한다. 새로운 **vm_entry**를 생성한다. 이때 타입은 **VM_ANON (Anonymous)**으로 설정한다. (스택은 파일과 연결되지 않았기 때문)
vm_insert로 SPT에 등록한 뒤, load_page를 즉시 호출하여 물리 프레임(frame_alloc)을 할당하고 페이지 테이블에 매핑한다.

## 5. File Memory Mapping

### requirements

Make it available for files to be mapped with memories. 

∙ File memory mapping (5 points): Create a file mapping table which manages a relationship between 
files and pages, and implement mapid_t mmap(int fd, void *addr) and void munmap(mapid_t mapping) 
system call. 

∙ Implement mmap by lazy loading. When it is dirty (write), conduct “write back” on a disk to prevent a 
lack of memory.  

∙ When the size of a file and a page size are not same in mmap, fill the remaining bit as 0, and in write 
back case, ignore this part.  

∙ A call to mmap may fail if the file open as fd has a length of zero bytes. It must fail if addr is not page
aligned or if the range of pages mapped overlaps any existing set of mapped pages, including the 
stack or pages mapped at executable load time. It must also fail if addr is 0, because some Pintos code 
assumes virtual page 0 is not mapped. Finally, file descriptors 0 and 1, representing console input and 
output, are not mappable.

### Implementation

mmap 시스템 콜은 파일을 프로세스의 가상 주소 공간에 매핑하는 기능을 제공한다. 이를 위해 VM_FILE이라는 새로운 페이지 타입을 정의하였으며, Lazy Loading을 통해 매핑 즉시 메모리를 할당하지 않고 실제 접근 시 로드되도록 구현하였다. 또한, munmap이나 Eviction 시 변경된 데이터(Dirty Page)를 디스크에 반영하는 Write-back 메커니즘을 포함한다.

#### mmap

```c
static void syscall_handler (struct intr_frame *f) {
    struct thread *cur = thread_current();
    cur->user_esp = f->esp;

    if (check_address(f->esp, f->esp) == NULL) force_exit(-1);

    int syscall_number = *(int *)(f->esp);

    switch (syscall_number) {
        case SYS_MMAP:
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp)) force_exit(-1);
            f->eax = mmap(*(int *)(f->esp + 4), *(void **)(f->esp + 8));
            break;

    }
}

static mapid_t mmap (int fd, void *addr) {
    struct thread *cur = thread_current();
    if (addr == NULL || pg_ofs(addr) != 0) return -1;
    if (fd == 0 || fd == 1) return -1;
    struct file *file = cur->fd_table[fd];
    if (file == NULL) return -1;

    lock_acquire(&filesys_lock);
    off_t file_len = file_length(file);
    lock_release(&filesys_lock);
    if (file_len == 0) return -1;

    lock_acquire(&filesys_lock);
    struct file *reopened_file = file_reopen(file);
    lock_release(&filesys_lock);
    if (reopened_file == NULL) return -1;

    void *current_addr = addr;
    off_t current_offset = 0;
    while (current_offset < file_len) {
        if (vm_find(&cur->vm, current_addr) != NULL) {
            file_close(reopened_file);
            return -1;
        }
        current_addr += PGSIZE;
        current_offset += PGSIZE;
    }

    mapid_t mapid = cur->next_mapid++;
    current_addr = addr;
    current_offset = 0;
    while (current_offset < file_len) {
        struct vm_entry *vme = malloc(sizeof(struct vm_entry));
        if (vme == NULL) {
            file_close(reopened_file);
            return -1;
        }
        vme->type = VM_FILE;
        vme->vaddr = current_addr;
        vme->writable = true;
        vme->is_loaded = false;
        vme->file = reopened_file;
        vme->thread = cur;
        vme->pinned = false;
        vme->offset = current_offset;
        vme->read_bytes = (file_len - current_offset < PGSIZE) ? (file_len - current_offset) : PGSIZE;
        vme->zero_bytes = PGSIZE - vme->read_bytes;
        vme->swap_index = BITMAP_ERROR;
        vme->mapid = mapid;
        
        if (!vm_insert(&cur->vm, vme)) {
            free(vme);
            file_close(reopened_file);
            return -1;
        }
        current_addr += PGSIZE;
        current_offset += PGSIZE;
    }
    return mapid;
}
```

요청한 주소(addr)가 0이거나 페이지 정렬되지 않은 경우, 또는 기존에 매핑된 영역과 겹치는 경우를 검사하여 실패 처리한다.

file_reopen: 매핑된 파일이 외부에서 닫히거나 삭제되어도 매핑을 유지하기 위해 file_reopen을 사용하여 독립적인 파일 객체를 확보한다.

vm_entry 생성 (Lazy Mapping): 파일을 페이지 단위(PGSIZE)로 나누어 루프를 돈다. 각 페이지마다 vm_entry를 생성하고, **type = VM_FILE**로 설정한다. 실제 메모리 할당(frame_alloc)은 하지 않고, vm_insert만 수행하여 SPT에 등록한다. 반환값으로 고유한 **mapid**를 부여하여 나중에 munmap 시 그룹으로 식별할 수 있게 한다.

#### munmap

```c
static void syscall_handler (struct intr_frame *f) {
    struct thread *cur = thread_current();
    cur->user_esp = f->esp;

    if (check_address(f->esp, f->esp) == NULL) force_exit(-1);

    int syscall_number = *(int *)(f->esp);

    switch (syscall_number) {
        case SYS_MUNMAP:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            munmap(*(mapid_t *)(f->esp + 4));
            f->eax = 0;
            break;
    }
}

static void munmap (mapid_t mapid) {
    struct thread *cur = thread_current();
    struct hash *vm = &cur->vm;
    struct file *file_to_close = NULL;

    while (true) {
        struct vm_entry *target_vme = NULL;
        struct hash_iterator i;

        hash_first(&i, vm);
        while (hash_next(&i)) {
            struct vm_entry *vme = hash_entry(hash_cur(&i), struct vm_entry, elem);
            if (vme->type == VM_FILE && vme->mapid == mapid) {
                target_vme = vme;
                break;
            }
        }

        if (target_vme == NULL) break;

        if (file_to_close == NULL) {
            file_to_close = target_vme->file;
        }

        vm_munmap_page(target_vme);

        hash_delete(vm, &target_vme->elem);
        free(target_vme);
    }

    if (file_to_close != NULL) {
        lock_acquire(&filesys_lock);
        file_close(file_to_close);
        lock_release(&filesys_lock);
    }
}

// vm/page.c
void
vm_munmap_page (struct vm_entry *vme)
{
    if (vme->is_loaded) {
        bool is_dirty = pagedir_is_dirty(vme->thread->pagedir, vme->vaddr);
        
        if (vme->type == VM_FILE && is_dirty) {
            
            struct lock *fs_lock = get_filesys_lock();
            bool lock_was_held = lock_held_by_current_thread(fs_lock);
            
            if (!lock_was_held) lock_acquire(fs_lock);
            
            off_t write_bytes = vme->read_bytes;
            off_t file_len = file_length(vme->file);
            if (vme->offset + write_bytes > file_len) {
                 write_bytes = file_len - vme->offset;
            }
            
            off_t written = file_write_at(vme->file, vme->kpage, write_bytes, vme->offset);
            
            if (!lock_was_held) lock_release(fs_lock);
        }
        
        pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        frame_free(vme->kpage);
        
        vme->is_loaded = false;
        vme->kpage = NULL;
    }
    
    if (vme->swap_index != BITMAP_ERROR) {
        swap_free(vme->swap_index);
        vme->swap_index = BITMAP_ERROR;
    }
}
```

munmap(mapid): 스레드의 SPT를 순회하며 해당 mapid를 가진 모든 vm_entry를 찾아 **vm_munmap_page**를 호출한다.

vm_munmap_page (Write-back 수행): pagedir_is_dirty를 통해 해당 페이지가 메모리에 로드된 후 수정되었는지 확인한다. 수정된 경우(dirty == true), filesys_lock을 획득하고 file_write_at을 호출하여 변경 내용을 원본 파일에 덮어쓴다. 페이지 테이블 매핑을 해제하고(pagedir_clear_page), 물리 프레임을 반환(frame_free)한다.

## 6. Swap table

### requirements

Implement a swap table to handle efficiently the case of when free frame doesn’t exist. 

∙ Swap table (5 points): Create a swap disk, and implement a swap table to manage the swap disk.  

∙ Swap out: Copying a selected evicting page into swap disk to get free frames.  

∙ Swap in: Allocating a page in swap disk into a new frame when page fault occurs.    

∙ Use read/write functions based on a sector implemented in the “devices/block.h, .c” file. 

∙ We recommend using a Bitmap structure. (“lib/kernel/bitmap.h, .c”). 

∙ Swap disk need to be created additionally. When “pintos-mkdisk swap.disk n” is executed in “vm/build”, 
a partition named “swap.disk” with n MB is created.

### Implementation

물리 메모리가 부족할 때, 파일 기반 페이지(VM_FILE)는 원본 파일에 쓰면 되지만, 스택이나 힙 같은 익명 페이지(VM_ANON)는 저장할 곳이 없다. 이를 위해 Swap Disk를 별도로 생성하고, 이 공간을 페이지 단위의 슬롯(Slot)으로 나누어 관리하는 Swap Table을 구현하였다.

#### Data Structure

Swap Disk (struct block *swap_disk): BLOCK_SWAP 역할을 하는 블록 디바이스에 대한 포인터.

Swap Table (struct bitmap *swap_table): 스왑 영역의 가용성을 관리하기 위해 **비트맵(Bitmap)**을 사용했다.
비트가 0이면 해당 슬롯이 비어있음(Free), 1이면 사용 중(Occupied)임을 나타낸다.

Synchronization (struct lock swap_lock): 비트맵 접근 시 경쟁 상태를 방지하기 위한 락.

#### 핵심 함수

A. `swap_init (void)`

```c
void
swap_init (void) 
{
    swap_disk = block_get_role (BLOCK_SWAP);
    if (swap_disk == NULL) {
        return;
    }

    size_t swap_slots = block_size (swap_disk) / SECTORS_PER_PAGE;
    swap_table = bitmap_create (swap_slots);
    if (swap_table == NULL) {
        PANIC ("Failed to create swap table bitmap.");
    }
    
    lock_init (&swap_lock);
}
```

시스템 시작 시 스왑 시스템을 초기화한다. block_get_role(BLOCK_SWAP)으로 스왑 디스크를 가져온다.
디스크 크기에 맞춰 비트맵을 생성한다. (슬롯 개수 = 디스크 크기 / 페이지 크기) swap_lock을 초기화한다.

B. `swap_out (void *kpage)`

```c

size_t
swap_out (void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);
    size_t free_index = bitmap_scan_and_flip (swap_table, 0, 1, false);
    lock_release (&swap_lock); 

    if (free_index == BITMAP_ERROR) {
        return BITMAP_ERROR;
    }

    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_write (swap_disk, 
                     (free_index * SECTORS_PER_PAGE) + i, 
                     kpage + (i * BLOCK_SECTOR_SIZE));
    }

    return free_index;
}
```

메모리에서 쫓겨나는 페이지의 데이터를 스왑 디스크로 복사하고, 저장된 위치(인덱스)를 반환한다.
lock_acquire 후 bitmap_scan_and_flip을 통해 빈 슬롯(First Fit)을 찾고 사용 중으로 표시한다.
Pintos의 섹터 크기는 512바이트, 페이지 크기는 4096바이트이므로, 8번의 block_write 반복문을 통해 페이지 전체를 디스크에 쓴다.
할당된 슬롯의 인덱스(swap_index)를 반환합니다.

C. `swap_in (size_t swap_index, void *kpage)`

```c
void
swap_in (size_t swap_index, void *kpage) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);

    lock_acquire (&swap_lock);
    if (!bitmap_test (swap_table, swap_index)) {
        lock_release (&swap_lock);
        PANIC ("Invalid swap slot index read attempt: %zu", swap_index);
    }
    lock_release (&swap_lock);
    for (int i = 0; i < SECTORS_PER_PAGE; i++) {
        block_read (swap_disk, 
                    (swap_index * SECTORS_PER_PAGE) + i, 
                    kpage + (i * BLOCK_SECTOR_SIZE));
    }
}
```

스왑 디스크에 저장된 데이터를 다시 물리 메모리로 불러온다.
swap_index가 유효한지 비트맵을 통해 검증한다.
8번의 block_read 반복문을 수행하여 디스크 데이터를 kpage로 복원한다.
읽기가 완료되면 해당 슬롯은 해제(swap_free)하여 재사용 가능하게 만든다.

D. `swap_free (size_t swap_index)`

```c
void
swap_free (size_t swap_index) 
{
    ASSERT (swap_disk != NULL && swap_table != NULL);
    
    lock_acquire (&swap_lock);
    if (bitmap_test (swap_table, swap_index)) {
        bitmap_reset (swap_table, swap_index); 
    }
    lock_release (&swap_lock);
}
```

스왑 슬롯을 해제한다. (프로세스 종료 시 또는 Swap In 완료 시 호출)
비트맵의 해당 인덱스 비트를 0으로 설정(bitmap_reset)하여 반납한다.

## 7. On Process Termination

### requirements

Make it available to deallocate all using resources when a process is terminated.  

∙ On process termination (5 points): When a process is terminated, delete related contents in S-page 
table, frame table, and swap table. Close all related files (Dirty page needs to be written back) 

### Implmentation

프로세스가 종료(exit)될 때 시스템 자원(Resource)의 누수(Leak)를 방지하기 위해 process_exit 함수를 중심으로 일괄적인 자원 해제 루틴을 구현하였다. 이는 **가상 메모리 정리(VM Cleanup)**와 파일 시스템 정리(File Cleanup) 두 단계로 나뉜다.

A. 가상 메모리 자원 해제 (vm_destroy): process_exit의 가장 첫 단계에서 호출된다. 스레드가 관리하던 Supplemental Page Table(Hash Table)의 모든 엔트리를 순회한다.

+ 개별 페이지 정리 (vm_munmap_page): 각 vm_entry에 대해 다음 작업을 수행한다.

    + Write-back (Data Integrity): 페이지 타입이 VM_FILE이고 Dirty 상태라면, 변경된 데이터를 원본 파일에 기록(file_write_at)하여 데이터 일관성을 보장한다.

    + Frame Free: 물리 메모리에 로드된 페이지(is_loaded)라면 frame_free()를 호출하여 프레임 테이블에서 제거하고 물리 메모리를 반환한다.

    + Swap Free: 스왑 영역에 있던 페이지(swap_index != -1)라면 swap_free()를 호출하여 스왑 슬롯을 비운다.

B. 파일 디스크립터 정리 (process_exit): 가상 메모리 정리가 끝난 후 수행된다. 현재 실행 중인 바이너리 파일(executable_file)에 걸려있던 file_deny_write 락을 풀고 파일을 닫는다. 프로세스가 열었던 모든 파일 디스크립터(fd_table)를 순회하며 file_close()를 호출하여 닫는다. 파일 시스템 접근 시 경쟁 상태를 방지하기 위해 filesys_lock을 획득하고 안전하게 닫는다.

C. 부모 프로세스와의 동기화

모든 자원 해제가 완료된 시점(Safe Point)에서 sema_up(&cur->wait_sema)를 호출하여, 기다리고 있던 부모 프로세스(process_wait)에게 종료를 알립니다. 이는 부모가 자식의 자원 해제가 덜 된 상태에서 접근하는 것을 방지한다.
