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

/*
** dtach is a quick hack, since I wanted the detach feature of screen without
** all the other crud. It'll work best with full-screen applications, as it
** does not keep track of the screen or anything like that.
*/

/* Make sure the binary has a copyright. */
const char copyright[] = "dtach - version " PACKAGE_VERSION "(C)Copyright 2004-2016 Ned T. Crigler";

/* argv[0] from the program */
char *progname;
/* The name of the passed in socket. */
char *sockname;
/* The character used for detaching. Defaults to '^\' */
int detach_char = '\\' - 64;
/* 1 if we should not interpret the suspend character. */
int no_suspend;
/* The default redraw method. Initially set to unspecified. */
int redraw_method = REDRAW_UNSPEC;
/* The default clear method. Initially set to unspecified. */
int clear_method = CLEAR_UNSPEC;
int quiet = 0;

/*
** The original terminal settings. Shared between the master and attach
** processes. The master uses it to initialize the pty, and the attacher uses
** it to restore the original settings.
*/
struct termios orig_term;
int dont_have_tty;

static void
usage()
{
	printf(
		"dtach - version %s, compiled on %s at %s.\n"
		"Usage: dtach -a <socket> <options>\n"
		"       dtach -A <socket> <options> <command...>\n"
		"       dtach -c <socket> <options> <command...>\n"
		"       dtach -n <socket> <options> <command...>\n"
		"       dtach -N <socket> <options> <command...>\n"
		"       dtach -p <socket>\n"
		"Modes:\n"
		"  -a\t\tAttach to the specified socket.\n"
		"  -A\t\tAttach to the specified socket, or create it if it\n"
		"\t\t  does not exist, running the specified command.\n"
		"  -c\t\tCreate a new socket and run the specified command.\n"
		"  -n\t\tCreate a new socket and run the specified command "
		"detached.\n"
		"  -N\t\tCreate a new socket and run the specified command "
		"detached,\n"
		"\t\t  and have dtach run in the foreground.\n"
		"  -p\t\tCopy the contents of standard input to the specified\n"
		"\t\t  socket.\n"
		"Options:\n"
		"  -e <char>\tSet the detach character to <char>, defaults "
		"to ^\\.\n"
		"  -E\t\tDisable the detach character.\n"
		"  -r <method>\tSet the redraw method to <method>. The "
		"valid methods are:\n"
		"\t\t     none: Don't redraw at all.\n"
		"\t\t   ctrl_l: Send a Ctrl L character to the program.\n"
		"\t\t    winch: Send a WINCH signal to the program.\n"
		"  -R <method>\tSet the clear method to <method>. The "
		"valid methods are:\n"
		"\t\t     none: Don't clear at all.\n"
		"\t\t     move: Move to last line (default behaviour).\n"
		"  -z\t\tDisable processing of the suspend key.\n"
		"  -q\t\tDisable printing of additional messages.\n"
		"\nReport any bugs to <" PACKAGE_BUGREPORT ">.\n",
		PACKAGE_VERSION, __DATE__, __TIME__);
	exit(0);
}

