# Process this file with autoconf to produce a configure script.
AC_INIT(dtach, 0.8, crigler@users.sourceforge.net)
AC_PREREQ(2.59)
AC_CONFIG_SRCDIR(main.c)
AC_CONFIG_HEADER(config.h)

# Checks for programs.
AC_PROG_CC
AC_PROG_GCC_TRADITIONAL

if test "$GCC" = yes; then
	CFLAGS="$CFLAGS -W -Wall";
fi

# Checks for libraries.
AC_CHECK_LIB(util, openpty)
AC_CHECK_LIB(socket, socket)

# Checks for header files.
AC_CHECK_HEADERS(fcntl.h sys/select.h sys/socket.h sys/time.h)
AC_CHECK_HEADERS(sys/ioctl.h sys/resource.h pty.h termios.h util.h)
AC_CHECK_HEADERS(libutil.h stropts.h)
AC_HEADER_TIME

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST
AC_TYPE_PID_T

# Checks for library functions.
AC_TYPE_SIGNAL
AC_CHECK_FUNCS(atexit dup2 memset)
AC_CHECK_FUNCS(select socket strerror)
AC_CHECK_FUNCS(openpty forkpty ptsname grantpt unlockpt)

AC_CONFIG_FILES(Makefile)
AC_OUTPUT
