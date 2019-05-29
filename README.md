$Id: README,v 1.2 2001/09/11 02:37:44 james Rel $

Quick installation:

	% ./configure
	% make
	# make install

See INSTALL for more complete instructions.

See TODO for ideas for future development.

**Usage：**
```
Usage: tcpxd [options]
       [[listen-address:]listen-port remote-host:remote-port]

Options can be
    --once         allow only one connection to happen then exit,
    --foreground   don't become a daemon but stay in the foreground,
    --timeout n    wait only n seconds for a connection,
    --local port   specify the port to call out on,
    --input file   list of 'port host port' triplets to use,
    --verbose      increment verbosity, repeat as required.

Parameters are
    listen-port    which local port to accept connection on
    remote-host    host to connect to when relaying connection
    remote-port    port to connect to when relaying connection

Example: tcpxd 110 pop.example.com 110
         relays POP3 connections to another host

Example: tcpxd 8023 localhost 23
         redirects connections on port 8023 to port 23
```

Maintainer is quozl@us.netrek.org.

#### Ref
http://quozl.us.netrek.org/tcpxd/