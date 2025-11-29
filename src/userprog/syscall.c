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
#include "vm/frame.h"
#include "threads/malloc.h"
#include "lib/kernel/bitmap.h"
#include <hash.h>

static void syscall_handler (struct intr_frame *);

static struct lock filesys_lock;

struct lock* get_filesys_lock(void) {
    return &filesys_lock;
}

void force_exit(int status) {
    struct thread *cur = thread_current();
    cur->exit_status = status;
    printf("%s: exit(%d)\n", cur->name, status);

    //sema_up(&cur->wait_sema);
    //sema_down(&cur->free_sema);

    thread_exit();
}

static struct vm_entry *check_address(void *uaddr, void *esp) {
    if (uaddr == NULL || uaddr >= PHYS_BASE || !is_user_vaddr(uaddr)) {
        return NULL;
    }

    struct thread *cur = thread_current();
    void *fault_page = pg_round_down(uaddr);
    struct vm_entry *vme = vm_find(&cur->vm, fault_page);
    
    if (vme == NULL) {
        return NULL;
    }
    
    if (!vme->is_loaded) {
        if (!load_page(vme)) return NULL;
    }
    return vme;
}

static void
check_valid_buffer (void *buffer, size_t size, bool to_write)
{
    if (buffer == NULL) {
        force_exit(-1);
    }
  
    struct thread *cur = thread_current();
    void *page;

    for (page = pg_round_down(buffer); 
         page <= pg_round_down(buffer + size - 1); 
         page += PGSIZE)
    {
        struct vm_entry *vme = check_address(page, cur->user_esp);
        if (vme == NULL) {
            force_exit(-1);
        }
       if (to_write && !vme->writable) {
            force_exit(-1);
        }
    }
}

static bool check_page_and_pin(void *addr, bool to_write) {
    struct vm_entry *vme = check_address(addr, thread_current()->user_esp);
    if (vme == NULL) return false;
    if (to_write && !vme->writable) return false;
    vme->pinned = true;
    return true;
}

static void unpin_page(void *addr) {
    struct vm_entry *vme = vm_find(&thread_current()->vm, pg_round_down(addr));
    if (vme) vme->pinned = false;
}

static bool validate_string(const char *uaddr) {
    if (check_address((void *)uaddr, thread_current()->user_esp) == NULL) return false;
    
    const char *page = pg_round_down(uaddr);
    while (*uaddr != '\0') {
        uaddr++;
        if (pg_round_down(uaddr) != page) {
            page = pg_round_down(uaddr);
            if (check_address((void *)uaddr, thread_current()->user_esp) == NULL) return false;
        }
    }
    return true;
}

static mapid_t mmap (int fd, void *addr); 
static void munmap (mapid_t mapid);

void syscall_init (void) {
    intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
}

