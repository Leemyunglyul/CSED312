#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include <list.h>
#include <stdio.h>
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"

static void *frame_evict (enum palloc_flags flags);
static bool frame_do_swap_out (struct vm_entry *vme);
static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

void
frame_init (void) 
{
    list_init (&frame_table);
    lock_init (&frame_lock);
    clock_hand = NULL;
}

void *
frame_alloc (struct vm_entry *vme, enum palloc_flags flags) 
{
    //ASSERT (flags & PAL_USER);

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

static struct vm_entry *
select_victim_frame (void)
{
    lock_acquire (&frame_lock);
    
    if (list_empty(&frame_table)) {
        lock_release (&frame_lock);
        PANIC("Frame table is empty, cannot evict!");
    }

    struct vm_entry *victim_vme = NULL;
    struct list_elem *e = clock_hand;
    
    int loop_count = 0;
    int max_loops = list_size(&frame_table) * 2 + 1; 

    while (true)
    {
        if (e == NULL || e == list_end(&frame_table)) {
            e = list_begin(&frame_table);
        }
        
        struct vm_entry *vme = list_entry (e, struct vm_entry, f_elem);
        
        struct list_elem *next_e = list_next(e);
        clock_hand = next_e;

        if (vme->pinned || !vme->is_loaded) {
            e = next_e;
            loop_count++;
            if (loop_count > max_loops) {
                 lock_release(&frame_lock);
                 return NULL;
            }
            continue;
        }

        if (pagedir_is_accessed (vme->thread->pagedir, vme->vaddr)) {
            pagedir_set_accessed (vme->thread->pagedir, vme->vaddr, false);
        } else {
            victim_vme = vme;
            list_remove(e);
            break;
        }
        
        e = next_e;
        loop_count++;
        
        if (loop_count > max_loops) {
             victim_vme = vme;
             list_remove(e);
             break;
        }
    }
    
    lock_release (&frame_lock);
    return victim_vme;
}

static void *
frame_evict (enum palloc_flags flags)
{
    struct vm_entry *victim_vme = select_victim_frame ();
    if (victim_vme == NULL) {
        return NULL; 
    }
    
    if (!frame_do_swap_out (victim_vme)) { 

        lock_acquire(&frame_lock);
        list_push_back(&frame_table, &victim_vme->f_elem); 
        lock_release(&frame_lock);
        return NULL; 
    }
    
    void *kpage = palloc_get_page (flags);
    if (kpage == NULL) {
        PANIC ("Eviction succeeded but palloc still fails.");
    }
    return kpage;
}

static bool
frame_do_swap_out (struct vm_entry *vme)
{
    ASSERT (vme->is_loaded == true);
    
    bool is_dirty = pagedir_is_dirty (vme->thread->pagedir, vme->vaddr);
    
    if (vme->type == VM_BIN && !is_dirty) {
        vme->is_loaded = false;
        pagedir_clear_page (vme->thread->pagedir, vme->vaddr);  
        palloc_free_page (vme->kpage);
        vme->kpage = NULL;
        return true;
    }

    if (vme->type == VM_FILE) {
        if (is_dirty) {
            bool lock_held = lock_held_by_current_thread(&filesys_lock);
            
            if (!lock_held) lock_acquire(&filesys_lock);
            
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
                          
            if (!lock_held) lock_release(&filesys_lock);
        }
        
        vme->is_loaded = false;
        pagedir_clear_page (vme->thread->pagedir, vme->vaddr);
        palloc_free_page (vme->kpage);
        vme->kpage = NULL;
        return true; 
    }
    
    size_t swap_index = swap_out (vme->kpage); 
    if (swap_index == BITMAP_ERROR) {
        return false;
    }
    
    vme->is_loaded = false;
    vme->swap_index = swap_index;
    vme->type = VM_ANON; 

    pagedir_clear_page (vme->thread->pagedir, vme->vaddr);
    palloc_free_page (vme->kpage);
    vme->kpage = NULL;
    
    return true;
}

void
frame_free (void *kpage) 
{
    struct list_elem *e;
    struct vm_entry *target_vme = NULL;

    lock_acquire (&frame_lock);
    
    e = list_begin (&frame_table);
    while (e != list_end (&frame_table)) 
    {
        struct vm_entry *vme = list_entry (e, struct vm_entry, f_elem);
        
        if (vme->kpage == kpage) {
            if (clock_hand == e) {
                clock_hand = list_next(e);
                if (clock_hand == list_end(&frame_table)) {
                    clock_hand = list_begin(&frame_table);
                }
            }
            
            list_remove (e);
            target_vme = vme;
            break; 
        }
        e = list_next(e); 
    }
    
    lock_release (&frame_lock);

    if (target_vme != NULL) {
        palloc_free_page (kpage);
        target_vme->kpage = NULL;
    }
}