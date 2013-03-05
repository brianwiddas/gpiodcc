/* SRCP for the Raspberry Pi
 *
 * THIS IS NOT A PROPER IMPLEMENTATION OF SRCP
 *
 * It is missing quite of a few things, and, more imporantly, skips out
 * some important bits in order to work properly with JMRI. JMRI appears
 * to have implemented its own protocol which is almost, but not quite,
 * entirely unlike SRCP
 *
 * SRCP purists, look away now...
 */

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>

/* Socket stuff */
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* getnameinfo() */
#include <netdb.h>

/* inet_ntop */
#include <arpa/inet.h>

/* strlen */
#include <string.h>

/* gettimeofday */
#include <sys/time.h>

/* Threads */
#include <pthread.h>

/* Time */
#include <time.h>

/* Signal handling */
#include <signal.h>

/* EX_OSERR */
//#include <sysexits.h>

/* errno variable & error numbers */
#include <errno.h>

/* Scheduler */
//#include <sched.h>

#include "srcp.h"
#include "dcc.h"


/* Structure for passing data to newly-created threads */
struct newthreaddata
{
	int socket;
};

/* Linked list of all the trains we know about */
static struct genericloco *trainlist = NULL;

void debug_printtrainlist(void)
{
	struct genericloco *list = trainlist;

	printf("Train list:\n");
	while(list)
	{
		printf("Address %u (p.v. %u)\n  SS %u   funcs %u = 0x%x\n  DM %u (%s)   V %d/%d\n", 
			list->address, list->protocolversion, list->speedsteps,
			list->numfuncs, list->funcs, list->drivemode,
			(list->drivemode?(list->drivemode==1?"F":"R"):"E"),
			list->v, list->vmax);

		list = list->next;
	}

	printf("List ends\n");
}

