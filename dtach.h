/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2001, 2004-2016 Ned T. Crigler

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#ifndef dtach_h
#define dtach_h

#if defined(__has_attribute)
#if __has_attribute(unused)
#define ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define ATTRIBUTE_UNUSED
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define ATTRIBUTE_UNUSED __attribute__((__unused__))
#else
#define ATTRIBUTE_UNUSED
#endif

#include <config.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if TIME_WITH_SYS_TIME
#include <sys/time.h>
#include <time.h>
#else
#if HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <time.h>
#endif
#endif

#ifdef HAVE_PTY_H
#include <pty.h>
#endif

#ifdef HAVE_UTIL_H
#include <util.h>
#endif

#ifdef HAVE_LIBUTIL_H
#include <libutil.h>
#endif

#ifdef HAVE_STROPTS_H
#include <stropts.h>
#endif

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#include <termios.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif

#ifndef S_ISSOCK
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)
#endif

extern char *progname, *sockname;
extern int detach_char, no_suspend, redraw_method;
extern struct termios orig_term;
extern int dont_have_tty;

enum
{
	MSG_PUSH	= 0,
	MSG_ATTACH	= 1,
	MSG_DETACH	= 2,
	MSG_WINCH	= 3,
	MSG_REDRAW	= 4,
};

enum
{
	REDRAW_UNSPEC	= 0,
	REDRAW_NONE	= 1,
	REDRAW_CTRL_L	= 2,
	REDRAW_WINCH	= 3,
};

/* The client to master protocol. */
struct packet
{
	unsigned char type;
	unsigned char len;
	union
	{
		unsigned char buf[sizeof(struct winsize)];
		struct winsize ws;
	} u;
};

/*
** The master sends a simple stream of text to the attaching clients, without
** any protocol. This might change back to the packet based protocol in the
** future. In the meantime, however, we minimize the amount of data sent back
** and forth between the client and the master. BUFSIZE is the size of the
** buffer used for the text stream.
*/
#define BUFSIZE 4096

/* This hopefully moves to the bottom of the screen */
#define EOS "\033[999H"

void write_buf_or_fail(int fd, const void *buf, size_t count);
void write_packet_or_fail(int fd, const struct packet *pkt);

int attach_main(int noerror);
int master_main(char **argv, int waitattach, int dontfork);
int push_main(void);

#ifdef sun
#define BROKEN_MASTER
#endif
#endif