int
main(int argc, char **argv)
{
	int mode = 0;

	/* Save the program name */
	progname = argv[0];
	++argv; --argc;

	/* Parse the arguments */
	if (argc >= 1 && **argv == '-')
	{
		if (strncmp(*argv, "--help", strlen(*argv)) == 0)
			usage();
		else if (strncmp(*argv, "--version", strlen(*argv)) == 0)
		{
			printf("dtach - version %s, compiled on %s at %s.\n",
				PACKAGE_VERSION, __DATE__, __TIME__);
			return 0;
		}

		mode = argv[0][1];
		if (mode == '?')
			usage();
		else if (mode != 'a' && mode != 'c' && mode != 'n' &&
			 mode != 'A' && mode != 'N' && mode != 'p')
		{
			printf("%s: Invalid mode '-%c'\n", progname, mode);
			printf("Try '%s --help' for more information.\n",
				progname);
			return 1;
		}
	}
	if (!mode)
	{
		printf("%s: No mode was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}
	++argv; --argc;

	if (argc < 1)
	{
		printf("%s: No socket was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}
	sockname = *argv;
	++argv; --argc;

	if (mode == 'p')
	{
		if (argc > 0)
		{
			printf("%s: Invalid number of arguments.\n",
				progname);
			printf("Try '%s --help' for more information.\n",
				progname);
			return 1;
		}
		return push_main();
	}

	while (argc >= 1 && **argv == '-')
	{
		char *p;

		if (strcmp(argv[0], "--") == 0) {
			++argv; --argc;
			break;
		}

		for (p = argv[0] + 1; *p; ++p)
		{
			if (*p == 'E')
				detach_char = -1;
			else if (*p == 'z')
				no_suspend = 1;
			else if (*p == 'q')
				quiet = 1;
			else if (*p == 'e')
			{
				++argv; --argc;
				if (argc < 1)
				{
					printf("%s: No escape character "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				if (argv[0][0] == '^' && argv[0][1])
				{
					if (argv[0][1] == '?')
						detach_char = '\177';
					else
						detach_char = argv[0][1] & 037;
				}
				else
					detach_char = argv[0][0];
				break;
			}
			else if (*p == 'r')
			{
				++argv; --argc;
				if (argc < 1)
				{
					printf("%s: No redraw method "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				if (strcmp(argv[0], "none") == 0)
					redraw_method = REDRAW_NONE;
				else if (strcmp(argv[0], "ctrl_l") == 0)
					redraw_method = REDRAW_CTRL_L;
				else if (strcmp(argv[0], "winch") == 0)
					redraw_method = REDRAW_WINCH;
				else
				{
					printf("%s: Invalid redraw method "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				break;
			}
			else if (*p == 'R')
			{
				++argv; --argc;
				if (argc < 1)
				{
					printf("%s: No clear method "
						"specified.\n", progname);	
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				if (strcmp(argv[0], "none") == 0)
					clear_method = CLEAR_NONE;
				else if (strcmp(argv[0], "move") == 0)
					clear_method = CLEAR_MOVE;
				else
				{
					printf("%s: Invalid clear method "
						"specified.\n", progname);
					printf("Try '%s --help' for more "
						"information.\n", progname);
					return 1;
				}
				break;
			}
			else
			{
				printf("%s: Invalid option '-%c'\n",
					progname, *p);
				printf("Try '%s --help' for more information.\n",
					progname);
				return 1;
			}
		}
		++argv; --argc;
	}

	if (mode != 'a' && argc < 1)
	{
		printf("%s: No command was specified.\n", progname);
		printf("Try '%s --help' for more information.\n",
			progname);
		return 1;
	}

	/* Save the original terminal settings. */
	if (tcgetattr(0, &orig_term) < 0)
	{
		memset(&orig_term, 0, sizeof(struct termios));
		dont_have_tty = 1;
	}

	if (dont_have_tty && mode != 'n' && mode != 'N')
	{
		printf("%s: Attaching to a session requires a terminal.\n",
			progname);
		return 1;
	}

	if (mode == 'a')
	{
		if (argc > 0)
		{
			printf("%s: Invalid number of arguments.\n",
				progname);
			printf("Try '%s --help' for more information.\n",
				progname);
			return 1;
		}
		return attach_main(0);
	}
	else if (mode == 'n')
		return master_main(argv, 0, 0);
	else if (mode == 'N')
		return master_main(argv, 0, 1);
	else if (mode == 'c')
	{
		if (master_main(argv, 1, 0) != 0)
			return 1;
		return attach_main(0);
	}
	else if (mode == 'A')
	{
		/* Try to attach first. If that doesn't work, create a new
		** socket. */
		if (attach_main(1) != 0)
		{
			if (errno == ECONNREFUSED || errno == ENOENT)
			{
				if (errno == ECONNREFUSED)
					unlink(sockname);
				if (master_main(argv, 1, 0) != 0)
					return 1;
			}
			return attach_main(0);
		}
	}
	return 0;
}


char const * clear_csi_data()
{
	switch (clear_method) {
		case CLEAR_NONE :
			return "\r\n";
		case CLEAR_UNSPEC :
		case CLEAR_MOVE :
		default :
			/* This hopefully moves to the bottom of the screen */
			return "\033[999H\r\n";
	}
}

