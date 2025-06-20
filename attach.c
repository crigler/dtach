/*
    dtach - A simple program that emulates the detach feature of screen.
    Copyright (C) 2004-2016 Ned T. Crigler

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
#include "dtach.h"

#ifndef VDISABLE
#ifdef _POSIX_VDISABLE
#define VDISABLE _POSIX_VDISABLE
#else
#define VDISABLE 0377
#endif
#endif

/*
** The current terminal settings. After coming back from a suspend, we
** restore this.
*/
static struct termios cur_term;
/* 1 if the window size changed */
static int win_changed;

/* Restores the original terminal settings. */
static void
restore_term(void)
{
	tcsetattr(0, TCSADRAIN, &orig_term);

	/* Make cursor visible. Assumes VT100. */
	printf("\033[?25h");
	fflush(stdout);
}

/* Connects to a unix domain socket */
static int
connect_socket(char *name)
{
	int s;
	struct sockaddr_un sockun;

	if (strlen(name) > sizeof(sockun.sun_path) - 1)
	{
		errno = ENAMETOOLONG;
		return -1;
	}

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	strcpy(sockun.sun_path, name);
	if (connect(s, (struct sockaddr *)&sockun, sizeof(sockun)) < 0)
	{
		close(s);

		/* ECONNREFUSED is also returned for regular files, so make
		** sure we are trying to connect to a socket. */
		if (errno == ECONNREFUSED)
		{
			struct stat st;

			if (stat(name, &st) < 0)
				return -1;
			else if (!S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode))
				errno = ENOTSOCK;
		}
		return -1;
	}
	return s;
}

/* Signal */
static RETSIGTYPE
die(int sig)
{
	/* Print a nice pretty message for some things. */
	if (sig == SIGHUP || sig == SIGINT)
		printf(EOS "\r\n[detached]\r\n");
	else
		printf(EOS "\r\n[got signal %d - dying]\r\n", sig);
	exit(1);
}

/* Window size change. */
static RETSIGTYPE
win_change(ATTRIBUTE_UNUSED int sig)
{
	signal(SIGWINCH, win_change);
	win_changed = 1;
}

/* Handles input from the keyboard. */
static void
process_kbd(int s, struct packet *pkt)
{
	/* Suspend? */
	if (!no_suspend && (pkt->u.buf[0] == cur_term.c_cc[VSUSP]))
	{
		/* Tell the master that we are suspending. */
		pkt->type = MSG_DETACH;
		write_packet_or_fail(s, pkt);

		/* And suspend... */
		tcsetattr(0, TCSADRAIN, &orig_term);
		printf(EOS "\r\n");
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		/* Tell the master that we are returning. */
		pkt->type = MSG_ATTACH;
		write_packet_or_fail(s, pkt);

		/* We would like a redraw, too. */
		pkt->type = MSG_REDRAW;
		pkt->len = redraw_method;
		ioctl(0, TIOCGWINSZ, &pkt->u.ws);
		write_packet_or_fail(s, pkt);
		return;
	}
	/* Detach char? */
	else if (pkt->u.buf[0] == detach_char)
	{
		printf(EOS "\r\n[detached]\r\n");
		exit(0);
	}
	/* Just in case something pukes out. */
	else if (pkt->u.buf[0] == '\f')
		win_changed = 1;

	/* Push it out */
	write_packet_or_fail(s, pkt);
}

