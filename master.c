/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2004 Ned T. Crigler

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/
#include "dtach.h"

/* The pty struct - The pty information is stored here. */
struct pty
{
	/* File descriptor of the pty */
	int fd;
#ifdef BROKEN_MASTER
	/* File descriptor of the slave side of the pty. For broken systems. */
	int slave;
#endif
	/* Process id of the child. */
	pid_t pid;
	/* The terminal parameters of the pty. Old and new for comparision
	** purposes. */
	struct termios term;
	/* The current window size of the pty. */
	struct winsize ws;
};

/* A connected client */
struct client
{
	/* The next client in the linked list. */
	struct client *next;
	/* The previous client in the linked list. */
	struct client **pprev;
	/* File descriptor of the client. */
	int fd;
	/* Whether or not the client is attached. */
	int attached;
};

/* The list of connected clients. */
static struct client *clients;
/* The pseudo-terminal created for the child process. */
static struct pty the_pty;

#ifndef HAVE_FORKPTY
pid_t forkpty(int *amaster, char *name, struct termios *termp,
	struct winsize *winp);
#endif

/* Unlink the socket */
static void
unlink_socket(void)
{
	unlink(sockname);
}

/* Signal */
static RETSIGTYPE 
die(int sig)
{
	/* Well, the child died. */
	if (sig == SIGCHLD)
	{
#ifdef BROKEN_MASTER
		/* Damn you Solaris! */
		close(the_pty.fd);
#endif
		return;
	}
	exit(1);
}

/* Initialize the pty structure. */
static int
init_pty(char **argv)
{
	/* Use the original terminal's settings. We don't have to set the
	** window size here, because the attacher will send it in a packet. */
	the_pty.term = orig_term;
	memset(&the_pty.ws, 0, sizeof(struct winsize));

	/* Create the pty process */
	the_pty.pid = forkpty(&the_pty.fd, NULL, &the_pty.term, NULL);
	if (the_pty.pid < 0)
		return -1;
	else if (the_pty.pid == 0)
	{
		/* Child.. Execute the program. */
		execvp(*argv, argv);
		exit(127);
	}
	/* Parent.. Finish up and return */
#ifdef BROKEN_MASTER
	{
		char *buf;

		buf = ptsname(the_pty.fd);
		the_pty.slave = open(buf, O_RDWR|O_NOCTTY);
	}
#endif
	return 0;
}

/* Creates a new unix domain socket. */
static int
create_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	strcpy(sockun.sun_path, name);
	if (bind(s, (struct sockaddr*)&sockun, sizeof(sockun)) < 0)
	{
		close(s);
		return -1;
	}
	if (listen(s, 128) < 0)
	{
		close(s);
		return -1;
	}
	/* chmod it to prevent any suprises */
	if (chmod(name, 0600) < 0)
	{
		close(s);
		return -1;
	}
	return s;
}

/* Sets a file descriptor to non-blocking mode. */
static int
setnonblocking(int fd)
{
	int flags;

#if defined(O_NONBLOCK)
	flags = fcntl(fd, F_GETFL);
	if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
		return -1;
	return 0;
#elif defined(FIONBIO)
	flags = 1;
	if (ioctl(fd, FIONBIO, &flags) < 0)
		return -1;
	return 0;
#else
#warning Do not know how to set non-blocking mode.
	return 0;
#endif
}

/* Process activity on the pty - Input and terminal changes are sent out to
** the attached clients. If the pty goes away, we die. */
static void
pty_activity()
{
	unsigned char buf[BUFSIZE];
	struct client *p;
	int len;

	/* Read the pty activity */
	len = read(the_pty.fd, buf, sizeof(buf));

	/* Error -> die */
	if (len <= 0)
		exit(1);

#ifdef BROKEN_MASTER
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.slave, &the_pty.term) < 0)
		exit(1);
#else
	/* Get the current terminal settings. */
	if (tcgetattr(the_pty.fd, &the_pty.term) < 0)
		exit(1);
#endif

	/* Send it out to the clients. */
	for (p = clients; p; p = p->next)
	{
		if (p->attached)
			write(p->fd, buf, len);
	}
}

