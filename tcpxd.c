/* 	$Id: tcpxd.c,v 1.10 2003/04/14 23:37:39 james Rel james $	 */

#ifndef lint
static char vcid[] = "$Id: tcpxd.c,v 1.10 2003/04/14 23:37:39 james Rel james $";
#endif /* lint */

#define VERSION "1.4"

/*
    tcpxd, a generic TCP/IP relay proxy
    Copyright (C) 2000  James Cameron <quozl@us.netrek.org>

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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/uio.h>
#include <syslog.h>
#include <stdarg.h>
#ifndef TCP_NODELAY
#include <netinet/tcp.h>
#endif
#include <netdb.h>

#define MAXCHUNK 65536	   /* maximum transfer in any network read/write */
#define MAXSYSLOGLEN 1024  /* maximum length of messages sent to syslog  */

/*
 * One service structure exists for each port that the relay is to listen for
 * connections on.  The services are connected with a doubly linked list.
 */
struct service {
  struct service *prior;
  struct service *next;
  struct sockaddr_in listen;	/* local end of incoming connection	*/
  struct sockaddr_in local;	/* local end of outgoing connection	*/
  struct sockaddr_in remote;	/* remote end of outgoing connection	*/
  char *remote_host;		/* remote host name			*/
  int bandwidth;		/* bytes per second			*/
  int listener;			/* file descriptor for listening on	*/
  int accepts;			/* count of accept() calls on socket	*/
} *services;

/*
 * For each connection that arrives on a service listening port, a new per-
 * connection structure is created to maintain context.  Connections are also
 * arranged as a doubly linked list, with a pointer back to the owning service.
 */
struct connection {
  struct connection *prior;	/* link to prior connection	*/
  struct connection *next;	/* link to next connection	*/
  struct service *service;	/* link to owning service	*/
  struct sockaddr_in peer;	/* peer address			*/
  int number;			/* which connection this is	*/
  int connected;		/* whether connection is done	*/
  int incoming;			/* socket file descriptor	*/
  int outgoing;			/* socket file descriptor	*/
  int fast;			/* incoming socket has shut	*/
  int constipate;		/* outgoing socket has shut	*/
  int total;			/* total byte count so far	*/
  int allow;			/* currently allowed byte count */
  time_t start, now;		/* start time of connection	*/
} *connections;

fd_set sr, sw, se;		/* set bits for select()	*/
fd_set or, ow, oe;		/* our bits after select()	*/

int verbose = 0;		/* how verbose to be in logging */
int arg_foreground = 0;		/* stay in the foreground	*/

/* short form for exit with failure */
static void die ()
{
    exit(EXIT_FAILURE);
}

/* set a socket blocking */
static void block (int fd)
{
    int flags;
    flags = (~O_NONBLOCK) & fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags);
}

/* set a socket non-blocking */
static void unblock (int fd)
{
    int flags;
    flags = O_NONBLOCK | fcntl(fd, F_GETFL);
    fcntl(fd, F_SETFL, flags);
}

/* printf to stderr or syslog */
static void lprintf(int priority, char *format, ...)
{
    va_list ap;
    char logstr[MAXSYSLOGLEN];

    va_start(ap, format);
    if (arg_foreground) {
        vfprintf(stderr, format, ap);
    } else {
        vsnprintf(logstr, MAXSYSLOGLEN, format, ap);
        syslog(priority, "%s", logstr);
    }
}

/* log an error (like perror but via lprintf) */
static void lerror(char *message)
{
    lprintf(LOG_ERR, "%s: %s\n", message, strerror(errno));
}

/* 
 * File descriptor debugging indicates which file descriptors are
 * ready for reading or writing.  This generates a lot of output and
 * is normally not compiled in.
 */

#ifdef FEATURE_FILE_DESCRIPTOR_DEBUG

static int int_strlen(int integer) {
   int i = 1;
   while (integer /= 10) ++i;
   return i;
}

static void append(char **buffer, char *string) {
  if (*buffer == NULL) {
    *buffer = malloc(strlen(string) + 1);
    if (*buffer == NULL) exit(ENOMEM); 	/* out of memory */
    strcpy(*buffer, string);
  } else {
    *buffer = realloc(*buffer, strlen(*buffer) + strlen(string) + 1);
    if (*buffer == NULL) exit(ENOMEM); 	/* out of memory */
    strcat(*buffer, string);
  }
}

