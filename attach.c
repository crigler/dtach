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

#ifndef VDISABLE
#ifdef _POSIX_VDISABLE
#define VDISABLE _POSIX_VDISABLE
#else
#define VDISABLE 0377
#endif
#endif

// The current terminal settings. After coming back from a suspend, we
// restore this.
static struct termios cur_term;
// 1 if the window size changed
static int win_changed;
// 1 if we want a redraw
static int want_redraw;

// This hopefully moves to the bottom of the screen
#define EOS "\033[999H"

// Restores the original terminal settings.
static void
restore_term(void)
{
	tcsetattr(0, TCSADRAIN, &orig_term);
	// Make cursor visible. Assumes VT100.
	printf("\033[?25h\033[?0c");
	fflush(stdout);
}

// Connects to a unix domain socket
static int
connect_socket(char *name)
{
	int s;
	struct sockaddr_un sun;

	s = socket(PF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	sun.sun_family = AF_UNIX;
	strcpy(sun.sun_path, name);
	if (connect(s, (struct sockaddr*)&sun, sizeof(sun)) < 0)
		return -1;
	return s;
}

// Signal
static RETSIGTYPE
die(int sig)
{
	// Print a nice pretty message for some things.
	if (sig == SIGHUP || sig == SIGINT)
		printf(EOS "\r\n[detached]\r\n");
	else
		printf(EOS "\r\n[got signal %d - dying]\r\n", sig);
	exit(1);
}

// Window size change.
static RETSIGTYPE
win_change()
{
	win_changed = 1;
}

// Handles input from the keyboard.
static void
process_kbd(int s, struct packet *pkt)
{
	// Suspend?
	if (!no_suspend && (pkt->u.buf[0] == cur_term.c_cc[VSUSP]))
	{
		// Tell the master that we are suspending.
		pkt->type = MSG_DETACH;
		write(s, pkt, sizeof(*pkt));

		// And suspend...
		tcsetattr(0, TCSADRAIN, &orig_term);
		kill(getpid(), SIGTSTP);
		tcsetattr(0, TCSADRAIN, &cur_term);

		// Tell the master that we are returning.
		pkt->type = MSG_ATTACH;
		write(s, pkt, sizeof(*pkt));

		// The window size might have changed, and we definately want
		// a redraw. We don't want to pass the suspend, though.
		win_changed = 1;
		want_redraw = 1;
		return;
	}
	// Detach char?
	else if (pkt->u.buf[0] == detach_char)
	{
		printf(EOS "\r\n[detached]\r\n");
		exit(1);
	}
	// Just in case something pukes out.
	else if (pkt->u.buf[0] == '\f')
		win_changed = 1;

	// Push it out
	write(s, pkt, sizeof(*pkt));
}

int
attach_main(int noerror)
{
	int s;
	struct pollfd polls[2];
	struct packet pkt;
	unsigned char buf[BUFSIZE];

	// The current terminal settings are equal to the original terminal
	// settings at this point.
	cur_term = orig_term;

	// Set a trap to restore the terminal when we die.
	atexit(restore_term);

	// Set some signals.
	signal(SIGPIPE, SIG_IGN);
	signal(SIGXFSZ, SIG_IGN);
	signal(SIGHUP, die);
	signal(SIGTERM, die);
	signal(SIGINT, die);
	signal(SIGQUIT, die);
	signal(SIGWINCH, win_change);

	// Attempt to open the socket. Don't display an error if noerror is
	// set.
	s = connect_socket(sockname);
	if (s < 0)
	{
		if (!noerror)
			printf("%s: %s: %s\n", progname, sockname,
				strerror(errno));
		return 1;
	}

	// Set raw mode, almost. We allow flow control to work, for instance.
	cur_term.c_iflag &= ~(IGNBRK|BRKINT|PARMRK|ISTRIP|INLCR|IGNCR|ICRNL);
	cur_term.c_oflag &= ~(OPOST);
	cur_term.c_lflag &= ~(ECHO|ECHONL|ICANON|ISIG|IEXTEN);
	cur_term.c_cflag &= ~(CSIZE|PARENB);
	cur_term.c_cflag |= CS8;
	cur_term.c_cc[VLNEXT] = VDISABLE;

	tcsetattr(0, TCSADRAIN, &cur_term);

	// Clear the screen. This assumes VT100.
	write(1, "\33[H\33[J", 6);

	// Set up the poll structures
	polls[0].fd = 0;
	polls[0].events = POLLIN;
	polls[0].revents = 0;
	polls[1].fd = s;
	polls[1].events = POLLIN;
	polls[1].revents = 0;

	// Send our window size.
	pkt.type = MSG_WINCH;
	ioctl(0, TIOCGWINSZ, &pkt.u.ws);
	write(s, &pkt, sizeof(pkt));
	// We would like a redraw, too.
	pkt.type = MSG_REDRAW;
	write(s, &pkt, sizeof(pkt));

	// Wait for things to happen
	while (1)
	{
		if (poll(polls, 2, -1) < 0)
		{
			if (errno != EINTR && errno != EAGAIN)
			{
				printf(EOS "\r\n[poll failed]\r\n");
				exit(1);
			}
		}
		// Pty activity
		if (polls[1].revents != 0)
		{
			int len = read(s, buf, sizeof(buf));

			if (len == 0)
			{
				printf(EOS "\r\n[EOF - dtach terminating]"
					"\r\n");
				exit(1);
			}
			else if (len < 0)
			{
				printf(EOS "\r\n[read returned an error]\r\n");
				exit(1);
			}
			// Send the data to the terminal.
			write(1, buf, len);
		}
		// stdin activity
		if (polls[0].revents != 0)
		{
			pkt.type = MSG_PUSH;
			memset(pkt.u.buf, 0, sizeof(pkt.u.buf));
			pkt.len = read(0, pkt.u.buf, sizeof(pkt.u.buf));

			if (pkt.len <= 0)
				exit(1);
			process_kbd(s, &pkt);
		}
		// Window size changed?
		if (win_changed)
		{
			win_changed = 0;

			pkt.type = MSG_WINCH;
			ioctl(0, TIOCGWINSZ, &pkt.u.ws);
			write(s, &pkt, sizeof(pkt));
		}
		// Want a redraw?
		if (want_redraw)
		{
			want_redraw = 0;

			pkt.type = MSG_REDRAW;
			write(s, &pkt, sizeof(pkt));
		}
	}
	return 0;
}
