Summary: A simple program that emulates the detach feature of screen.
Name: dtach
Version: 0.4
Release: 1
License: GPL
URL: http://dtach.sourceforge.net
Group: Applications/System
Source: http://prdownloads.sourceforge.net/dtach/dtach-%{version}.tar.gz
Buildroot: %{_tmppath}/%{name}-%{version}-root
 
%description

dtach is a program that emulates the detach feature of screen, with
less overhead.  It is designed to be transparent and un-intrusive; it
avoids interpreting the input and output between attached terminals
and the program under its control. Consequently, it works best with
full-screen applications such as emacs.
  
%prep
%setup
 
%build
%configure
make
 
%install
rm -rf $RPM_BUILD_ROOT/*
mkdir -p $RPM_BUILD_ROOT/%{_bindir}
mkdir -p $RPM_BUILD_ROOT/usr/share/doc/dtach-%{version}
mkdir -p $RPM_BUILD_ROOT/%{_mandir}/man1
install -m 755 dtach $RPM_BUILD_ROOT/%{_bindir}/dtach
install -m 644 dtach.1 $RPM_BUILD_ROOT/%{_mandir}/man1/dtach.1

%clean
make clean
rm -rf $RPM_BUILD_ROOT
rm -rf $RPM_BUILD_DIR/%{name}-%{version}

%files
%defattr(-,root,root,-)
%doc COPYING README
%{_bindir}/dtach
%{_mandir}/*/*

%changelog
* Fri Nov 30 2001 Trond Eivind Glomsrød <teg@redhat.com> 0.4-1
- s/Copyright/License/
- Minor description change
- fix use of %%doc
- Add full location of source tarball

* Sat Nov 03 2001 Ned T. Crigler <crigler@hell-city.org> 0.4
- Portability updates thanks to sourceforge's compile farm. dtach should now
  work on: FreeBSD, Debian/alpha, Debian/PPC, Debian/sparc, Debian/PPC, and
  Solaris.

* Thu Sep 27 2001 Ned T. Crigler <crigler@hell-city.org>
- Modified spec file URL: to point to http://dtach.sourceforge.net

* Wed Sep 26 2001 Ned T. Crigler <crigler@hell-city.org> 0.3
- Use getrlimit and dynamically allocate the data structures, if possible.
- Added some more autoconf checks.
- Initial sourceforge release.

* Thu Sep 20 2001 Ned T. Crigler <crigler@hell-city.org>
- Changed the master to send a stream of text to attaching clients instead
  of sending a huge packet all the time.
- Decreased the client <-> master packet size.
- Changed the attach code so that it tells the master when a suspend occurs.

* Tue Sep 18 2001 Ned T. Crigler <crigler@hell-city.org>
- Fixed a typo in dtach.1

* Tue Sep 18 2001 Ned T. Crigler <crigler@hell-city.org> 0.2
- Removed silly thinko regarding terminal settings in attach, we
  always set the terminal to raw mode now.
- Moved redraw code into the master, which tries to be smarter when
  using ^L.
- Moved the code that obtains the current terminal settings into main,
  preventing a race condition between the master and attach processes.
- Rewrote argument parsing code.
- Changed name to dtach.
- Added a man page.

* Mon Sep 17 2001 Ned T. Crigler <crigler@hell-city.org>
- Changed fchmod to chmod in create_socket.

* Mon Sep 17 2001 Isaiah Weiner <iweiner@redhat.com>
- Modified spec file to correct detach binary permissions
- Modified spec file to correct detach documentation path
- Modified spec file URL: to point to http://people.redhat.com/iweiner/detach
- Modified spec file %clean to remove buildroot and builddir.

* Mon Sep 17 2001 Ned T. Crigler <crigler@hell-city.org> 0.1
- Initial rpm release.
