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

#include "dcc.h"


#define GPFSEL0	0	/* 0x00 */
#define GPSET0	7	/* 0x1C */
#define GPCLR0	10	/* 0x28 */

static volatile unsigned int *gpio;
static unsigned int on = 0;
static pthread_t dccthreadid;

struct dccmessage *messagelist;

/* This message is sent when there is nothing else to do. The NMRA DCC
 * standard says every decoder must treat it as a valid DCC packet that
 * is addressed to some other decoder
 */
static struct dccmessage idlemessage =
{
	.length = 2,
	.data = { 0xff, 0 },
	.next = NULL
};

/* Prevent anything changing the DCC message linked list while dccthread
 * is transmitting its contents
 */
static pthread_mutex_t dccmutex;

/* Debug */
void debug_printlist(void)
{
	struct dccmessage *message;
	unsigned int count;

	message = messagelist;

	printf("DCC message list:\n");

	while(message)
	{
		printf("%p: %u bytes:", message, message->length);

		count = 0;

		while(count < message->length)
		{
			printf(" 0x%0hhx", message->data[count]);
			count++;
		}

		printf("\n");

		message = message->next;
	}

	printf("End\n\n");
}

/* Add or update an instruction
 * Create a new instruction in memory if one not supplied
 */
struct dccmessage *add_update(struct dccmessage *message,
	unsigned int bytes, ...)
{
	unsigned int i;
	unsigned int byte;	/* varargs won't take char arguments */
	va_list ap;
	struct dccmessage **ptr;

	pthread_mutex_lock(&dccmutex);

	if(!message)
	{
		message = malloc(sizeof(struct dccmessage));

		ptr = &messagelist;

		while(*ptr)
			ptr = &((*ptr)->next);

		*ptr = message;

		message->next = NULL;
	}

	va_start(ap, bytes);

	for(i=0; i<bytes; i++)
	{
		byte = va_arg(ap, unsigned int);

		message->data[i] = byte;
	}

	message->length = bytes;

	pthread_mutex_unlock(&dccmutex);

	va_end(ap);

	return message;
}

void delete(struct dccmessage *message)
{
	struct dccmessage **ptr;

	pthread_mutex_lock(&dccmutex);

	ptr = &messagelist;

	while(*ptr)
	{
		if(*ptr == message)
		{
			*ptr = message->next;

			free(message);

			pthread_mutex_unlock(&dccmutex);
			return;
		}

		ptr = &((*ptr)->next);
	}

	pthread_mutex_unlock(&dccmutex);
}

static void dccthread_cleanup(void *unused)
{
	/* Stop everything */
	gpio[GPCLR0] = 1+2+16;

	/* Cancel the DCC mutex, if this thread is holding it (if not,
	 * nothing will happen other than an ignored error)
	 */
	pthread_mutex_unlock(&dccmutex);
}

void quit(int data)
{
	printf("Exiting...\n");

	/* Kill thread */
	pthread_cancel(dccthreadid);

	/* Wait for it to end */
	pthread_join(dccthreadid, NULL);

	exit(0);
}


static void rearm_timer(timer_t timerid, struct itimerspec *time, unsigned int intervalnsec)
{
	time->it_value.tv_nsec += intervalnsec;

	if(time->it_value.tv_nsec >= 1000000000)
	{
		time->it_value.tv_nsec -= 1000000000;
		time->it_value.tv_sec++;
	}

	if(timer_settime(timerid, TIMER_ABSTIME, time, NULL) < 0)
	{
		perror("Could not set timer");
		exit(EX_OSERR);
	}
}

/* Various macros for the function below */
#define OUTPUT_A gpio[GPCLR0] = 1+16; gpio[GPSET0] = 2+16
#define OUTPUT_B gpio[GPCLR0] = 2+16; gpio[GPSET0] = 1+16
#define PULSE0 OUTPUT_A; rearm_timer(timerid, &interval, 116000); sigwaitinfo(&sigwait_mask, NULL); OUTPUT_B; rearm_timer(timerid, &interval, 116000); sigwaitinfo(&sigwait_mask, NULL)
#define PULSE1 OUTPUT_A; rearm_timer(timerid, &interval, 58000); sigwaitinfo(&sigwait_mask, NULL); OUTPUT_B; rearm_timer(timerid, &interval, 58000); sigwaitinfo(&sigwait_mask, NULL)
#define PULSEIDLE OUTPUT_A; rearm_timer(timerid, &interval, 8000000); sigwaitinfo(&sigwait_mask, NULL); OUTPUT_B; rearm_timer(timerid, &interval, 1000000); sigwaitinfo(&sigwait_mask, NULL)

