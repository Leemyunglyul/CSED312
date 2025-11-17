#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h" 
#include "userprog/process.h"
#include "threads/synch.h"
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h" 
#include "vm/page.h"
#include "threads/malloc.h" // mmap에서 vm_entry 생성 시 필요
#include "lib/kernel/bitmap.h"
#include <hash.h>

static void syscall_handler (struct intr_frame *);
struct lock filesys_lock;

static void unpin_all_frames_iterator(struct hash_elem *e, void *aux UNUSED) {
    struct vm_entry *vme = hash_entry(e, struct vm_entry, elem);
    vme->pinned = false;
}

static void unpin_all_frames(void) {
    struct hash *vm = &thread_current()->vm;
    hash_apply(vm, unpin_all_frames_iterator);
}

void force_exit(int status) {
    struct thread *cur = thread_current();
    cur->exit_status = status;
    printf("%s: exit(%d)\n", cur->name, status);

    sema_up(&cur->wait_sema);
    sema_down(&cur->free_sema);

    thread_exit();
}

static bool validate_address(const void *uaddr) {
    if (uaddr == NULL || uaddr >= PHYS_BASE || !is_user_vaddr(uaddr)) {
        return false;
    }

    struct thread *cur = thread_current();
    void *fault_page = pg_round_down(uaddr);
    struct vm_entry *vme = vm_find(&cur->vm, pg_round_down(uaddr));
    if (vme == NULL) {
        if (!grow_stack(uaddr, cur->user_esp)) {
            return false;
        }
        vme = vm_find(&cur->vm, fault_page);
        if (vme == NULL) {
            /* grow_stack이 성공했는데 vme가 없으면 커널 오류 */
            return false; 
        }
    }
    if (!vme->is_loaded) {
        if (!load_page(vme)) { // 로드 실패
            return false;
        }
    }
    vme->pinned = true; 
    return true; // 성공
}

/* [수정] bool을 반환하도록 변경 */
static bool validate_buffer(const void *uaddr, unsigned size) {
    if (size == 0) return true;

    char *ptr = (char *) pg_round_down(uaddr);
    char *end = (char *) uaddr + size;

    while (ptr < end) {
        if (!validate_address(ptr)) return false;
        ptr += PGSIZE;
    }
    
    /* * 버퍼의 마지막 바이트도 검사합니다.
     * * (size가 0이 아님은 위에서 확인했습니다.)
     */
    if (!validate_address((const char *)uaddr + size - 1)) return false;
    
    return true;
}

/* [수정] bool을 반환하도록 변경 */
static bool validate_string(const char *uaddr) {
    if (!validate_address(uaddr)) return false; // 첫 페이지 검사
    
    const char *page = pg_round_down(uaddr);
    while (true) {
        /* * 페이지 경계를 넘어가면, 다음 페이지도 
         * * validate (및 로드)해야 합니다.
         */
        if (pg_round_down(uaddr) != page) {
            page = pg_round_down(uaddr);
            if (!validate_address(uaddr)) return false;
        }
        
        /* * validate_address()가 이 페이지를 메모리에 로드했음을
         * * 보장하므로, *uaddr 역참조는 안전합니다.
         */
        if (*uaddr == '\0')
            break;
            
        uaddr++; // 다음 바이트로 이동
    }
    return true;
}

static mapid_t
mmap (int fd, void *addr)
{
    struct thread *cur = thread_current();

    /* 1. 주소 유효성 검사 */
    if (addr == NULL || pg_ofs(addr) != 0) {
        return -1; // 널 포인터 또는 페이지 정렬 안 됨
    }

    /* 2. fd 유효성 검사 */
    if (fd == 0 || fd == 1) {
        return -1; // STDIN/STDOUT 매핑 불가
    }
    struct file *file = cur->fd_table[fd];
    if (file == NULL) {
        return -1; // 유효하지 않은 fd
    }
    
    /* 3. 파일 길이 검사 */
    lock_acquire(&filesys_lock);
    off_t file_len = file_length(file);
    lock_release(&filesys_lock);
    
    if (file_len == 0) {
        return -1; // 빈 파일 매핑 불가
    }

    /* 4. [중요] file_reopen: mmap이 유지되는 동안 
     * 다른 곳(e.g. close(fd))에서 파일을 닫아도 
     * 커널은 이 파일을 계속 참조해야 함.
     */
    lock_acquire(&filesys_lock);
    struct file *reopened_file = file_reopen(file);
    lock_release(&filesys_lock);
    if (reopened_file == NULL) {
        return -1; // 파일 리오픈 실패
    }
    
    /* 5. 페이지 겹침(Overlap) 검사 */
    /* (파일을 페이지 단위로 순회하며) */
    void *current_addr = addr;
    off_t current_offset = 0;
    while (current_offset < file_len) 
    {
        if (vm_find(&cur->vm, current_addr) != NULL) {
            file_close(reopened_file); // 실패 시 리오픈한 파일 닫기
            return -1; // 이미 SPT에 매핑된 주소 (겹침)
        }
        current_addr += PGSIZE;
        current_offset += PGSIZE;
    }
    
    /* 6. 모든 검사 통과 -> vm_entry 생성 (지연 로딩 설정) */
    mapid_t mapid = cur->next_mapid++;
    current_addr = addr;
    current_offset = 0;
    
    while (current_offset < file_len)
    {
        struct vm_entry *vme = malloc(sizeof(struct vm_entry));
        if (vme == NULL) {
            /* (TODO: 실패 시 지금까지 만든 vme들 롤백해야 함) */
            file_close(reopened_file);
            return -1; 
        }

        vme->type = VM_FILE; // mmap 타입
        vme->vaddr = current_addr;
        vme->writable = true; // mmap은 기본적으로 쓰기 가능
        vme->is_loaded = false;
        vme->file = reopened_file;
        vme->thread = cur;
        vme->pinned = false;
        
        vme->offset = current_offset;
        vme->read_bytes = (file_len - current_offset < PGSIZE) ? (file_len - current_offset) : PGSIZE;
        vme->zero_bytes = PGSIZE - vme->read_bytes;
        
        vme->swap_index = BITMAP_ERROR;
        vme->mapid = mapid; // 이 vme가 속한 map ID
        
        if (!vm_insert(&cur->vm, vme)) {
             /* (TODO: 롤백) */
            free(vme);
            file_close(reopened_file);
            return -1;
        }

        current_addr += PGSIZE;
        current_offset += PGSIZE;
    }
    
    return mapid;
}

