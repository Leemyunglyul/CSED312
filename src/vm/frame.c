#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include <list.h>
#include <stdio.h>
#include "vm/swap.h"
#include "userprog/pagedir.h"
#include "lib/kernel/bitmap.h"
#include "threads/vaddr.h"
#include "filesys/file.h"
#include "userprog/syscall.h"

static struct list frame_table;
static struct lock frame_lock;
static struct list_elem *clock_hand;

static struct vm_entry *select_victim_frame(void);
static bool frame_do_swap_out(struct vm_entry *vme);
static void *frame_evict(enum palloc_flags flags);

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

static void *
frame_evict (enum palloc_flags flags)
{
    struct vm_entry *victim;
    void *kpage;
    
    while (true) {
        victim = select_victim_frame();
        
        if (victim == NULL) {
            return NULL;
        }

        if (frame_do_swap_out(victim)) {
            kpage = palloc_get_page(flags);
            
            if (kpage != NULL) {
                return kpage; 
            }
        } else {
            lock_acquire(&frame_lock);
            list_push_back(&frame_table, &victim->f_elem);
            lock_release(&frame_lock);
        }
    }
}

static bool
frame_do_swap_out (struct vm_entry *vme)
{

    if (vme == NULL || !vme->is_loaded) {
        return true;
    }
    
    bool is_dirty = pagedir_is_dirty(vme->thread->pagedir, vme->vaddr);
    
    if (vme->type == VM_BIN) {
        if (!is_dirty) {
            pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
            palloc_free_page(vme->kpage);
            vme->is_loaded = false;
            vme->kpage = NULL;
            return true;
        }
    }

    if (vme->type == VM_FILE) {
        if (is_dirty) {
            struct lock *filesys_lock = get_filesys_lock();
            lock_acquire(filesys_lock);
            
            off_t file_len = file_length(vme->file);
            off_t write_bytes = vme->read_bytes;
            if (vme->offset + write_bytes > file_len) 
                write_bytes = file_len - vme->offset;
                
            file_write_at(vme->file, vme->kpage, write_bytes, vme->offset);
            
            lock_release(filesys_lock);
        }
        
        pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
        palloc_free_page(vme->kpage);
        vme->is_loaded = false;
        vme->kpage = NULL;
        return true;
    }

    size_t swap_index = swap_out(vme->kpage);
    
    if (swap_index == BITMAP_ERROR) {
        return false;
    }

    vme->swap_index = swap_index;
    vme->type = VM_ANON;
    vme->is_loaded = false;

    pagedir_clear_page(vme->thread->pagedir, vme->vaddr);
    palloc_free_page(vme->kpage);
    vme->kpage = NULL;

    return true;
}