static void dump_fd_before_select()
{
  int i;
  char *out = NULL;
  char *tmp = malloc(int_strlen(FD_SETSIZE) + 1);
  
  append(&out, "select: ");
  for(i = 0;i < FD_SETSIZE;i++) {
    if (FD_ISSET(i, &or) || FD_ISSET(i, &ow)) {
      sprintf(tmp, "%d", i);
      append(&out, tmp);
      append(&out, FD_ISSET(i, &or) ? "r" : " ");
      append(&out, FD_ISSET(i, &ow) ? "w " : "  ");
    }
  }
  append(&out, "\n");
  lprintf(LOG_DEBUG, out);
  free(out);
  free(tmp);
}

static void dump_fd_after_select()
{
  int i;
  char *out = NULL;
  char *tmp = malloc(int_strlen(FD_SETSIZE) + 1);
  
  append(&out, "select: ");
  for(i = 0;i < FD_SETSIZE;i++) {
    if (FD_ISSET(i, &sr) || FD_ISSET(i, &sw)) {
      sprintf(tmp, "%d", i);
      append(&out, tmp);
      if (FD_ISSET(i, &sr)) 
	if (FD_ISSET(i, &or)) append(&out, "R"); 
	else append(&out, "r");
      else append(&out, " ");
      if (FD_ISSET(i,&sw))
	if (FD_ISSET(i,&ow)) append(&out, "R"); 
	else append(&out, "r"); 
      else append(&out, " "); 
      append(&out, " ");
    }
  }
  append(&out, "\n");
  lprintf(LOG_DEBUG, out);
  free(out);
  free(tmp);
}
#endif /* FEATURE_FILE_DESCRIPTOR_DEBUG */

static int file2fd(const char *path, const char *mode, int fd) {
  int ok = 0;
  FILE *f = NULL;

  f = fopen(path, mode);
  if (f != NULL && dup2(fileno(f), fd) != -1)
    ok = 1;

  if (f) fclose(f);

  return ok;
}

/* fork and disconnect from terminal */
static void become_daemon() {
  pid_t pid, sid;
  
  pid = fork();
  if (pid < 0) {
    perror("fork");
    die();
  }
  
  if (pid > 0)              exit (EXIT_SUCCESS);
  if ((sid = setsid()) < 0) die();
  if (chdir("/") < 0)       die();
  umask(0);
  if (! file2fd("/dev/null", "rb", STDIN_FILENO))  die();
  if (! file2fd("/dev/null", "wb", STDERR_FILENO)) die();
  if (! file2fd("/dev/null", "wb", STDOUT_FILENO)) die();
}

static void tohex(char **out, const unsigned char in[], int size) {
  char hex[] = "0123456789abcdef";
  int i;
  
  if (size < 1) return;
  sprintf(*out, "%c%c", hex[in[0]/16], hex[in[0]%16]);
  for(i = 1;i < size;++i) {
    sprintf(*out, "%s %c%c", *out, hex[in[i]/16], hex[in[i]%16]);
  }
}

/* 
 * Transfer data from one side to the other for a connection.  The code
 * presumes that select() has already indicated that the incoming socket
 * has data ready, and the outgoing socket is writeable.
 */
static int transfer ( int incoming, int outgoing, int allow )
{
  unsigned char data[MAXCHUNK];
  char *hex = NULL;
  int bytes;

  bytes = recv(outgoing, data, allow, 0);
  if (bytes == 0) return 0;
  if (bytes < 0) {
     lerror("recv(): outgoing");
     return 0;
  }

  if (verbose > 3) {
    if ((hex = malloc(bytes * 3)) == NULL) exit (ENOMEM); /* out of memory */
    tohex(&hex, data, bytes);
    lprintf(LOG_DEBUG, "%d->%d: %s\n", outgoing, incoming, hex);
    free(hex);
  }

  bytes = send(incoming, data, bytes, 0);
  if (bytes < 0) {
     lerror("send(): incoming");
     return 0;
  }
  return bytes;
}

