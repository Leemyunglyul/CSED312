#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"
#include "vm/frame.h"
#include "vm/page.h"
#include "lib/kernel/bitmap.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

static struct thread *find_child_process(tid_t child_tid) {
    struct thread *cur = thread_current();
    struct list_elem *e;
    struct thread *child_t = NULL;

    for (e = list_begin(&cur->child_list); e != list_end(&cur->child_list); e = list_next(e)) {
        child_t = list_entry(e, struct thread, child_elem);
        if (child_t->tid == child_tid) {
            return child_t;
        }
    }
    return NULL;
}

tid_t
process_execute (const char *file_name) 
{
  char *fn_copy;
  tid_t tid;

  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, file_name, PGSIZE);

  char program_name[16];
  char *save_ptr;
  strlcpy(program_name, file_name, sizeof(program_name));
  strtok_r(program_name, " ", &save_ptr);

  tid = thread_create (program_name, PRI_DEFAULT, start_process, fn_copy);

  if (tid == TID_ERROR) {
    palloc_free_page (fn_copy);
    return tid;
  }

  struct thread *child = get_thread(tid);
  if (!child) {
    return TID_ERROR;
  }
  
  list_push_back(&thread_current()->child_list, &child->child_elem);
  
  sema_down(&child->load_sema);

  if (!child->load_success) {
    return TID_ERROR;
  }

  return tid;
}

static void
start_process (void *file_name_)
{
  char *file_name = file_name_;
  struct intr_frame if_;
  bool success;
  
  char *argv[128];
  int argc = 0;
  char *token, *save_ptr;
  for (token = strtok_r(file_name, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr))
  {
      argv[argc++] = token;
  }

  struct thread *cur = thread_current();
  vm_init (&cur->vm);
  lock_init (&cur->spt_lock); 

  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (argv[0], &if_.eip, &if_.esp);
  
  cur->load_success = success;
  cur->parent = get_thread(cur->parent_tid);

  sema_up(&cur->load_sema);
  
  if (success) {
    char *arg_addrs[argc];
    int i;

    for (i = argc - 1; i >= 0; i--) {
        int len = strlen(argv[i]) + 1;
        if_.esp -= len;
        memcpy(if_.esp, argv[i], len);
        arg_addrs[i] = if_.esp;
    }
    while ((int)if_.esp % 4 != 0) {
        if_.esp--;
        *(uint8_t *)if_.esp = 0;
    }
    if_.esp -= 4;
    *(char **)if_.esp = NULL;
    for (i = argc - 1; i >= 0; i--) {
        if_.esp -= 4;
        *(char **)if_.esp = arg_addrs[i];
    }
    if_.esp -= 4;
    *(char ***)if_.esp = if_.esp + 4;
    if_.esp -= 4;
    *(int *)if_.esp = argc;
    if_.esp -= 4;
    *(int *)if_.esp = 0;
  }
  
  palloc_free_page (file_name_);
  
  if (!success) {
    force_exit(-1);
  }

  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

int
process_wait (tid_t child_tid) 
{
  struct thread *child = find_child_process(child_tid);
  if (!child) {
      return -1; 
  }
  
  sema_down(&child->wait_sema);

  int status = child->exit_status;
  list_remove(&child->child_elem);
  
  sema_up(&child->free_sema);

  return status;
}

void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  struct lock *fs_lock = get_filesys_lock();
  
  vm_destroy (&cur->vm);

  lock_acquire(fs_lock);

  if (cur->executable_file != NULL) {
      file_close(cur->executable_file);
      cur->executable_file = NULL; 
  }

  for (int i = 2; i < FDT_SIZE; i++) {
      if (cur->fd_table[i] != NULL) {
          file_close(cur->fd_table[i]);
          cur->fd_table[i] = NULL;
      }
  }

  lock_release(fs_lock);

  sema_up(&cur->wait_sema);
  sema_down(&cur->free_sema);


  struct list_elem *e;
  while (!list_empty(&cur->child_list))
  {
      e = list_pop_front(&cur->child_list);
      struct thread *child = list_entry (e, struct thread, child_elem);
      if (child) {
          sema_up(&child->free_sema);
      }
  }

  pd = cur->pagedir;
  if (pd != NULL) 
    {
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

void
process_activate (void)
{
  struct thread *t = thread_current ();
  pagedir_activate (t->pagedir);
  tss_update ();
}

typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;
#define PE32Wx PRIx32 
#define PE32Ax PRIx32 
#define PE32Ox PRIx32 
#define PE32Hx PRIx16 

struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

#define PT_NULL    0            
#define PT_LOAD    1            
#define PT_DYNAMIC 2            
#define PT_INTERP  3            
#define PT_NOTE    4            
#define PT_SHLIB   5            
#define PT_PHDR    6            
#define PT_STACK   0x6474e551   

#define PF_X 1          
#define PF_W 2          
#define PF_R 4          

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  struct lock *fs_lock = get_filesys_lock();

  lock_acquire(fs_lock);
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      lock_release(fs_lock);
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  lock_release(fs_lock);

  lock_acquire(fs_lock);
  file_deny_write(file);
  lock_release(fs_lock);
  t->executable_file = file;

  lock_acquire(fs_lock);
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      lock_release(fs_lock);
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }
  lock_release(fs_lock);

  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      lock_acquire(fs_lock);
      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr) {
        lock_release(fs_lock);
        goto done;
      }
      lock_release(fs_lock);
      
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t read_bytes = phdr.p_filesz;
              uint32_t zero_bytes = phdr.p_memsz - phdr.p_filesz;
              if (!load_segment (file, phdr.p_offset, (void *) phdr.p_vaddr,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  if (!setup_stack (esp))
    goto done;

  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  return success;
}

bool install_page (void *upage, void *kpage, bool writable);

static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  if (phdr->p_memsz == 0)
    return false;
  
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  if (phdr->p_vaddr < PGSIZE)
    return false;

  return true;
}

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

bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}