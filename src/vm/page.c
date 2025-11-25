#include "vm/page.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include <string.h>
#include "threads/synch.h"
#include "userprog/syscall.h"
#include "lib/kernel/bitmap.h"
#include "userprog/pagedir.h"

#define STACK_MAX_SIZE (8 * 1024 * 1024)

void
vm_munmap_page (struct vm_entry *vme)
{
    if (vme->is_loaded) {
        /* Dirty 비트 확인 */
        bool is_dirty = pagedir_is_dirty(vme->thread->pagedir, vme->vaddr);
        
        /* [디버깅] mmap 페이지 해제 시 상태 출력 */
        /*if (vme->type == VM_FILE) {
             printf("DEBUG: munmap vaddr=%p, type=FILE, dirty=%d\n", 
                    vme->vaddr, is_dirty);
        }*/

        if (vme->type == VM_FILE && is_dirty) {
            
            struct lock *fs_lock = get_filesys_lock();
            bool lock_was_held = lock_held_by_current_thread(fs_lock);
            
            if (!lock_was_held) lock_acquire(fs_lock);
            
            /* 쓰기 크기 계산 */
            off_t write_bytes = vme->read_bytes;
            off_t file_len = file_length(vme->file);
            if (vme->offset + write_bytes > file_len) {
                 write_bytes = file_len - vme->offset;
            }
            
            /* 파일 쓰기 수행 및 결과 확인 */
            off_t written = file_write_at(vme->file, vme->kpage, write_bytes, vme->offset);
            
            //printf("DEBUG: file_write_at result=%d, expected=%d\n", (int)written, (int)write_bytes);

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

void
vm_init (struct hash *vm)
{
    hash_init (vm, vm_hash_func, vm_less_func, NULL);
    //lock_init(&thread_current()->spt_lock);
}

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

bool
vm_insert (struct hash *vm, struct vm_entry *vme)
{
    lock_acquire(&thread_current()->spt_lock); 
    bool success = hash_insert (vm, &vme->elem) == NULL;
    lock_release(&thread_current()->spt_lock); 
    return success;
}

bool
vm_delete (struct hash *vm, struct vm_entry *vme)
{
    lock_acquire(&thread_current()->spt_lock);
    bool success = hash_delete (vm, &vme->elem) != NULL;
    lock_release(&thread_current()->spt_lock);
    return success;
}

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
             
             /* 파일 읽기 시도 */
             int read_bytes = file_read_at(vme->file, kpage + (vme->offset % PGSIZE), 
                                           vme->read_bytes, vme->offset);
             
             /* 읽기 후, 내가 잡았던 락이라면 해제 */
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