int
attach_main(int noerror)
{
	struct packet pkt;
	unsigned char buf[BUFSIZE];
	fd_set readfds;
	int s;

	/* Attempt to open the socket. Don't display an error if noerror is
	** set. */
	s = connect_socket(sockname);
	if (s < 0 && errno == ENAMETOOLONG)
	{
		char *slash = strrchr(sockname, '/');

		/* Try to shorten the socket's path name by using chdir. */
		if (slash)
		{
			int dirfd = open(".", O_RDONLY);

			if (dirfd >= 0)
			{
				*slash = '\0';
				if (chdir(sockname) >= 0)
				{
					s = connect_socket(slash + 1);
					if (s >= 0 && fchdir(dirfd) < 0)
					{
						close(s);
						s = -1;
					}
				}
				*slash = '/';
				close(dirfd);
			}
		}
	}
	if (s < 0)
	{
		if (!noerror)
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
		return 1;
	}

	/* The current terminal settings are equal to the original terminal
	** settings at this point. */
	cur_term = orig_term;

	/* Set a trap to restore the terminal when we die. */
	atexit(restore_term);

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, die);
	signal(SIGTERM, die);
	signal(SIGINT, die);
	signal(SIGQUIT, die);
	signal(SIGWINCH, win_change);

	/* Set raw mode. */
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
	cur_term.c_iflag &= ~(IXON|IXOFF);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = VDISABLE;
	cur_term.c_cc[VMIN] = 1;
	cur_term.c_cc[VTIME] = 0;
	tcsetattr(0, TCSADRAIN, &cur_term);

	/* Clear the screen. This assumes VT100. */
	write_buf_or_fail(1, "\33[H\33[J", 6);

	/* Tell the master that we want to attach. */
	memset(&pkt, 0, sizeof(struct packet));
	pkt.type = MSG_ATTACH;
	write_packet_or_fail(s, &pkt);

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write_packet_or_fail(s, &pkt);

	/* Wait for things to happen */
	while (1)
	{
		int n;

		FD_ZERO(&readfds);
		FD_SET(0, &readfds);
		FD_SET(s, &readfds);
		n = select(s + 1, &readfds, NULL, NULL, NULL);
		if (n < 0 && errno != EINTR && errno != EAGAIN)
		{
			printf(EOS "\r\n[select failed]\r\n");
			exit(1);
		}

		/* Pty activity */
		if (n > 0 && FD_ISSET(s, &readfds))
		{
			ssize_t len = read(s, buf, sizeof(buf));

			if (len == 0)
			{
				printf(EOS "\r\n[EOF - dtach terminating]"
				       "\r\n");
				exit(0);
			}
			else if (len < 0)
			{
				printf(EOS "\r\n[read returned an error]\r\n");
				exit(1);
			}
			/* Send the data to the terminal. */
			write_buf_or_fail(1, buf, len);
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds))
		{
			ssize_t len;

			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (len <= 0)
				exit(1);

			pkt.len = len;
			process_kbd(s, &pkt);
			n--;
		}

		/* Window size changed? */
		if (win_changed)
		{
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write_packet_or_fail(s, &pkt);
		}
	}
	return 0;
}

int
push_main()
{
	struct packet pkt;
	int s;

	/* Attempt to open the socket. */
	s = connect_socket(sockname);
	if (s < 0 && errno == ENAMETOOLONG)
	{
		char *slash = strrchr(sockname, '/');

		/* Try to shorten the socket's path name by using chdir. */
		if (slash)
		{
			int dirfd = open(".", O_RDONLY);

			if (dirfd >= 0)
			{
				*slash = '\0';
				if (chdir(sockname) >= 0)
				{
					s = connect_socket(slash + 1);
					if (s >= 0 && fchdir(dirfd) < 0)
					{
						close(s);
						s = -1;
					}
				}
				*slash = '/';
				close(dirfd);
			}
		}
	}
	if (s < 0)
	{
		printf("%s: %s: %s\n", progname, sockname, strerror(errno));
		return 1;
	}

	/* Set some signals. */
	signal(SIGPIPE, SIG_IGN);

	/* Push the contents of standard input to the socket. */
	pkt.type = MSG_PUSH;
	for (;;)
	{
		ssize_t len;

		memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
		len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

		if (len == 0)
			return 0;
		else if (len < 0)
		{
			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}

		pkt.len = len;
		len = write(s, &pkt, sizeof(struct packet));
		if (len != sizeof(struct packet))
		{
			if (len >= 0)
				errno = EPIPE;

			printf("%s: %s: %s\n", progname, sockname,
			       strerror(errno));
			return 1;
		}
	}
}
