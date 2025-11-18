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
        if (vme->type == VM_FILE && pagedir_is_dirty(vme->thread->pagedir, vme->vaddr)) {
            lock_acquire(&filesys_lock);

            off_t file_len = file_length(vme->file);
            off_t write_offset = vme->offset;
            off_t write_len = vme->read_bytes;
            
            if (write_offset + write_len > file_len) {
                 write_len = file_len - write_offset;
            }
            
            if (write_len > 0) {
                 file_write_at(vme->file, 
                               vme->kpage, 
                               write_len, 
                               write_offset);
                 
                 pagedir_set_dirty(vme->thread->pagedir, vme->vaddr, false);
            }

            lock_release(&filesys_lock);
        }
        
        pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        frame_free(vme->kpage);
    }
    
    if (vme->swap_index != BITMAP_ERROR) {
        swap_free(vme->swap_index);
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

    if (vme->is_loaded || vme->swap_index != BITMAP_ERROR) {
        vm_munmap_page(vme);
    }
    
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

static void
do_munmap_internal (mapid_t mapid)
{
    struct thread *cur = thread_current();
    struct hash *vm = &cur->vm;
    struct file *file_to_close = NULL;

    lock_acquire(&cur->spt_lock);

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

        if (file_to_close == NULL) file_to_close = target_vme->file;

        if (target_vme->is_loaded) {
            if (target_vme->writable && pagedir_is_dirty(cur->pagedir, target_vme->vaddr)) {
                lock_acquire(&filesys_lock);
                off_t file_len = file_length(target_vme->file);
                off_t write_offset = target_vme->offset;
                off_t write_len = target_vme->read_bytes;
                
                if (write_offset + write_len > file_len) {
                    write_len = file_len - write_offset;
                }
                
                if (write_len > 0) {
                    file_write_at(target_vme->file, 
                                target_vme->kpage, 
                                write_len, 
                                write_offset);
                    pagedir_set_dirty(cur->pagedir, target_vme->vaddr, false);
                }
                lock_release(&filesys_lock);
            }
            frame_free(target_vme->kpage);
            pagedir_clear_page(cur->pagedir, target_vme->vaddr);
        }
        
        hash_delete(vm, &target_vme->elem);
        free(target_vme);
    }
    lock_release(&cur->spt_lock);

    if (file_to_close != NULL) {
        lock_acquire(&filesys_lock);
        file_close(file_to_close);
        lock_release(&filesys_lock);
    }
}

void
munmap_process_exit (void)
{
    struct thread *cur = thread_current();
    struct hash *vm = &cur->vm;
    
    while (true) {
        mapid_t target_mapid = -1;
        bool found = false;
        struct hash_iterator i;

        lock_acquire(&cur->spt_lock);
        hash_first (&i, vm);
        while (hash_next (&i)) {
            struct vm_entry *vme = hash_entry (hash_cur (&i), struct vm_entry, elem);
            if (vme->type == VM_FILE) {
                target_mapid = vme->mapid;
                found = true;
                break; 
            }
        }
        lock_release(&cur->spt_lock);

        if (!found){
            break;
        }
        do_munmap_internal(target_mapid);
    }
}