/* perform host lookup for a service entry */
static void resolve ( struct service *service, char *host,
		      struct sockaddr_in *address )
{
  struct in_addr result;
  struct hostent *entry;
  
  if (verbose > 0)
     lprintf(LOG_INFO, "%d: resolving %s ", 
	   ntohs(service->listen.sin_port), host);

  if (inet_aton(host, &result)) {
    address->sin_addr = result;
    if (verbose > 0) lprintf(LOG_INFO, "using inet_aton() to %s\n", 
			     inet_ntoa(address->sin_addr));
    return;
  }

  entry = gethostbyname(host);
  if (entry == NULL) {
    herror(host);
    die();
  }

  address->sin_addr.s_addr = * (long *) entry->h_addr;
  if (verbose > 0) lprintf(LOG_INFO, "using gethostbyname() to %s\n", 
			   inet_ntoa(address->sin_addr));
}

static void get_address_port ( char *string, char **addr, short int *port )
{
  char *colon = strchr(string, ':');

  if (colon == NULL) {
    /* no colon in string */
    if (strchr(string, '.') == NULL) {
      /* no dots in string */
      if (isdigit(*string)) {
	/* numeric, must be port number */
	*port = atoi(string);
      } else {
	/* alphanumeric, must be unqualified host */
	*addr = strdup(string);
	*port = 0;
      }
    } else {
      /* dots in string, must be address */
      *addr = strdup(string);
      *port = 0;
    }
  } else {
    /* colon in string, split out address and port */
    *colon = 0;
    *addr = strdup(string);
    *colon = ':';
    *port = atoi(colon + 1);
  }
}

static void usage()
{
  fprintf(stderr, 
	  "tcpxd version " VERSION ", Copyright (C) 2001 James Cameron\n"
	  "tcpxd comes with ABSOLUTELY NO WARRANTY; for details see source.\n"
	  "This is free software, and you are welcome to redistribute it\n"
	  "under certain conditions; see source for details.\n\n" );
  
  fprintf(stderr, 
	  "Usage: tcpxd [options]\n"
          "       [[listen-address:]listen-port remote-host:remote-port]\n"
	  "\n"
	  "Options can be\n"
	  "    --once         allow only one connection to happen then exit,\n"
	  "    --foreground   don't become a daemon but stay in the foreground,\n"
	  "    --timeout n    wait only n seconds for a connection,\n"
	  "    --local port   specify the port to call out on,\n"
	  "    --input file   list of 'port host port' triplets to use,\n"
	  "    --verbose      increment verbosity, repeat as required.\n"
	  "\n"
	  "Parameters are\n"
	  "    listen-port    which local port to accept connection on\n"
	  "    remote-host    host to connect to when relaying connection\n"
	  "    remote-port    port to connect to when relaying connection\n"
	  "\n"
	  "Example: tcpxd 110 pop.example.com 110\n"
	  "         relays POP3 connections to another host\n"
	  "\n"
	  "Example: tcpxd 8023 localhost 23\n"
	  "         redirects connections on port 8023 to port 23\n" );
}