static void syscall_handler (struct intr_frame *f) {
    struct thread *cur = thread_current();
    cur->user_esp = f->esp;

    if (check_address(f->esp, f->esp) == NULL) force_exit(-1);

    int syscall_number = *(int *)(f->esp);

    switch (syscall_number) {
        case SYS_HALT: shutdown_power_off(); break;

        case SYS_EXIT:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            int status = *(int *)(f->esp + 4);
            if (cur->executable_file) file_allow_write(cur->executable_file);
            force_exit(status);
            break;

        case SYS_EXEC:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            if (!validate_string(*(char **)(f->esp + 4))) force_exit(-1);
            f->eax = process_execute(*(char **)(f->esp + 4));
            break;

        case SYS_WAIT:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            f->eax = process_wait(*(tid_t *)(f->esp + 4));
            break;

        case SYS_CREATE:
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp)) force_exit(-1);
            if (!validate_string(*(char **)(f->esp + 4))) force_exit(-1);
            lock_acquire(&filesys_lock);
            f->eax = filesys_create(*(char **)(f->esp + 4), *(unsigned *)(f->esp + 8));
            lock_release(&filesys_lock);
            break;

        case SYS_REMOVE:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            if (!validate_string(*(char **)(f->esp + 4))) force_exit(-1);
            lock_acquire(&filesys_lock);
            f->eax = filesys_remove(*(char **)(f->esp + 4));
            lock_release(&filesys_lock);
            break;

        case SYS_OPEN:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            if (!validate_string(*(char **)(f->esp + 4))) force_exit(-1);
            lock_acquire(&filesys_lock);
            struct file *file_obj = filesys_open(*(char **)(f->esp + 4));
            lock_release(&filesys_lock);
            if (!file_obj) f->eax = -1;
            else if (cur->next_fd < FDT_SIZE) cur->fd_table[cur->next_fd++] = file_obj, f->eax = cur->next_fd - 1;
            else file_close(file_obj), f->eax = -1;
            break;

        case SYS_FILESIZE:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            int fd_sz = *(int *)(f->esp + 4);
            if (fd_sz < 2 || fd_sz >= FDT_SIZE || !cur->fd_table[fd_sz]) f->eax = -1;
            else {
                lock_acquire(&filesys_lock);
                f->eax = file_length(cur->fd_table[fd_sz]);
                lock_release(&filesys_lock);
            }
            break;
        case SYS_READ:
        {
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp) || !check_address(f->esp + 12, f->esp)) force_exit(-1);
            int fd = *(int *)(f->esp + 4);
            void *buf = *(void **)(f->esp + 8);
            unsigned size = *(unsigned *)(f->esp + 12);
            int bytes_read = 0;
            check_valid_buffer(buf, size, true);

            while (size > 0) {
                size_t chunk_size = PGSIZE - pg_ofs(buf);
                if (chunk_size > size) chunk_size = size;

                if (!check_page_and_pin(buf, true)) force_exit(-1);

                int ret = 0;
                if (fd == 0) {
                    for (size_t i = 0; i < chunk_size; i++) ((uint8_t*)buf)[i] = input_getc();
                    ret = chunk_size;
                } else if (fd >= 2 && fd < FDT_SIZE && cur->fd_table[fd]) {
                    lock_acquire(&filesys_lock);
                    ret = file_read(cur->fd_table[fd], buf, chunk_size);
                    lock_release(&filesys_lock);
                } else {
                    unpin_page(buf);
                    f->eax = -1; 
                    return; 
                }

                unpin_page(buf);

                if (ret < 0) { f->eax = -1; return; }
                bytes_read += ret;
                if (ret < (int)chunk_size) break;

                buf += chunk_size;
                size -= chunk_size;
            }
            f->eax = bytes_read;
            break;
        }

        case SYS_WRITE:
        {
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp) || !check_address(f->esp + 12, f->esp)) force_exit(-1);
            int fd = *(int *)(f->esp + 4);
            void *buf = *(void **)(f->esp + 8);
            unsigned size = *(unsigned *)(f->esp + 12);
            int bytes_written = 0;
            check_valid_buffer(buf, size, false);

            /* [Chunking Loop] */
            while (size > 0) {
                size_t chunk_size = PGSIZE - pg_ofs(buf);
                if (chunk_size > size) chunk_size = size;

                if (!check_page_and_pin(buf, false)) force_exit(-1);

                int ret = 0;
                if (fd == 1) {
                    putbuf(buf, chunk_size);
                    ret = chunk_size;
                } else if (fd >= 2 && fd < FDT_SIZE && cur->fd_table[fd]) {
                    lock_acquire(&filesys_lock);
                    ret = file_write(cur->fd_table[fd], buf, chunk_size);
                    lock_release(&filesys_lock);
                } else {
                    unpin_page(buf);
                    f->eax = -1;
                    return;
                }

                unpin_page(buf);

                if (ret < 0) { f->eax = -1; return; }
                bytes_written += ret;
                
                buf += chunk_size;
                size -= chunk_size;
            }
            f->eax = bytes_written;
            break;
        }

        case SYS_SEEK:
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp)) force_exit(-1);
            int fd_sk = *(int *)(f->esp + 4);
            unsigned pos = *(unsigned *)(f->esp + 8);
            if (fd_sk >= 2 && fd_sk < FDT_SIZE && cur->fd_table[fd_sk]) {
                lock_acquire(&filesys_lock);
                file_seek(cur->fd_table[fd_sk], pos);
                lock_release(&filesys_lock);
            }
            break;

        case SYS_TELL:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            int fd_tl = *(int *)(f->esp + 4);
            if (fd_tl >= 2 && fd_tl < FDT_SIZE && cur->fd_table[fd_tl]) {
                lock_acquire(&filesys_lock);
                f->eax = file_tell(cur->fd_table[fd_tl]);
                lock_release(&filesys_lock);
            } else f->eax = -1;
            break;

        case SYS_CLOSE:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            int fd_cl = *(int *)(f->esp + 4);
            if (fd_cl >= 2 && fd_cl < FDT_SIZE && cur->fd_table[fd_cl]) {
                lock_acquire(&filesys_lock);
                file_close(cur->fd_table[fd_cl]);
                lock_release(&filesys_lock);
                cur->fd_table[fd_cl] = NULL;
            } else force_exit(-1);
            break;

        case SYS_MMAP:
            if (!check_address(f->esp + 4, f->esp) || !check_address(f->esp + 8, f->esp)) force_exit(-1);
            f->eax = mmap(*(int *)(f->esp + 4), *(void **)(f->esp + 8));
            break;
            
        case SYS_MUNMAP:
            if (!check_address(f->esp + 4, f->esp)) force_exit(-1);
            munmap(*(mapid_t *)(f->esp + 4));
            f->eax = 0;
            break;

        default: force_exit(-1); break;
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