/* Set up the listening socket for the server */
int setup_network(void)
{
	int listensock, optval;
	struct sockaddr_storage bind_storage;
	struct sockaddr_in *bind4;
	struct sockaddr_in6 *bind6;
	struct sockaddr *bindaddr;

	bindaddr = (struct sockaddr *) &bind_storage;

	bind6 = (struct sockaddr_in6 *) bindaddr;
	bind6->sin6_family = AF_INET6;
	bind6->sin6_port = htons(LISTENPORT);
	bind6->sin6_addr = in6addr_any;
	bind6->sin6_flowinfo = 0;

	listensock = socket(AF_INET6, SOCK_STREAM, 0);
	if(listensock<0)
	{
		if(errno == EAFNOSUPPORT)
		{
			/* No support for IPv6. Boo */
			bind4 = (struct sockaddr_in *) bindaddr;
			bind4->sin_family = AF_INET;
			bind4->sin_port = htons(LISTENPORT);
			bind4->sin_addr.s_addr = INADDR_ANY;

			listensock = socket(AF_INET, SOCK_STREAM, 0);
			if(listensock<0)
			{
				perror("(IPv4) socket");
				exit(1);
			}
		}
		else
		{
			perror("(IPv6) socket");
			exit(1);
		}
	}

	optval = 1;
	if(setsockopt(listensock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0)
	{
		perror("setsockopt");
		exit(1);
	}

	if(bind(listensock, bindaddr, sizeof(bind_storage)) < 0)
	{
		perror("bind");
		exit(1);
	}

	if(listen(listensock, 10) < 0)
	{
		perror("listen");
		exit(1);
	}

	/* DEBUG */
	printf("Listening on port %u\n", LISTENPORT);

	return listensock;
}

/* Send a string to a socket, adding the required timestamp */
static void writeto(unsigned int fd, char *string)
{
	unsigned int writelen;
/* SRCP standard says a timestamp should preceed every message, but JMRI
 * doesn't work if it does.
 */
#ifdef INCLUDETIME
	struct timeval time;
	char buffer[4];
	char longbuf[2048];

	gettimeofday(&time, NULL);

	snprintf(buffer, 4, "%06i", (int)time.tv_usec);
	snprintf(longbuf, 2048, "%lu.%s %s",
		time.tv_sec, buffer, string);

	string = longbuf;
#endif

	writelen = write(fd, string, strlen(string));
	if(writelen < 0)
	{
		perror("write");
		exit(1);
	}
	printf("Sent: %s", string);
}

static size_t readfrom(int sock, char *buffer, size_t buffersize)
{
	size_t readlen;
	char *lineend;

	readlen = read(sock, buffer, buffersize - 1);
	if(readlen < 0)
	{
		perror("read");
		exit(1);
	}

	/* Null terminate the string in the buffer */
	buffer[readlen] = 0;

	/* Remove CR/LF from the end of the line */
	lineend = strpbrk(buffer, "\r\n");
	if(lineend)
	{
		*lineend = 0;
	}

	printf("Received: %s\n", buffer);

	return readlen;
}

/* Returns string number if one of the strings matches, or -1 otherwise
 *
 * eg tokenmatch("LIST", saveptr, "TEST", "LIST", "OF", "THINGS") = 1
 */
#define tokenmatch(...)	__tokenmatch(__VA_ARGS__, NULL)
static int __tokenmatch(char *buffer, char **saveptr, ...)
{
	char *token, *matchstr;
	unsigned int stringnumber;
	va_list ap;

	token = strtok_r(buffer, " \r\n", saveptr);

	if(!token)
		return -1;

	stringnumber = 0;

	va_start(ap, saveptr);
	
	while((matchstr = va_arg(ap, char *)))
	{
		/* strcmp returns 0 if the strings match */
		if(!strcmp(token, matchstr))
			return stringnumber;

		stringnumber++;
	}

	return -1;
}

/* Returns next token as an integer, ignore failure */
static int tokeninteger(char *buffer, char **saveptr)
{
	char *token;

	token = strtok_r(buffer, " \r\n", saveptr);

	if(token)
		return atoi(token);
	else
		return -1;
}

static void badcommand(int sock, char **unused)
{
	writeto(sock, "410 ERROR unknown command\r\n");
}

static struct genericloco *findloco(unsigned int address)
{
	struct genericloco *loco;

	loco = trainlist;

	while(loco)
	{
		if(loco->address == address)
			break;

		loco = loco->next;
	}

	return loco;
}

static struct genericloco *newloco(void)
{
	struct genericloco **loco;

	loco = &trainlist;

	while(*loco)
	{
		loco = &((*loco)->next);
	}

	*loco = malloc(sizeof(struct genericloco));

	printf("New loco\n\n");
	return *loco;
}

static void removeloco(unsigned int address)
{
	struct genericloco **loco;
	struct genericloco *ptr;

	loco = &trainlist;

	while(*loco)
	{
		if((*loco)->address == address)
		{
			ptr = *loco;

			*loco = (*loco)->next;

			free(ptr);

			printf("Deleted loco\n\n");
			return;
		}
		
		loco = &((*loco)->next);
	}
}

static void locoinfo(int sock, struct genericloco *loco)
{
	const unsigned int bus = 1;
	char buffer[1024];
	char *ptr;
	int charcount;
	unsigned int i;

	charcount = sprintf(buffer, "100 INFO %u GL %u %u %d %d", bus,
		loco->address, loco->drivemode, loco->v, loco->vmax);

	ptr = &buffer[charcount];

	for(i = 0; i < loco->numfuncs; i++)
	{
		charcount = sprintf(ptr, " %u", (loco->funcs&(1<<i))?1:0);

		ptr = &ptr[charcount];
	}

	charcount = sprintf(ptr, "\r\n");

	writeto(sock, buffer);
}

static void initloco(int sock, char **saveptr)
{
	int address, protocol, speedsteps, numfuncs;
	struct genericloco *newtrain;
	const unsigned int bus = 1;

	address = tokeninteger(NULL, saveptr);

	if(tokenmatch(NULL, saveptr, "N"))
	{
		writeto(sock, "412 ERROR wrong value\r\n");
		return;
	}

	protocol = tokeninteger(NULL, saveptr);

	if(protocol != 1 && protocol != 2)
	{
		writeto(sock, "412 ERROR wrong value\r\n");
	}

	speedsteps = tokeninteger(NULL, saveptr);

	if(speedsteps != 14 && speedsteps != 28 && speedsteps != 128)
	{
		writeto(sock, "412 ERROR wrong value. "
			"Only speedsteps of 14/28/128 supported\r\n");
		return;
	}

	numfuncs = tokeninteger(NULL, saveptr);

	if(numfuncs < 0 || numfuncs > 32)
	{
		writeto(sock, "412 ERROR wrong value. "
			"0-32 functions only\r\n");
		return;
	}

	newtrain = newloco();

	newtrain->address = address;
	newtrain->protocolversion = protocol;
	newtrain->speedsteps = speedsteps;
	newtrain->numfuncs = numfuncs;
	newtrain->funcs = 0;
	newtrain->drivemode = 1 ;//2;
	newtrain->v = 0;
	newtrain->vmax = 100;
	newtrain->dccspeed = NULL;
	newtrain->dccfunc0to4 = NULL;
	newtrain->next = NULL;

	writeto(sock, "200 OK\r\n");
}

static void setloco(int sock, char **saveptr)
{
	int address, drivemode, v, vmax, funcs;
	int value;
	struct genericloco *train;
	const unsigned int bus = 1;
	unsigned int i;

	unsigned int calc;

	address = tokeninteger(NULL, saveptr);

	train = findloco(address);

	if(!train)
	{
		writeto(sock, "412 ERROR wrong value\r\n");
		return;
	}
		
	drivemode = tokeninteger(NULL, saveptr);

	if(drivemode < 0 || drivemode > 2)
	{
		writeto(sock, "412 ERROR wrong value\r\n");
		return;
	}

	v = tokeninteger(NULL, saveptr);
	vmax = tokeninteger(NULL, saveptr);

	if(vmax < 1 || v > vmax)
	{
		/* v can be less than 0; JMRI uses it for emergency stop */
		writeto(sock, "412 ERROR wrong value.\r\n");
		return;
	}

	funcs = 0;

	for(i=0; i < train->numfuncs; i++)
	{
		value = tokeninteger(NULL, saveptr);

		funcs |= (value?1:0)<<i;
	}

	train->funcs = funcs;
	train->drivemode = drivemode;
	train->v = v;
	train->vmax = vmax;

	/* TODO: 14/28 speed steps, long addresses */
	calc = (126 * v)/vmax;
	if(calc>0)
		calc++;	/* 1 is emergency stop, 2-127 are speed steps */

	if(v<0 || drivemode==2)
		calc=1;	/* Emergency stop */

	train->dccspeed =
		add_update(train->dccspeed, 3, address, 0x3f,
			calc | (drivemode?128:0));

	/* 5-12 not currently supported. Not sure I have a suitable
	 * decoder for testing
	 */
	train->dccfunc0to4 =
		add_update(train->dccfunc0to4, 2, address,
			0x80 | ((funcs & 1)<<4) | ((funcs & 0x1e)>>1));

	writeto(sock, "200 OK\r\n");
}

static void getloco(int sock, char **saveptr)
{
	int address;
	struct genericloco *train;
	const unsigned int bus = 1;

	address = tokeninteger(NULL, saveptr);

	train = findloco(address);

	if(!train)
	{
		writeto(sock, "416 ERROR no data\r\n");
		return;
	}

	locoinfo(sock, train);
}

static void termloco(int sock, char **saveptr)
{
	int address;
	struct genericloco *train;
	const unsigned int bus = 1;

	address = tokeninteger(NULL, saveptr);

	train = findloco(address);

	if(!train)
	{
		writeto(sock, "412 ERROR wrong value\r\n");
		return;
	}

	removeloco(address);

	writeto(sock, "200 OK\r\n");
}


static void initpower(int sock, char **saveptr)
{
	const unsigned int bus = 1;

	/* Do something here? */

	writeto(sock, "200 OK\r\n");
}

static void termpower(int sock, char **saveptr)
{
	const unsigned int bus = 1;

	/* And here */

	writeto(sock, "200 OK\r\n");
}

static void setpower(int sock, char **saveptr)
{
	const unsigned int bus = 1;
	int on;

	on = tokenmatch(NULL, saveptr, "OFF", "ON");

	if(on < 0)
	{
		writeto(sock, "412 ERROR wrong value\r\n");
		return;
	}

	/* Do something with the power setting */

	writeto(sock, "200 OK\r\n");
}

static void getpower(int sock, char **saveptr)
{
	const unsigned int bus = 1;
	char buffer[512];

	int on = 1;

	/* Here too */

	sprintf(buffer, "100 INFO %u POWER %s\r\n", bus, (on?"ON":"OFF"));

	writeto(sock, buffer);
}



#define COMMANDS "GET", "SET", "CHECK", "WAIT", "INIT", "TERM", "RESET", "VERIFY"
#define FACILITIES "GL", "POWER"

static void (*srcpfunction[8][2])(int, char **) =
{
	{ getloco, getpower },
	{ setloco, setpower },
	{ badcommand, badcommand },
	{ badcommand, badcommand },
	{ initloco, initpower },
	{ termloco, termpower },
	{ badcommand, badcommand },
	{ badcommand, badcommand }
};

static void *communicationthread(struct newthreaddata *ntd)
{
	int sock;
	ssize_t readlen;
	char *saveptr;
	char buffer[1024];
	enum { NONE = -1, INFO = 0, COMMAND = 1 } connectionmode;
	int command, bus, facility;
	char *send;

	sock = ntd->socket;

	/* Finished with this now */
	free(ntd);

	/* The welcome message doesn't have a timestamp */
	send = "SRCP 0.8.3\r\n";
	if(write(sock, send, strlen(send)) < 0)
	{
		perror("write");
		exit(1);
	}

	while(1)
	{
		readlen = readfrom(sock, buffer, sizeof(buffer));

		/* Handshake */
		if(tokenmatch(buffer, &saveptr, "SET") ||
			tokenmatch(NULL, &saveptr, "PROTOCOL") ||
			tokenmatch(NULL, &saveptr, "SRCP"))
		{
			writeto(sock, "410 ERROR unknown command\r\n");

			continue;
		}

		/* 0.8.3 = JMRI
		 * 0.82 = Android SRCP client
		 */
		if(tokenmatch(NULL, &saveptr, "0.8.3", "0.82") >= 0)
			break;

		writeto(sock, "400 ERROR unsupported protocol\r\n");
	}

	writeto(sock, "201 OK PROTOCOL SRCP\r\n");

	connectionmode = NONE;

	while(connectionmode == NONE)
	{
		readlen = readfrom(sock, buffer, sizeof(buffer));

		if(tokenmatch(buffer, &saveptr, "SET") ||
			tokenmatch(NULL, &saveptr, "CONNECTIONMODE") ||
			tokenmatch(NULL, &saveptr, "SRCP"))
		{
			writeto(sock, "410 ERROR unknown command\r\n");

			continue;
		}

		connectionmode = tokenmatch(NULL, &saveptr, "INFO", "COMMAND");

		switch(connectionmode)
		{
			case INFO:
				writeto(sock, "401 ERROR unsupported connection mode\r\n");
				break;
			case COMMAND:
				writeto(sock, "202 OK CONNECTIONMODEOK\r\n");
				break;
			default:
				writeto(sock, "401 ERROR unsupported connection mode\r\n");
		}
	}

	while(1)
	{
		readlen = readfrom(sock, buffer, sizeof(buffer));

		if(!tokenmatch(buffer, &saveptr, "GO"))
			break;

		writeto(sock, "410 ERROR unknown command\r\n");
	}

	sprintf(buffer, "200 OK %u\r\n", (unsigned int)pthread_self());
	writeto(sock, buffer);

	/* Handshake completed */

	while ((readlen = readfrom(sock, buffer, sizeof(buffer))) > 0)
	{
		command = tokenmatch(buffer, &saveptr, COMMANDS);
		
		if(command<0)
		{
			writeto(sock, "410 ERROR unknown command\r\n");

			continue;
		}

		bus = tokeninteger(NULL, &saveptr);

		facility = tokenmatch(NULL, &saveptr, FACILITIES);

		if(facility<0)
		{
			writeto(sock, "410 ERROR unknown command\r\n");

			continue;
		}

		srcpfunction[command][facility](sock, &saveptr);
	}

	return NULL;
}

int main(void)
{
	int listensock, acceptsock, result;
	struct sockaddr_storage remoteaddr;
	char buffer[1024];
	socklen_t addrlen;
	pthread_t threadid;
	struct newthreaddata *ntd;

	setup_dcc();

	listensock = setup_network();

	addrlen = sizeof(remoteaddr);

	while(1)
	{
		acceptsock = accept(listensock, (struct sockaddr *)&remoteaddr,
			&addrlen);

		if(acceptsock < 0)
		{
			perror("accept");
			exit(1);
		}
	
		result = getnameinfo((struct sockaddr *)&remoteaddr,
			sizeof(remoteaddr), buffer, sizeof(buffer),
			NULL, 0, NI_NUMERICHOST);

		if(result < 0)
		{
			fprintf(stderr, "getnameinfo: %s\n",
				gai_strerror(result));
			exit(1);
		}

		printf("Connection from: %s\n", buffer);

		ntd = malloc(sizeof(struct newthreaddata));

		if(!ntd)
		{
			perror("malloc");
			exit(1);
		}

		ntd->socket = acceptsock;
	
		if(pthread_create(&threadid, NULL, (void * (*)(void *)) communicationthread, ntd))
		{
			/* Failed to create thread */
			perror("pthread_create");
			exit(1);
		}
	}
}