static void *dccthread(void *unused)
{
	struct itimerspec interval;
	timer_t timerid;

	sigset_t sigwait_mask;

	sigset_t block_mask;

	unsigned int count = 0;
	unsigned int i,j;
	unsigned char byte, edd;

	int trylockresponse;

	struct dccmessage *message;

	/* If this thread is cancelled, turn the track power off */
	pthread_cleanup_push(dccthread_cleanup, NULL);

	sigemptyset(&block_mask);
	sigaddset(&block_mask, SIGINT);
	sigaddset(&block_mask, SIGTERM);
	sigaddset(&block_mask, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &block_mask, NULL);

	if(timer_create(CLOCK_MONOTONIC, NULL, &timerid) < 0)
	{
		perror("Could not create timer");
		exit(EX_OSERR);
	}

	clock_gettime(CLOCK_MONOTONIC, &interval.it_value);

	interval.it_interval.tv_sec = 0;
	interval.it_interval.tv_nsec = 0;

	rearm_timer(timerid, &interval, 1160000);

	sigemptyset(&sigwait_mask);
	sigaddset(&sigwait_mask, SIGALRM);

	sigwaitinfo(&sigwait_mask, NULL);

	while(1)
	{
		/* Lock the message list - if it can't be locked at the
		 * moment, keep transmitting zeroes until it can
		 */
		while((trylockresponse = pthread_mutex_trylock(&dccmutex)))
		{
			if(trylockresponse != EBUSY)
			{
				fprintf(stderr, "pthread_mutex_trylock: %s",
					strerror(trylockresponse));
				/* Can a pthread cancel itself? */
				quit(0);
			}

			PULSE0;
		}

		message = messagelist;

		/* If the message list is empty (NULL), idle instead */
		if(!message)
			message = &idlemessage;

		while(message)
		{
			/* Preamble */
			for(i=14; i; i--)
			{
				/* Short pulses */
				PULSE1;
			}

			edd = 0;

			for(i=0; i<=message->length; i++)
			{
				/* Start-of-packet/start-of-byte pulse */
				PULSE0;

				if(i<message->length)
				{
					byte = message->data[i];
					edd ^= byte;
				}
				else
				{
					byte = edd;
				}

				for(j=8; j; j--)
				{
					if(byte & 128)
					{
						PULSE1;
					}
					else
					{
						PULSE0;
					}

					byte = byte << 1;
				}
			}

			/* End of packet */
			PULSE1;

			/* Idle for 2 cycles (58*2*2*2 = 464us).
			 * It is theoretically possible to launch right into
			 * the next packet, even to the exent of treating the
			 * end-of-packet 1 pulse as a preamble bit.
			 * But we'll do this instead.
			 */
			for(count=0; count<2; count++)
			{
				PULSE0;
			}

			message = message->next;
		}

		/* Unlock the message list */
		pthread_mutex_unlock(&dccmutex);

		/* Transmit zero for 16ms - 2 long half-waves to give the
		 * CPU chance to let something else run
		 */
		PULSEIDLE;
	}

	/* This point is never reached, but pthread_cleanup_push needs to
	 * be paired with pthread_cleanup_pop, or it won't compile
	 */ 
	pthread_cleanup_pop(1);
}

void setup_dcc(void)
{
	int fd;
	pthread_attr_t dccthreadattr;
	struct sched_param sp;
	sigset_t mask;
	struct sigaction sa;

	sigset_t block_mask;

	fd = open("/dev/mem", O_RDWR);

	gpio = mmap(NULL, 0xb0, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0x20200000);

	if(gpio == MAP_FAILED)
	{
		perror("Could not map memory");
		exit(1);
	}

	/* GPIO[0:1] and GPIO[4] are outputs */
	gpio[GPFSEL0] = (9+(1<<12)) | (gpio[GPFSEL0] & ~(63+(7<<12)));

	/* Initialise DCC message list mutex */
	pthread_mutex_init(&dccmutex, NULL);

	pthread_attr_init(&dccthreadattr);

	pthread_create(&dccthreadid, &dccthreadattr, dccthread, NULL);

#ifndef NO_SCHEDULER	/* Option to not set the scheduler */
	sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
	pthread_setschedparam(dccthreadid, SCHED_FIFO, &sp);
#endif /* NO_SCHEDULER */

	sigemptyset(&block_mask);
	sigaddset(&block_mask, SIGALRM);
	pthread_sigmask(SIG_BLOCK, &block_mask, NULL);

        sigemptyset(&mask);
        sa.sa_handler = quit;
        sa.sa_flags = 0;
        sa.sa_mask = mask;

	if(sigaction(SIGTERM, &sa, NULL) < 0)
	{
		perror("Creating signal handler");
		exit(EX_OSERR);
	}
	if(sigaction(SIGINT, &sa, NULL) < 0)
	{
		perror("Creating signal handler");
		exit(EX_OSERR);
	}
}