/* Process activity on the control socket */
static void
control_activity(int s)
{
	int fd;
	struct client *p;
 
	/* Accept the new client and link it in. */
	fd = accept(s, NULL, NULL);
	if (fd < 0 || setnonblocking(fd) < 0)
		return;

	/* Link it in. */
	p = malloc(sizeof(struct client));
	p->fd = fd;
	p->attached = 0;
	p->pprev = &clients;
	p->next = *(p->pprev);
	if (p->next)
		p->next->pprev = &p->next;
	*(p->pprev) = p;
}

/* Send a signal to the slave side of a pseudo-terminal. */
static void
killpty(struct pty *pty, int sig)
{
	pid_t pgrp = -1;

#ifdef TIOCSIGNAL
	if (ioctl(pty->fd, TIOCSIGNAL, sig) >= 0)
		return;
#endif
#ifdef TIOCSIG
	if (ioctl(pty->fd, TIOCSIG, sig) >= 0)
		return;
#endif
#ifdef TIOCGPGRP
#ifdef BROKEN_MASTER
	if (ioctl(pty->slave, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
		kill(-pgrp, sig) >= 0)
		return;
#endif
	if (ioctl(pty->fd, TIOCGPGRP, &pgrp) >= 0 && pgrp != -1 &&
		kill(-pgrp, sig) >= 0)
		return;
#endif

	/* Fallback using the child's pid. */
	kill(-pty->pid, sig);
}

/* Process activity from a client. */
static void
client_activity(struct client *p)
{
	int len;
	struct packet pkt;

	/* Read the activity. */
	len = read(p->fd, &pkt, sizeof(struct packet));
	/* Close the client on an error. */
	if (len <= 0)
	{
		close(p->fd);
		if (p->next)
			p->next->pprev = p->pprev;
		*(p->pprev) = p->next;
		free(p);
		return;
	} 

	/* Push out data to the program. */
	if (pkt.type == MSG_PUSH)
		write(the_pty.fd, pkt.u.buf, pkt.len);

	/* Attach or detach from the program. */
	else if (pkt.type == MSG_ATTACH)
		p->attached = 1;
	else if (pkt.type == MSG_DETACH)
		p->attached = 0;

	/* Window size change request, without a forced redraw. */
	else if (pkt.type == MSG_WINCH)
	{
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);
	}

	/* Force a redraw using a particular method. */
	else if (pkt.type == MSG_REDRAW)
	{
		int method = pkt.len;

		/* If the client didn't specify a particular method, use
		** whatever we had on startup. */
		if (method == REDRAW_UNSPEC)
			method = redraw_method;
		if (method == REDRAW_NONE)
			return;

		/* Set the window size. */
		the_pty.ws = pkt.u.ws;
		ioctl(the_pty.fd, TIOCSWINSZ, &the_pty.ws);

		/* Send a ^L character if the terminal is in no-echo and
		** character-at-a-time mode. */
		if (method == REDRAW_CTRL_L)
		{
			char c = '\f';

                	if (((the_pty.term.c_lflag & (ECHO|ICANON)) == 0) &&
                        	(the_pty.term.c_cc[VMIN] == 1))
			{
				write(the_pty.fd, &c, 1);
			}
		}
		/* Send a WINCH signal to the program. */
		else if (method == REDRAW_WINCH)
		{
			killpty(&the_pty, SIGWINCH);
		}
	}
}

/* The master process - It watches over the pty process and the attached */
/* clients. */
static void
master_process(int s, char **argv)
{
	struct client *p, *next;
	fd_set readfds;
	int highest_fd;

	/* Okay, disassociate ourselves from the original terminal, as we
	** don't care what happens to it. */
	setsid();

	/* Create a pty in which the process is running. */
	if (init_pty(argv) < 0)
	{
		printf("%s: init_pty: %s\n", progname, strerror(errno));
		exit(1);
	}

	/* Set up some signals. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	signal(SIGTTIN, SIG_IGN);
	signal(SIGTTOU, SIG_IGN);
	signal(SIGINT, die);
	signal(SIGTERM, die);
	signal(SIGCHLD, die);

	/* Close the original terminal. We are now a daemon. */
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);

	/* Set a trap to unlink the socket when we die. */
	atexit(unlink_socket);

	/* Loop forever. */
	while (1)
	{
		/* Re-initialize the file descriptor set for select. */
		FD_ZERO(&readfds);
		FD_SET(s, &readfds);
		FD_SET(the_pty.fd, &readfds);
		if (s > the_pty.fd)
			highest_fd = s;
		else
			highest_fd = the_pty.fd;
		for (p = clients; p; p = p->next)
		{
			FD_SET(p->fd, &readfds);
			if (p->fd > highest_fd)
				highest_fd = p->fd;
		}

		/* Wait for something to happen. */
		if (select(highest_fd + 1, &readfds, NULL, NULL, NULL) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			exit(1);
		}	
		/* pty activity? */
		if (FD_ISSET(the_pty.fd, &readfds))
			pty_activity(&the_pty);
		/* New client? */
		if (FD_ISSET(s, &readfds))
			control_activity(s);
		/* Activity on a client? */
		for (p = clients; p; p = next)
		{
			next = p->next;
			if (FD_ISSET(p->fd, &readfds))
				client_activity(p);
		}
	}
}