int main (int argc, char *argv[])
{
					/* command line arguments	*/
  short int arg_bound_port = 0;		/* port number to listen on	*/
  char *arg_bound_address = NULL;	/* interface addr to listen on  */
  char *arg_remote_address = NULL;	/* remote host to connect to	*/
  short int arg_remote_port = 0;	/* remote port to connect to	*/
  short int arg_local_port = 0;		/* local source port to use	*/
  int arg_once = 0;			/* do only one session?		*/
  int arg_band = 0;			/* bandwidth limit bytes/second	*/
  char *arg_input = NULL;		/* file name of services	*/
  time_t arg_timeout = 0;		/* time to wait for connect     */

  int flag_once = 0;			/* got a session already?	*/

  int i;
  int status;
  
  struct service *service;

  FD_ZERO(&sr);
  FD_ZERO(&sw);
  FD_ZERO(&se);

  services = NULL;
  connections = NULL;

  if (argc == 1) {
    usage();
    die();
  }

  /* process command line arguments */
  for(i = 1;i < argc;i++) {
    
    /* --input takes a parameter which is the file containing services */
    if (!strcmp(argv[i], "-i")||!strcmp(argv[i], "--input")) {
      if (i++ == argc) break;
      arg_input = argv[i];
      continue;
    }

    /* --bandwidth takes a parameter in bytes per second */
    if (!strcmp(argv[i], "-b")||!strcmp(argv[i], "--bandwidth")) {
      if (i++ == argc) break;
      arg_band = atoi(argv[i]);
      continue;
    }

    /* --timeout takes a parameter in seconds */
    if (!strcmp(argv[i], "-t")||!strcmp(argv[i], "--timeout")) {
      if (i++ == argc) break;
      arg_timeout = time(NULL) + atoi(argv[i]);
      continue;
    }

    /* --verbose increments the verbosity value, more messages */
    if (!strcmp(argv[i], "-v")||!strcmp(argv[i], "--verbose")) {
      verbose++;
      continue;
    }
    
    /* --version reports the version */
    if (!strcmp(argv[i], "-V")||!strcmp(argv[i], "--version")) {
      printf("%s\n", vcid);
      continue;
    }
    
    /* --once allows just one incoming connection and stops listening */
    if (!strcmp(argv[i], "--once")) {
      arg_once = 1;
      continue;
    }
    
    /* --foreground don't become a daemon but stay in the foreground */
    if (!strcmp(argv[i], "--foreground")) {
      arg_foreground = 1;
      continue;
    }
    
    /* --local takes a parameter to use as the local port number */
    if (!strcmp(argv[i], "--local")) {
      if (i++ == argc) break;
      arg_local_port = atoi(argv[i]);
      continue;
    }
    
    if (!strcmp(argv[i], "-h")||!strcmp(argv[i], "--help")) {
      usage();
      die();
    }
    
    if (arg_bound_port == 0) {
      get_address_port(argv[i], &arg_bound_address, &arg_bound_port);
      continue;
    }

    if (arg_remote_address == NULL) {
      get_address_port(argv[i], &arg_remote_address, &arg_remote_port);
      continue;
    }

    if (arg_remote_port == 0)    {
      arg_remote_port = atoi(argv[i]);
      continue;
    }
  }

  /* did user give us a file name to read for services */
  if (arg_input == NULL) {

    /* no, so add the command line service only */

    /* die if not given command line service description */
    if (arg_remote_port == 0 || arg_remote_address == NULL || 
	arg_bound_port == 0) {
      fprintf(stderr, "%s: insufficient parameters\n", argv[0]);
      usage();
      die();
    }

    service = malloc ( sizeof ( struct service ) );
    service->listen.sin_family = AF_INET;
    if (arg_bound_address != NULL)
      inet_aton(arg_bound_address, &service->listen.sin_addr);
    else
      service->listen.sin_addr.s_addr = INADDR_ANY;
    service->listen.sin_port = htons(arg_bound_port);
    service->local.sin_port = htons(arg_local_port);
    service->remote.sin_family = AF_INET;
    resolve(service, arg_remote_address, &service->remote);
    service->remote.sin_port = htons(arg_remote_port);
    service->remote_host = strdup(arg_remote_address);
    service->bandwidth = arg_band;
    service->listener = -1;
    service->next = services;
    service->prior = NULL;
    service->accepts = 0;
    services = service;
  } else {

    /* yes, so read the file */
    FILE *input;

    input = fopen(arg_input, "r");
    if (input == NULL) {
      perror(arg_input);
      die();
    }

    while(1) {
      char buffer[1024], *line, *token;
      
      line = fgets(buffer, 1024, input);
      if (line == NULL) break;

      /* ignore lines starting with comment characters */
      if (line[0] == '#') continue;
      if (line[0] == '!') continue;

      token = strtok(line, " ");
      if (token == NULL) continue;
      get_address_port(token, &arg_bound_address, &arg_bound_port);

      token = strtok(NULL, " ");
      if (token == NULL) continue;
      get_address_port(token, &arg_remote_address, &arg_remote_port);

      if (arg_remote_port == 0) {
        token = strtok(NULL, " ");
        if (token == NULL) continue;
        arg_remote_port = atoi(token);
      }

      service = malloc ( sizeof ( struct service ) );
      service->listen.sin_family = AF_INET;
      service->listen.sin_port = htons(arg_bound_port);
      if (arg_bound_address != NULL)
	inet_aton(arg_bound_address, &service->listen.sin_addr);
      else
	service->listen.sin_addr.s_addr = INADDR_ANY;
      service->local.sin_port = 0;
      service->remote.sin_family = AF_INET;
      resolve(service, arg_remote_address, &service->remote);
      service->remote.sin_port = htons(arg_remote_port);
      service->remote_host = strdup(arg_remote_address);
      service->bandwidth = 0;
      service->listener = -1;
      service->next = services;
      service->prior = NULL;
      service->accepts = 0;
      services = service;
    }

    fclose(input);
  }

  /* default is to fork and become a daemon */
  if (!arg_foreground) {
     if (verbose > 0)
	printf("Becoming a daemon. Messages will now go through syslogd.\n");
     openlog("tcpxd", LOG_PID, LOG_DAEMON);
     become_daemon();
  }

  /* disable the broken pipe signal */
  signal(SIGPIPE, SIG_IGN);
  
  /* for each service defined */
  for(service = services;service != NULL;service = service->next) {
    
    /* create the listening socket */
    service->listener = socket(AF_INET, SOCK_STREAM, 0);
    if (service->listener == -1) { lerror("socket"); die(); }
    
    /* set the socket to allow address re-use */
    {
      int option_value = 1;
      status = setsockopt(service->listener, SOL_SOCKET, SO_REUSEADDR, 
			  (char *) &option_value, sizeof(option_value));
      if (status < 0) { lerror("setsockopt"); die(); }
    }
    
    /* bind the socket to the specified port */
    {
      service->listen.sin_family = AF_INET;

      status = bind(service->listener,
		    (struct sockaddr *) &service->listen,
		    sizeof service->listen);
      if (status) { lerror("bind"); die(); }
      if (verbose > 1) 
	lprintf(LOG_INFO, "%d: bind(): listener socket %d bound to %s:%d\n", 
	       ntohs(service->listen.sin_port), service->listener, 
	       inet_ntoa(service->listen.sin_addr), 
	       ntohs(service->listen.sin_port));
    }
    
    /* start listening for connections */
    status = listen(service->listener, 10);
    if (status) { lerror("listen"); die(); }
    if (verbose > 0)
      lprintf(LOG_INFO, 
	      "%d: listen(): listener socket %d waiting for connection\n",
	      ntohs(service->listen.sin_port), service->listener);

    /* make a note that we need to watch this file descriptor */
    FD_SET(service->listener,&sr);
  }
  
  /* main loop, exits only on error or if --once is used */
  while(1) {
    struct connection *connection;
    struct timeval timeout;
  
    /* copy our file descriptor masks to use in select() call */
    memcpy(&or,&sr,sizeof(fd_set));
    memcpy(&ow,&sw,sizeof(fd_set));
    memcpy(&oe,&se,sizeof(fd_set));
    
#ifdef FEATURE_FILE_DESCRIPTOR_DEBUG
    if (verbose > 2) dump_fd_before_select();
#endif /* FEATURE_FILE_DESCRIPTOR_DEBUG */

    timeout.tv_sec = 1;
    timeout.tv_usec = 0;

    /* wait for activity on the current file descriptors */
    status = select(FD_SETSIZE,&or,&ow,NULL,(arg_timeout) ? &timeout : NULL);
    if (status < 0) { lerror("select"); die(); }

#ifdef FEATURE_FILE_DESCRIPTOR_DEBUG
    if (verbose > 2) dump_fd_after_select();
#endif /* FEATURE_FILE_DESCRIPTOR_DEBUG */
    
    if (status == 0) {
        /* timeout: check, if time is up */
        if (connections || arg_timeout > time(NULL)) {
            /* have open connections or times not up */
            continue;
        }

        if (verbose > 0) {
            lprintf(LOG_INFO, "%d.%d: timeout waiting for connection, shutting down.\n", 0, 0); 
        }
        break;
    }

    /* for each defined service ... */
    for(service = services;service != NULL;service = service->next) {
      
      /* is the listening socket ready for reading? */
      if (FD_ISSET(service->listener,&or)) {

	/* yes, it is, so a connection has arrived */
	struct connection *connection;
	int length = sizeof(connection->peer);
	
	/* allocate heap for a connection structure and accept it */
	connection = malloc(sizeof(struct connection));
	connection->incoming = accept(service->listener,
				      (struct sockaddr *) &connection->peer, 
				      &length);
	if (connection->incoming == -1) { 
	  lerror("accept");
	  free(connection);
	  continue;
	}
	
	service->accepts++;
	connection->number = service->accepts;

	if (verbose > 0)
	  lprintf(LOG_INFO, 
		  "%d.%d: accept(): incoming socket %d accepted from %s:%d\n", 
		  ntohs(service->listen.sin_port), connection->number, 
		  connection->incoming, inet_ntoa(connection->peer.sin_addr), 
		  ntohs(connection->peer.sin_port));
	
	/* 
	 * Set the socket to disable the Nagle algorithm, trust the other ends
	 * to use Nagle if required ... thus we don't almalgamate packets but
	 * rather pass them right through at the same rate.  Useful for X.
	 */
	{
	  int option_value = 1;
	  status = setsockopt(connection->incoming, IPPROTO_TCP, TCP_NODELAY, 
			      (char *) &option_value, sizeof(option_value));
	  if (status < 0) { lerror("setsockopt"); }
	}
      
      retry:
	/*
	 * Create a socket for the outgoing link to the remote host.
	 * If this fails, drop the incoming connection.
	 */
	connection->outgoing = socket(AF_INET, SOCK_STREAM, 0);
	if (connection->outgoing == -1) { 
	  lerror("socket"); 
	  close(connection->incoming);
	  free(connection);
	  continue;
	}
	
	/*
	 * Set the SO_REUSEADDR bit for the outgoing socket, so that the
	 * address combination (IP & port) can be reused by another
	 * connection quickly.
	 */
	{
	  int option_value = 1;
	  status = setsockopt(connection->outgoing, SOL_SOCKET, SO_REUSEADDR, 
			      (char *) &option_value, sizeof(option_value));
	  if (status < 0) { lerror("setsockopt"); }
	}
	
	if (service->bandwidth != 0) {
	  int option_value = service->bandwidth*10;
	  
	  status = setsockopt
	    (
	     connection->outgoing,    /* socket        */
	     SOL_SOCKET,              /* level         */
	     SO_RCVBUF,               /* option name   */
	     (char *) &option_value,  /* option value  */
	     sizeof(option_value)     /* option length */
	     );
	  
	  if (status < 0) { lerror("setsockopt (SO_RCVBUF)"); }
	} else {
	  int option_value = 1;
	  
	  status = setsockopt(connection->outgoing, IPPROTO_TCP, TCP_NODELAY, 
			      (char *) &option_value, sizeof(option_value));
	  if (status < 0) { lerror("setsockopt"); }
	}
	
	/*
	 * Bind to a specific port at this end of the outgoing connection?
	 */
	if (service->local.sin_port != 0) {
	  service->local.sin_family = AF_INET;
	  memset((char *) &service->local.sin_addr, 0, sizeof(struct in_addr));
	  status = bind(connection->outgoing, 
			(struct sockaddr *)&service->local, 
			sizeof(struct sockaddr));
	  if (status < 0) { lerror("bind"); }
	  
	  if (verbose > 1) 
	    lprintf(LOG_INFO, 
		    "%d.%d: bind(): outgoing socket %d bound to %s:%d\n",
		    ntohs(service->listen.sin_port), connection->number, 
		    connection->outgoing, inet_ntoa(service->local.sin_addr), 
		    ntohs(service->local.sin_port));
	}

	unblock(connection->outgoing);

	if (verbose > 1) 
	  lprintf(LOG_INFO, 
		  "%d.%d: connect(): outgoing socket %d connecting to %s:%d\n",
		  ntohs(service->listen.sin_port), connection->number,
		  connection->outgoing, inet_ntoa(service->remote.sin_addr), 
		  ntohs(service->remote.sin_port));
	
	/*
	 * Start a connection to the remote host.  Note that on some
	 * platforms (notably Cygnus Win32) this call is synchronous,
	 * despite setting the socket non-blocking.
	 */
	status = connect(connection->outgoing, 
			 (struct sockaddr *) &service->remote, 
			 sizeof(service->remote));
	if (status) {
	  if (errno != EINPROGRESS) {
	    if (errno != EADDRINUSE) { 
	      lerror("connect");
	      close(connection->incoming);
	      close(connection->outgoing);
	      free(connection);
	      continue;
	    }
	    service->remote.sin_port = 
	      htons(ntohs(service->remote.sin_port)+1);
	    close(connection->outgoing);
	    lprintf(LOG_INFO, "address in use, retrying\n");
	    goto retry;
	  }
	}
	
	connection->connected = 0;
	connection->fast = 0;
	connection->constipate = 0;
	connection->service = service;
	connection->total = 0;
	connection->allow = service->bandwidth;
	if (connection->allow == 0) connection->allow = MAXCHUNK;
	connection->start = time(NULL);
	connection->now = connection->start;

	/* add to start of connection chain */
	connection->next = connections;
	if (connections != NULL) connection->next->prior = connection;
	connection->prior = NULL;
	connections = connection;
	
	/* begin by waiting for the sockets to be writeable */
	FD_SET(connection->incoming,&sw);
	FD_SET(connection->outgoing,&sw);
      }
    }
    
    /* from this point the file descriptor bits dance around in an amusingly 
       complex manner; first we set the bit to indicate we want to be told
       when the socket is writeable, and only when it is do we dare ask to be
       told that the peer socket is readable, and once both of those have 
       happened we can transfer data without stalling. */  
    
    /* process each active connection */
    for(connection = connections;connection != NULL;) {
      int bytes;
      
      /* if the outgoing connection is now writeable */
      if (FD_ISSET(connection->outgoing,&ow)) {

	/* check result of an asynchronous connection? */
	if (connection->connected == 0) {
	  int deferred_errno, length;
	  struct service *service = connection->service;

	  length = sizeof(deferred_errno);
	  if (getsockopt(connection->outgoing, SOL_SOCKET, SO_ERROR, 
			 &deferred_errno, &length) < 0) deferred_errno = errno;
	  block(connection->outgoing);

	  if (deferred_errno == 0) {

	    if (verbose > 0) 
	      lprintf(LOG_INFO, "%d.%d: connect(): outgoing socket %d "
		      "connected to %s:%d\n",
		      ntohs(service->listen.sin_port), connection->number, 
		      connection->outgoing, service->remote_host, 
		      ntohs(service->remote.sin_port));
	    
	    connection->connected++;

	  } else {
	    /* connection failed! */
	    lprintf(LOG_WARNING, "%d.%d: connect(): failed, to %s:%d, %s\n", 
		    ntohs(service->listen.sin_port), connection->number,
		    service->remote_host, ntohs(service->remote.sin_port),
		    strerror(deferred_errno));
	    /* simulate close of both ends */
	    connection->fast++;
	    connection->constipate++;
	    /* ignore this connection for writeability test now */
	    FD_CLR(connection->outgoing,&sw);
	    FD_CLR(connection->incoming,&sw);
	    FD_CLR(connection->outgoing,&sr);
	    FD_CLR(connection->incoming,&sr);

	    FD_CLR(connection->outgoing,&ow);
	    FD_CLR(connection->incoming,&ow);
	    FD_CLR(connection->outgoing,&or);
	    FD_CLR(connection->incoming,&or);
	  }
	} else {
	  /* start looking for incoming readable data */
	  FD_SET(connection->incoming,&sr);
	  /* and stop looking for outgoing writeability */
	  FD_CLR(connection->outgoing,&sw);
	}
      }
      
      /* if the incoming connection is now writeable */
      if (FD_ISSET(connection->incoming,&ow)) {
	/* start looking for outgoing readable data */
	FD_SET(connection->outgoing,&sr);
	/* and stop looking for incoming writeability */
	FD_CLR(connection->incoming,&sw);
      }
      
      /* if there is outgoing readable data */
      if (FD_ISSET(connection->outgoing,&or)) {
	/* transfer it */
	bytes = transfer(connection->incoming, connection->outgoing, 
			 connection->allow);
	if (bytes == 0) {
	  if (verbose > 1) 
	    lprintf(LOG_INFO, "%d.%d: recv(): outgoing socket %d "
		    "connection closed\n", 
		    ntohs(connection->service->listen.sin_port), 
		    connection->number, connection->outgoing);
	  connection->constipate++;
	  FD_CLR(connection->outgoing,&sr);
	  shutdown(connection->incoming,1);
	  shutdown(connection->outgoing,0);
	} else {
	  /* stop looking for readable data */
	  FD_CLR(connection->outgoing,&sr);
	  /* wait for ability to write again */
	  FD_SET(connection->incoming,&sw);
	}
	connection->total += bytes;
      }
      
      /* if there is incoming readable data */
      if (FD_ISSET(connection->incoming,&or)) {
	bytes = transfer(connection->outgoing, connection->incoming, 
			 connection->allow);
	if (bytes == 0) {
	  if (verbose > 1) 
	    lprintf(LOG_INFO, "%d.%d: recv(): incoming socket %d "
		    "connection closed\n",
		    ntohs(connection->service->listen.sin_port), 
		    connection->number, connection->incoming);
	  connection->fast++;
	  FD_CLR(connection->incoming,&sr);
	  shutdown(connection->outgoing,1);
	  shutdown(connection->incoming,0);
	} else {
	  /* stop looking for readable data */
	  FD_CLR(connection->incoming,&sr);
	  /* wait for ability to write again */
	  FD_SET(connection->outgoing,&sw);
	}
	connection->total += bytes;
      }
      
      /* bandwidth support temporarily disabled until i rework it */

      /*
	if (connection->service->bandwidth != 0) {
	int actual;
	
	connection->now = time(NULL);
	actual = (total/(now-start+1));
	connection->allow = (connection->service->bandwidth-actual)*5+connection->service->bandwidth;
	lprintf(LOG_INFO, "interim bandwidth %d bytes per second\n", actual);
	if (connection->allow < 1) connection->allow = 1;
	if (connection->allow > MAXCHUNK) connection->allow = MAXCHUNK;
	if (actual > connection->service->bandwidth/2) sleep(1);
	}
      */
      
      if (connection->fast && connection->constipate) {
	struct connection *waste;
	
	if (verbose > 0) 
	  lprintf(LOG_INFO, "%d.%d: connection is closed\n", 
		  ntohs(connection->service->listen.sin_port), 
		  connection->number );
	
	if (verbose > 2) 
	  if (connection->service->bandwidth != 0)
	    lprintf(LOG_INFO,
		    "%d.%d: full run bandwidth %d bytes per second\n",
		    ntohs(connection->service->listen.sin_port), 
		    connection->number, 
		    (int)(connection->total/(connection->now-connection->start+1)));
	
	status = shutdown(connection->incoming, 2);
	if (status == -1) { 
	  if (errno != ENOTCONN) lerror("shutdown(): incoming");
	}
	status = shutdown(connection->outgoing, 2);
	if (status == -1) { 
	  if (errno != ENOTCONN) lerror("shutdown(): outgoing");
	}

	status = close(connection->incoming);
	if (status) lerror("close(): incoming");
	status = close(connection->outgoing);
	if (status) lerror("close(): outgoing");
	
	/* delete connection from list */
	if (connection->prior != NULL) {
	  connection->prior->next = connection->next;
	} else {
	  connections = connection->next;
	}
	
	if (connection->next != NULL) {
	  connection->next->prior = connection->prior;
	}
	
	/* gymnastics for freeing */
	waste = connection;
	connection = connection->next;
	free(waste);

	flag_once = 1;

      } else {
	connection = connection->next;
      }
    }

    if (!connections && arg_once && flag_once) {
      if (verbose > 0) {
	lprintf(LOG_INFO, 
		"%d.%d: Had one connection. Shutting down.\n", 0, 0); 
      }
      
      break;
    }
    
  } /* while(1) */
  exit(EXIT_SUCCESS);
}
