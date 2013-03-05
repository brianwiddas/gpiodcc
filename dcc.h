
#ifndef DCC_H
#define DCC_H

#if 0
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

/* strerror */
#include <string.h>

/* Threads */
#include <pthread.h>

/* mmap() */
#include <sys/mman.h>

/* open() */
#include <sys/stat.h>
#include <fcntl.h>

/* Timers */
#include <signal.h>
#include <time.h>

/* EX_OSERR */
#include <sysexits.h>

/* errno & EBUSY */
#include <errno.h>

/* Scheduler */
#include <sched.h>

#endif

// pthread_t dccthreadid;

struct dccmessage
{
	unsigned int length;
	unsigned char data[10];	/* How long is the longest packet? */
	struct dccmessage *next;	/* Linked list */
};

// struct dccmessage *messagelist;

//pthread_mutex_t dccmutex;

/* Debug */
extern void debug_printlist(void);

/* Add or update an instruction
 * Create a new instruction in memory if one not supplied
 */
extern struct dccmessage *add_update(struct dccmessage *message,
	unsigned int bytes, ...);

extern void delete(struct dccmessage *message);

extern void quit(int data);

extern void setup_dcc(void);

#endif /* DCC_H */
