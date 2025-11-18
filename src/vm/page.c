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

extern struct lock filesys_lock;

void
vm_munmap_page (struct vm_entry *vme)
{
    if (vme->is_loaded) {
        /* 1. Dirty 확인 (매핑 끊기 전에 확인 필수!) */
        bool is_dirty = pagedir_is_dirty(vme->thread->pagedir, vme->vaddr);
        
        /* 2. 파일 매핑이고 Dirty라면 파일에 기록 */
        if (vme->type == VM_FILE && is_dirty) {
            lock_acquire(&filesys_lock);
            
            off_t write_bytes = vme->read_bytes;
            off_t file_len = file_length(vme->file);
            
            if (vme->offset + write_bytes > file_len) {
                 write_bytes = file_len - vme->offset;
            }
            
            file_write_at(vme->file, vme->kpage, write_bytes, vme->offset);
            
            lock_release(&filesys_lock);
        }
        
        /* 3. 매핑 해제 */
        pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        
        /* 4. 프레임 반환 */
        frame_free(vme->kpage);
        
        vme->is_loaded = false;
        vme->kpage = NULL;
    }
    
    /* 스왑 슬롯 해제 */
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

    bool lock_held = lock_held_by_current_thread(&filesys_lock);

    if (vme->type == VM_BIN || vme->type == VM_FILE) {
        if (vme->swap_index == BITMAP_ERROR) {
             if (!lock_held) lock_acquire(&filesys_lock);
             
             if (file_read_at(vme->file, kpage + (vme->offset % PGSIZE), 
                              vme->read_bytes, vme->offset) != (int)vme->read_bytes) {
                 if (!lock_held) lock_release(&filesys_lock);
                 frame_free(kpage);
                 return false;
             }
             
             if (!lock_held) lock_release(&filesys_lock);
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

static void
munmap_helper (struct hash_elem *e, void *aux)
{
    mapid_t *mapid = (mapid_t *)aux;
    struct vm_entry *vme = hash_entry(e, struct vm_entry, elem);

    if (vme->type == VM_FILE && vme->mapid == *mapid) {
        
        if (vme->is_loaded) {
            if (pagedir_is_dirty(vme->thread->pagedir, vme->vaddr)) {
                lock_acquire(&filesys_lock);
                file_write_at(vme->file, 
                              vme->kpage, 
                              vme->read_bytes, 
                              vme->offset);
                lock_release(&filesys_lock);
            }
            frame_free(vme->kpage);
            pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        }
        
        
    }
}