int
master_main(char **argv)
{
	int s;
	pid_t pid;

	/* Use a default redraw method if one hasn't been specified yet. */
	if (redraw_method == REDRAW_UNSPEC)
		redraw_method = REDRAW_CTRL_L;

	/* Create the unix domain socket. */
	s = create_socket(sockname);
	if (s < 0)
	{
		printf("%s: %s: %s\n", progname, sockname, strerror(errno));
		return 1;
	}

	/* Fork off so we can daemonize and such */
	pid = fork();
	if (pid < 0)
	{
		printf("%s: fork: %s\n", progname, strerror(errno));
		return 1;
	}
	else if (pid == 0)
	{
		/* Child - this becomes the master */
		master_process(s, argv);
		return 0;
	}
	/* Parent - just return. */
	close(s);
	return 0;
}

/* BSDish functions for systems that don't have them. */
#ifndef HAVE_OPENPTY
#define HAVE_OPENPTY
/* openpty: Use /dev/ptmx and Unix98 if we have it. */
#if defined(HAVE_PTSNAME) && defined(HAVE_GRANTPT) && defined(HAVE_UNLOCKPT)
int
openpty(int *amaster, int *aslave, char *name, struct termios *termp,
	struct winsize *winp)
{
	int master, slave;
	char *buf;

	master = open("/dev/ptmx", O_RDWR);
	if (master < 0)
		return -1;
	if (grantpt(master) < 0)
		return -1;
	if (unlockpt(master) < 0)
		return -1;
	buf = ptsname(master);
	if (!buf)
		return -1;

	slave = open(buf, O_RDWR|O_NOCTTY);
	if (slave < 0)
		return -1;

#ifdef I_PUSH
	if (ioctl(slave, I_PUSH, "ptem") < 0)
		return -1;
	if (ioctl(slave, I_PUSH, "ldterm") < 0)
		return -1;
#endif

	*amaster = master;
	*aslave = slave;
	if (name)
		strcpy(name, buf);
	if (termp)
		tcsetattr(slave, TCSAFLUSH, termp);
	if (winp)
		ioctl(slave, TIOCSWINSZ, winp);
	return 0;
}
#else
#error Do not know how to define openpty.
#endif
#endif

#ifndef HAVE_FORKPTY
#if defined(HAVE_OPENPTY)
pid_t
forkpty(int *amaster, char *name, struct termios *termp,
	struct winsize *winp)
{
	pid_t pid;
	int master, slave;

	if (openpty(&master, &slave, name, termp, winp) < 0)
		return -1;
	*amaster = master;

	/* Fork off... */
	pid = fork();
	if (pid < 0)
		return -1;
	else if (pid == 0)
	{
		char *buf;
		int fd;

		setsid();
#ifdef TIOCSCTTY
		buf = NULL;
		if (ioctl(slave, TIOCSCTTY, NULL) < 0)
			_exit(1);
#else
		buf = ptsname(master);
		fd = open(buf, O_RDWR);
		close(fd);
#endif
		dup2(slave, 0);
		dup2(slave, 1);
		dup2(slave, 2);

		if (slave > 2)
			close(slave);
		close(master);
		return 0;
	}
	else
	{
		close(slave);
		return pid;
	}
}
#else
#error Do not know how to define forkpty.
#endif
#endif
