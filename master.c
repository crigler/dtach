/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2001 Ned T. Crigler

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
#include "detach.h"

/* The pty struct - The pty information is stored here. */
struct pty
{
	/* File descriptor of the pty */
	int fd;
#ifdef BROKEN_MASTER
	/* File descriptor of the slave side of the pty. For broken systems. */
	int slave;
#endif
	/* The terminal parameters of the pty. Old and new for comparision
	** purposes. */
	struct termios term;
	/* The current window size of the pty. */
	struct winsize ws;
};

/* The poll structures */
static struct pollfd *polls;
/* The number of active poll slots */
static int num_polls;
/* Boolean array for whether a particular connection is attached. */
static int *attached;
/* The highest file descriptor possible, as returned by getrlimit. */
static int highest_fd;

/* The number of fixed slots in the poll structures */
#define FIXED_SLOTS 2

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
		close(polls[1].fd);
#endif
		return;
	}
	exit(1);
}

/* Initialize the pty structure. */
static int
init_pty(struct pty *pty, char **argv)
{
	pid_t pid;

	/* Use the original terminal's settings. We don't have to set the
	** window size here, because the attacher will send it in a packet. */
	pty->term = orig_term;

	/* Create the pty process */
	pid = forkpty(&pty->fd, NULL, &pty->term, NULL);
	if (pid < 0)
		return -1;
	else if (pid == 0)
	{
		int i;

		/* Child.. Close some file descriptors and execute the
		** program. */
		for (i = highest_fd; i > 2; --i)
			close(i);

		execvp(*argv, argv);
		exit(127);
	}
	/* Parent.. Finish up and return */
#ifdef BROKEN_MASTER
	{
		char *buf;

		buf = ptsname(pty->fd);
		pty->slave = open(buf, O_RDWR|O_NOCTTY);
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

/* Process activity on a pty - Input and terminal changes are sent out to
** the attached clients. If the pty goes away, we die. */
static void
pty_activity(struct pty *pty)
{
	int i, len;
	unsigned char buf[BUFSIZE];

	/* Read the pty activity */
	len = read(pty->fd, buf, sizeof(buf));

	/* Error -> die */
	if (len <= 0)
		exit(1);

#ifdef BROKEN_MASTER
	/* Get the current terminal settings. */
	if (tcgetattr(pty->slave, &pty->term) < 0)
		exit(1);
#else
	/* Get the current terminal settings. */
	if (tcgetattr(pty->fd, &pty->term) < 0)
		exit(1);
#endif

	/* Send it out to the clients. */
	for (i = FIXED_SLOTS; i < num_polls; ++i)
	{
		if (attached[polls[i].fd])
			write(polls[i].fd, buf, len);
	}
}

/* Process activity on the control socket */
static void
control_activity(int s)
{
	int fd;
 
	/* Accept the new client and link it in. */
	fd = accept(s, 0, 0);
	if (fd < 0)
		return;

	/* Link it in. */
	polls[num_polls].fd = fd;
	polls[num_polls].events = POLLIN;
	polls[num_polls].revents = 0;
	attached[fd] = 1;
	++num_polls;
}

/* Process activity from a client. */
static void
client_activity(int i, struct pty *pty)
{
	int len;
	struct packet pkt;

	/* Read the activity. */
	len = read(polls[i].fd, &pkt, sizeof(pkt));
	if (len <= 0)
	{
		/* Close the socket and go bye bye */
		attached[polls[i].fd]=0;
		close(polls[i].fd);
		memcpy(polls + i, polls + i + 1, num_polls - i);
		--num_polls;
		return;
	} 

	/* Okay, check the command byte. Push out data if we need to. */
	if (pkt.type == MSG_PUSH)
		write(pty->fd, pkt.u.buf, pkt.len);
	/* Window size change. */
	else if (pkt.type == MSG_WINCH)
	{
		pty->ws = pkt.u.ws;
		ioctl(pty->fd, TIOCSWINSZ, &pty->ws);
	}
	/* Redraw request? */
	else if (pkt.type == MSG_REDRAW)
	{
		char c = '\f';

		/* Guess that ^L might work under certain conditions. */
		if (((pty->term.c_lflag & (ECHO|ICANON)) == 0) &&
			(pty->term.c_cc[VMIN] == 1))
		{
			write(pty->fd, &c, sizeof(c));
		}
	}
	/* Attach request? */
	else if (pkt.type == MSG_ATTACH)
		attached[polls[i].fd] = 1;
	else if (pkt.type == MSG_DETACH)
		attached[polls[i].fd] = 0;
}

/* The master process - It watches over the pty process and the attached */
/* clients. */
static void
master_process(int s, char **argv)
{
	struct pty pty;
	int i;

#ifdef HAVE_GETRLIMIT
	struct rlimit rlim;

	/* Dynamically allocate structures based on the number of file
	** descriptors. */

	if (getrlimit(RLIMIT_NOFILE, &rlim) < 0)
	{	
		printf("%s: getrlimit: %s\n", progname, strerror(errno));
		exit(1);
	}
	highest_fd = rlim.rlim_cur;
#else
	/* We can't query the OS for the number of file descriptors, so
	** we pull a number out of the air. */
	highest_fd = 1024;
#endif
	polls = (struct pollfd*)malloc(highest_fd * sizeof(struct pollfd));
	attached = (int*)malloc(highest_fd * sizeof(int));

	/* Okay, disassociate ourselves from the original terminal, as we
	** don't care what happens to it. */
	setsid();

	/* Create a pty in which the process is running. */
	if (init_pty(&pty, argv) < 0)
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

	/* Set a trap to unlink the socket when we die */
	atexit(unlink_socket);

	/* Set up the poll structures. Slot 0 is the control socket, slot 1
	** is the pty, and slot 2 .. n is the connected clients. */
	polls[0].fd = s;
	polls[0].events = POLLIN;
	polls[0].revents = 0;
	polls[1].fd = pty.fd;
	polls[1].events = POLLIN;
	polls[1].revents = 0;
	num_polls = FIXED_SLOTS;

	/* Loop forever. */
	while (1)
	{
		/* Wait for something to happen. */
		if (poll(polls, num_polls, -1) < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			exit(1);
		}	
		/* pty activity? */
		if (polls[1].revents != 0)
			pty_activity(&pty);
		/* New client? */
		if (polls[0].revents != 0)
			control_activity(s);
		/* Activity on a client? */
		for (i = 2; i < num_polls; ++i)
		{
			if (polls[i].revents != 0)
				client_activity(i, &pty);
		}
	}
}

int
master_main(char **argv)
{
	int s;
	pid_t pid;

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
