#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include "threads/synch.h"

typedef int mapid_t;

extern struct lock filesys_lock;

void syscall_init (void);
void force_exit (int status);

#endif
