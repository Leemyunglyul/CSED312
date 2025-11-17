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

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
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

/* A thread function that loads a user process and starts it
   running. */
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

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
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


/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

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
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

    vm_destroy (&cur->vm);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
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

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
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

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  file_deny_write(file);
  t->executable_file = file;


  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
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

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (!success && file != NULL) {
    file_close(file);
    t->executable_file = NULL;
  }
  return success;
}

/* load() helpers. */

bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  // upage는 이제 phdr.p_vaddr (예: 0x080480a0)
  // ofs는 phdr.p_offset (예: 0x0a0)
  // read_bytes는 phdr.p_filesz (예: 0x50)

  ASSERT ((read_bytes + zero_bytes) >= 0);
  ASSERT (pg_ofs (upage) == (ofs % PGSIZE)); // vaddr과 ofs의 페이지 내 오프셋은 같음

  off_t current_ofs = ofs;
  uint8_t *current_upage = upage;
  uint32_t total_bytes_to_process = read_bytes + zero_bytes; // 처리할 총 바이트

  while (total_bytes_to_process > 0) 
  {
      // 1. 현재 페이지 시작 오프셋과 남은 공간 계산
      size_t page_offset = pg_ofs(current_upage); // e.g., 0xa0 (첫 루프)
      size_t page_remaining = PGSIZE - page_offset; // e.g., 0xF60 (첫 루프)

      // 2. 이 페이지에 채울 파일 바이트 수 계산
      size_t page_read_bytes = (read_bytes < page_remaining) ? read_bytes : page_remaining;

      // 3. 이 페이지에 채울 0 바이트 수 계산
      size_t page_zero_bytes = 0;
      if (page_read_bytes < page_remaining) {
          page_zero_bytes = (zero_bytes < (page_remaining - page_read_bytes))
                          ? zero_bytes : (page_remaining - page_read_bytes);
      }

      // 4. SPT에서 이 페이지(정렬된 주소)를 찾음
      uint8_t *page_vaddr = pg_round_down(current_upage); // e.g., 0x08048000
      struct vm_entry *vme = vm_find(&thread_current()->vm, page_vaddr);

      if (vme == NULL) {
          vme = malloc(sizeof(struct vm_entry));
          if (vme == NULL) return false;

          vme->type = VM_BIN;
          vme->vaddr = page_vaddr;
          vme->writable = writable;
          vme->is_loaded = false;
          vme->file = file;

          vme->offset = current_ofs;
          vme->read_bytes = page_read_bytes;
          vme->zero_bytes = page_zero_bytes;

          if (!vm_insert(&thread_current()->vm, vme)) {
              free(vme);
              return false;
          }
      } else {
          // 중복 처리: .text와 .data가 겹치는 경우
          if (writable && !vme->writable) {
              vme->writable = true;
          }
      }

      // 5. 이 페이지에서 처리한 총 바이트 수
      size_t bytes_processed_this_page = page_read_bytes + page_zero_bytes;

      // 6. 남은 바이트 수 업데이트
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      total_bytes_to_process -= bytes_processed_this_page;

      // 7. 다음 변수 업데이트
      current_ofs += page_read_bytes;
      current_upage += bytes_processed_this_page;
  }

  return true;

}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
    bool success = false;
    void *stack_upage = ((uint8_t *) PHYS_BASE) - PGSIZE;
    struct thread *cur = thread_current();
    
    /* [수정] vme를 먼저 찾거나 생성 (malloc 1번) */
    struct vm_entry *vme = vm_find (&cur->vm, stack_upage);
    if (vme == NULL) {
        vme = malloc(sizeof(struct vm_entry));
        if (vme == NULL) return false; // malloc 실패

        vme->type = VM_ANON;
        vme->vaddr = stack_upage;
        vme->writable = true;
        vme->is_loaded = true; // (이제 로드할 것임)
        vme->file = NULL;
        
        if (!vm_insert (&cur->vm, vme)) {
            free(vme);
            return false;
        }
    } else {
        vme->is_loaded = true;
        vme->type = VM_ANON; 
        vme->writable = true;
    }

    /* [수정] frame_alloc에 vme 전달 */
    uint8_t *kpage = frame_alloc (vme, PAL_USER | PAL_ZERO);
    if (kpage == NULL) {
        /* * 롤백: vme는 SPT에 남겨두고 'not_loaded'로 되돌림
         * * (혹은 vm_delete 호출) 
         */
        vme->is_loaded = false; 
        return false;
    }
    
    success = install_page (stack_upage, kpage, true);
    if (!success)
    {
        frame_free (kpage);
        vme->is_loaded = false;
        return false;
    }
    
    *esp = PHYS_BASE;
    return true; /* 성공 */
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
