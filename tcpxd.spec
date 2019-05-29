%define name tcpxd
%define ver 1.2
%define extension tar.gz

Summary: TCP/IP Relay
Name: %name
Version: %{ver}
Release: 2
Copyright: GPL
Group: Applications/Networking
Source: %{name}-%{ver}.%{extension}
URL: http://quozl.netrek.org/tcpxd/
Buildroot: /tmp/%{name}-%{ver}-root

%description
tcpxd is a general purpose TCP/IP relay program designed to be simple
to get going, requiring only three parameters; the port to listen on,
the host to forward to, and the port on that host to connect to.

The features of this particular relay program are:

- accepts connections and forwards them to another host,
- uses TCP_NODELAY to avoid almalgamating X packets,
- can be told to listen only to connections on a specific interface
- multiple simultaneous connections without threading or forking,
- no buffering; waits for peer to be writeable before reading
- relays a connection closure on one side to the other, but can
  still keep the return connection open until the peer closes,
- resolves IP addresses on initialisation rather than on each connection
- performs the connect() call asynchronously, such that a new
  connection does not affect existing connections.

%prep
%setup
%build
./configure --prefix=/usr
make

%install
make install DESTDIR=${RPM_BUILD_ROOT}
strip $RPM_BUILD_ROOT/usr/bin/* || :

install -d $RPM_BUILD_ROOT/etc
install -m 644 ./tcpxd.conf $RPM_BUILD_ROOT/etc/tcpxd.conf
install -d $RPM_BUILD_ROOT/etc/rc.d/init.d
install -m 755 ./tcpxd.init $RPM_BUILD_ROOT/etc/rc.d/init.d/tcpxd

%clean
rm -rf $RPM_BUILD_ROOT

%files
%doc ChangeLog AUTHORS COPYING README TODO NEWS
%doc ${RPM_BUILD_ROOT}/usr/doc/
%config /etc/tcpxd.conf
%config /etc/rc.d/init.d/tcpxd
/usr/bin/

%changelog
* Mon May 21 2001 Peter Holzleitner <peter@holzleitner.com>
- created the package
