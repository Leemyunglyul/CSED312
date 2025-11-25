#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

typedef int mapid_t;

struct lock* get_filesys_lock(void);

void syscall_init (void);
void force_exit (int status);

#endif
