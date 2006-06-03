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

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sockun.sun_family = AF_UNIX;
	strcpy(sockun.sun_path, name);
	if (connect(s, (struct sockaddr*)&sockun, sizeof(sockun)) < 0)
	{
		close(s);
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
win_change()
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
		write(s, pkt, sizeof(struct packet));

		/* And suspend... */
		tcsetattr(0, TCSADRAIN, &orig_term);
		printf(EOS "\r\n");
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		/* Tell the master that we are returning. */
		pkt->type = MSG_ATTACH;
		write(s, pkt, sizeof(struct packet));

		/* We would like a redraw, too. */
		pkt->type = MSG_REDRAW;
		pkt->len = redraw_method;
		ioctl(0, TIOCGWINSZ, &pkt->u.ws);
		write(s, pkt, sizeof(struct packet));
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
	write(s, pkt, sizeof(struct packet));
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
	write(1, "\33[H\33[J", 6);

	/* Tell the master that we want to attach. */
	pkt.type = MSG_ATTACH;
	write(s, &pkt, sizeof(struct packet));

	/* We would like a redraw, too. */
	pkt.type = MSG_REDRAW;
	pkt.len = redraw_method;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write(s, &pkt, sizeof(struct packet));

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
			int len = read(s, buf, sizeof(buf));

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
			write(1, buf, len);
			n--;
		}
		/* stdin activity */
		if (n > 0 && FD_ISSET(0, &readfds))
		{
			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			pkt.len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (pkt.len <= 0)
				exit(1);
			process_kbd(s, &pkt);
			n--;
		}

		/* Window size changed? */
		if (win_changed)
		{
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write(s, &pkt, sizeof(pkt));
		}
	}
	return 0;
}
