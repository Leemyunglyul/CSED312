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

static void syscall_handler (struct intr_frame *);
static struct lock filesys_lock;

/* ROX 테스트를 위한 보조 구조체 및 함수 (아이노드 비교 방식) */
struct open_check_helper_inode {
    struct inode *inode;
    bool is_executable;
};

static void
check_if_inode_is_executable (struct thread *t, void *aux_)
{
    struct open_check_helper_inode *aux = aux_;
    if (aux->is_executable) return;

    if (t->executable_file != NULL) {
        if (file_get_inode(t->executable_file) == aux->inode) {
            aux->is_executable = true;
        }
    }
}
/* --- */

void force_exit(int status) {
    struct thread *cur = thread_current();
    cur->exit_status = status;
    printf("%s: exit(%d)\n", cur->name, status);

    sema_up(&cur->wait_sema);
    sema_down(&cur->free_sema);

    thread_exit();
}

static void validate_address(const void *uaddr) {
    if (uaddr == NULL || !is_user_vaddr(uaddr) || pagedir_get_page(thread_current()->pagedir, uaddr) == NULL) {
        force_exit(-1);
    }
}

static void validate_buffer(const void *uaddr, unsigned size) {
    if (size == 0) return;
    validate_address(uaddr);
    validate_address((const char *)uaddr + size - 1);
}

static void validate_string(const char *uaddr) {
    for (;; uaddr++) {
        validate_address(uaddr);
        if (*uaddr == '\0')
            break;
    }
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
  validate_address(f->esp);

  int syscall_number = *(int *)f->esp;
  struct thread *cur = thread_current();

  switch (syscall_number) {
    case SYS_HALT:
      shutdown_power_off();
      break;

    case SYS_EXIT:
      validate_address(f->esp + 4);
      int status = *(int *)(f->esp + 4);
      //struct thread *cur = thread_current();
      if (cur->executable_file != NULL) {
          file_allow_write(cur->executable_file);
      }
      force_exit(status); 
      break;

    case SYS_EXEC:
      validate_address(f->esp + 4);
      const char *cmd_line = *(const char **)(f->esp + 4);
      validate_string(cmd_line);
      f->eax = process_execute(cmd_line);
      break;

    case SYS_WAIT:
      validate_address(f->esp + 4);
      tid_t tid = *(tid_t *)(f->esp + 4);
      f->eax = process_wait(tid);
      break;

    case SYS_CREATE:
      validate_address(f->esp + 4);
      validate_address(f->esp + 8);
      const char *file_create = *(const char **)(f->esp + 4);
      unsigned initial_size = *(unsigned *)(f->esp + 8);
      validate_string(file_create);
      lock_acquire(&filesys_lock);
      f->eax = filesys_create(file_create, initial_size);
      lock_release(&filesys_lock);
      break;

    case SYS_REMOVE:
      validate_address(f->esp + 4);
      const char *file_remove = *(const char **)(f->esp + 4);
      validate_string(file_remove);
      lock_acquire(&filesys_lock);
      f->eax = filesys_remove(file_remove);
      lock_release(&filesys_lock);
      break;

    case SYS_OPEN: // 이 부분은 원래의 간단한 코드로 되돌립니다.
        validate_address(f->esp + 4);
        const char *file_open = *(const char **)(f->esp + 4);
        validate_string(file_open);
        
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
      validate_address(f->esp + 4);
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
      validate_address(f->esp + 4);
      validate_address(f->esp + 8);
      validate_address(f->esp + 12);
      int fd_read = *(int *)(f->esp + 4);
      void *buffer_read = *(void **)(f->esp + 8);
      unsigned size_read = *(unsigned *)(f->esp + 12);
      validate_buffer(buffer_read, size_read);

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
          if (cur->fd_table[fd_read] == NULL) {
              f->eax = -1;
          } else {
              lock_acquire(&filesys_lock);
              f->eax = file_read(cur->fd_table[fd_read], buffer_read, size_read);
              lock_release(&filesys_lock);
          }
      }
      break;

    case SYS_WRITE:
        validate_address(f->esp + 4);
        validate_address(f->esp + 8);
        validate_address(f->esp + 12);
        int fd_write = *(int *)(f->esp + 4);
        const void *buffer_write = *(const void **)(f->esp + 8);
        unsigned size_write = *(unsigned *)(f->esp + 12);
        validate_buffer(buffer_write, size_write);

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
              // file_write가 알아서 deny_write_cnt를 확인합니다.
              lock_acquire(&filesys_lock);
              f->eax = file_write(file_to_write, buffer_write, size_write);
              lock_release(&filesys_lock);
          }
      }
        break;

    case SYS_SEEK:
      validate_address(f->esp + 4);
      validate_address(f->esp + 8);
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
      validate_address(f->esp + 4);
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
      validate_address(f->esp + 4);
      int fd_close = *(int *)(f->esp + 4);
      if (fd_close < 2 || fd_close >= FDT_SIZE) {
          force_exit(-1);
      }
      //struct thread *cur = thread_current();
      if (cur->fd_table[fd_close] == NULL) {
          force_exit(-1);
      }
      lock_acquire(&filesys_lock);
      file_close(cur->fd_table[fd_close]);
      lock_release(&filesys_lock);
      cur->fd_table[fd_close] = NULL;
      break;

    default:
      force_exit(-1);
      break;
  }
}