/* munmap 헬퍼 함수 (일단 뼈대만) */
/* userprog/syscall.c -> munmap() */

static void
munmap (mapid_t mapid)
{
    struct thread *cur = thread_current();
    struct hash *vm = &cur->vm;
    
    struct hash_iterator i;
    struct file *file_to_close = NULL; 
    bool found = false;

    hash_first (&i, vm);
    while (hash_next (&i))
    {
        struct vm_entry *vme = hash_entry (hash_cur (&i), struct vm_entry, elem);

        if (vme->type == VM_FILE && vme->mapid == mapid) 
        {
            if (file_to_close == NULL) {
                file_to_close = vme->file; 
            }
            
            vm_munmap_page(vme); // Write-back 및 프레임 해제
            
            hash_delete (vm, &vme->elem);
            hash_first(&i, vm); // 반복자 리셋
            free (vme);
            found = true;
        }
    }
    
    // 4. mmap이 reopen한 파일 닫기
    if (file_to_close != NULL && found) {
        lock_acquire(&filesys_lock);
        file_close(file_to_close);
        lock_release(&filesys_lock);
    }
    // munmap은 성공/실패 코드를 반환하지 않습니다.
}

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init(&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f) 
{
    struct thread *cur = thread_current();
    cur->user_esp = f->esp; // [핵심 추가] 유저 esp 저장

    if (!validate_address(f->esp)) {
        force_exit(-1);
    }

    int syscall_number = *(int *)(f->esp);


  switch (syscall_number) {
    case SYS_HALT:
        shutdown_power_off();
        break;

    case SYS_EXIT:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        int status = *(int *)(f->esp + 4);
        if (cur->executable_file != NULL) {
            file_allow_write(cur->executable_file);
        }
        force_exit(status); 
        break;

    case SYS_EXEC:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        const char *cmd_line = *(const char **)(f->esp + 4);
        if (!validate_string(cmd_line)) {
            force_exit(-1);
        }
        f->eax = process_execute(cmd_line);
        break;

    case SYS_WAIT:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        tid_t tid = *(tid_t *)(f->esp + 4);
        f->eax = process_wait(tid);
        break;

    case SYS_CREATE:
        if (!validate_address(f->esp + 4) || !validate_address(f->esp + 8)) {
            force_exit(-1);
        }
        const char *file_create = *(const char **)(f->esp + 4);
        unsigned initial_size = *(unsigned *)(f->esp + 8);
        if (!validate_string(file_create)) {
            force_exit(-1);
        }
        lock_acquire(&filesys_lock);
        f->eax = filesys_create(file_create, initial_size);
        lock_release(&filesys_lock);
        break;

    case SYS_REMOVE:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        const char *file_remove = *(const char **)(f->esp + 4);
        if (!validate_string(file_remove)) {
            force_exit(-1);
        }
        lock_acquire(&filesys_lock);
        f->eax = filesys_remove(file_remove);
        lock_release(&filesys_lock);
        break;

    case SYS_OPEN:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        const char *file_open = *(const char **)(f->esp + 4);
        if (!validate_string(file_open)) {
            force_exit(-1);
        }
        
        lock_acquire(&filesys_lock);
        struct file *file_obj = filesys_open(file_open);
        lock_release(&filesys_lock);

        if (file_obj == NULL) {
            f->eax = -1;
        } else {
            struct thread *cur = thread_current();
            if (cur->next_fd < FDT_SIZE) {
                cur->fd_table[cur->next_fd] = file_obj;
                f->eax = cur->next_fd;
                cur->next_fd++;
            } else {
                file_close(file_obj);
                f->eax = -1;
            }
        }
        break;

    case SYS_FILESIZE:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        int fd_size = *(int *)(f->esp + 4);
        if (fd_size < 2 || fd_size >= FDT_SIZE) {
            f->eax = -1;
        } else {
            struct thread *cur = thread_current();
            if (cur->fd_table[fd_size] == NULL) {
                f->eax = -1;
            } else {
                lock_acquire(&filesys_lock);
                f->eax = file_length(cur->fd_table[fd_size]);
                lock_release(&filesys_lock);
            }
        }
        break;

    case SYS_READ:
        if (!validate_address(f->esp + 4) || !validate_address(f->esp + 8) || !validate_address(f->esp + 12)) {
            force_exit(-1);
        }
        int fd_read = *(int *)(f->esp + 4);
        void *buffer_read = *(void **)(f->esp + 8);
        unsigned size_read = *(unsigned *)(f->esp + 12);
        if (!validate_buffer(buffer_read, size_read)) force_exit(-1);

        if (fd_read == 0) {
            unsigned i;
            uint8_t *local_buffer = (uint8_t *) buffer_read;
            for (i = 0; i < size_read; i++) {
                local_buffer[i] = input_getc();
            }
            f->eax = i;
        } else if (fd_read < 2 || fd_read >= FDT_SIZE) {
            f->eax = -1;
        } else {
            struct thread *cur = thread_current();
            lock_acquire(&filesys_lock);
            if (cur->fd_table[fd_read] == NULL) {
                f->eax = -1;
            } else {
                f->eax = file_read(cur->fd_table[fd_read], buffer_read, size_read);
            }
            lock_release(&filesys_lock);
        }
        break;

    case SYS_WRITE:
        if (!validate_address(f->esp + 4) || !validate_address(f->esp + 8) || !validate_address(f->esp + 12)) {
            force_exit(-1);
        }
        int fd_write = *(int *)(f->esp + 4);
        const void *buffer_write = *(const void **)(f->esp + 8);
        unsigned size_write = *(unsigned *)(f->esp + 12);
        if (!validate_buffer(buffer_write, size_write)) {
            force_exit(-1);
        }

        if (fd_write == 1) { // STDOUT
            putbuf(buffer_write, size_write);
            f->eax = size_write;
        } else if (fd_write < 2 || fd_write >= FDT_SIZE) { // 잘못된 fd
            f->eax = -1;
        } else {
          struct thread *cur = thread_current();
          struct file *file_to_write = cur->fd_table[fd_write];
          if (file_to_write == NULL) { // 닫혔거나 없는 fd
              f->eax = -1;
          } else {
            lock_acquire(&filesys_lock);
            f->eax = file_write(file_to_write, buffer_write, size_write);
            lock_release(&filesys_lock);
          }
      }
        break;

    case SYS_SEEK:
      if (!validate_address(f->esp + 4) || !validate_address(f->esp + 8)) {
          force_exit(-1);
      }
      int fd_seek = *(int *)(f->esp + 4);
      unsigned position = *(unsigned *)(f->esp + 8);
      if (fd_seek >= 2 && fd_seek < FDT_SIZE) {
          struct thread *cur = thread_current();
          if (cur->fd_table[fd_seek] != NULL) {
              lock_acquire(&filesys_lock);
              file_seek(cur->fd_table[fd_seek], position);
              lock_release(&filesys_lock);
          }
      }
      break;

    case SYS_TELL:
      if (!validate_address(f->esp + 4)) {
          force_exit(-1);
      }
      int fd_tell = *(int *)(f->esp + 4);
      if (fd_tell < 2 || fd_tell >= FDT_SIZE) {
          f->eax = -1;
      } else {
          struct thread *cur = thread_current();
          if (cur->fd_table[fd_tell] == NULL) {
              f->eax = -1;
          } else {
              lock_acquire(&filesys_lock);
              f->eax = file_tell(cur->fd_table[fd_tell]);
              lock_release(&filesys_lock);
          }
      }
      break;

    case SYS_CLOSE:
      if (!validate_address(f->esp + 4)) {
          force_exit(-1);
      }
      int fd_close = *(int *)(f->esp + 4);
      if (fd_close < 2 || fd_close >= FDT_SIZE) {
          force_exit(-1);
      }
      if (cur->fd_table[fd_close] == NULL) {
          force_exit(-1);
      }
      lock_acquire(&filesys_lock);
      file_close(cur->fd_table[fd_close]);
      lock_release(&filesys_lock);
      cur->fd_table[fd_close] = NULL;
      break;

    case SYS_MMAP:
        if (!validate_address(f->esp + 4) || !validate_address(f->esp + 8)) {
            force_exit(-1);
        }
        int fd_mmap = *(int *)(f->esp + 4);
        void *addr_mmap = *(void **)(f->esp + 8);
        
        f->eax = mmap(fd_mmap, addr_mmap);
        break;
        
    case SYS_MUNMAP:
        if (!validate_address(f->esp + 4)) {
            force_exit(-1);
        }
        mapid_t mapid_munmap = *(mapid_t *)(f->esp + 4);
        munmap(mapid_munmap);
        f->eax = 0; // 성공 시 0 반환
        break;

    default:
      force_exit(-1);
      break;
  }
  unpin_all_frames();
}
