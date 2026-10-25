/* Prospero AutoDrive - Spike S1: port probe
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Question answered by this spike: can a payload bind TCP 445 (the SMB
 * port Windows uses by default) and the fallback port 4445, or is 445
 * already taken by something on the console?
 *
 * Every probe port answers plain HTTP so that the built-in curl.exe on
 * Windows can read the result, including the reason a bind failed.
 * The probe writes no files. It exits after RUN_SECONDS or on GET /quit.
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>

#define RUN_SECONDS  600
#define CONTROL_PORT 4445

static const unsigned short PORTS[] = {445, CONTROL_PORT};
#define NPORTS ((int)(sizeof PORTS / sizeof PORTS[0]))

typedef struct probe_port {
  unsigned short port;
  int fd;
  int bind_errno;
} probe_port_t;


#ifdef __SCE__
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);
#endif


static void
notify(const char *fmt, ...) {
  char msg[1024];
  va_list args;

  va_start(args, fmt);
  vsnprintf(msg, sizeof msg, fmt, args);
  va_end(args);

#ifdef __SCE__
  notify_request_t req;
  memset(&req, 0, sizeof req);
  snprintf(req.message, sizeof req.message, "%s", msg);
  sceKernelSendNotificationRequest(0, &req, sizeof req, 0);
#endif
  printf("%s\n", msg);
  fflush(stdout);
}


/* Ask an earlier probe instance (if any) to exit, so that a second start
 * does not misreport its own predecessor as a foreign service on 445. */
static void
stop_previous_instance(void) {
  static const char req[] = "GET /quit HTTP/1.0\r\n\r\n";
  struct sockaddr_in addr;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return;
  }
  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONTROL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if(connect(fd, (struct sockaddr*)&addr, sizeof addr) == 0) {
    if(send(fd, req, sizeof req - 1, 0) > 0) {
      sleep(2);
    }
  }
  close(fd);
}


static void
open_port(probe_port_t *p) {
  struct sockaddr_in addr;
  int one = 1;

  p->fd = -1;
  p->bind_errno = 0;

  if((p->fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    p->bind_errno = errno;
    return;
  }
  setsockopt(p->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(p->port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(p->fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     listen(p->fd, 8) != 0) {
    p->bind_errno = errno;
    close(p->fd);
    p->fd = -1;
  }
}


static int
format_report(char *buf, size_t size, const probe_port_t *ports,
              unsigned short via_port, long uptime) {
  int n = snprintf(buf, size, "PROSPERO-S1 OK\nconnected_port=%u\n", via_port);

  for(int i=0; i<NPORTS && n>0 && (size_t)n<size; i++) {
    if(ports[i].fd >= 0) {
      n += snprintf(buf+n, size-n, "bind %u: OK\n", ports[i].port);
    } else {
      n += snprintf(buf+n, size-n, "bind %u: FAIL errno=%d (%s)\n",
                    ports[i].port, ports[i].bind_errno,
                    strerror(ports[i].bind_errno));
    }
  }
  if(n>0 && (size_t)n<size) {
    n += snprintf(buf+n, size-n, "uid=%d euid=%d\nuptime_s=%ld\n",
                  (int)getuid(), (int)geteuid(), uptime);
  }
  return n;
}


/* Serve one HTTP request. Returns 1 if the client asked us to quit. */
static int
serve_client(int fd, const probe_port_t *ports, unsigned short via_port,
             long uptime) {
  struct timeval tv = {2, 0};
  char req[1024];
  char body[1024];
  char head[128];
  ssize_t len;
  int quit;
  int n;

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  len = recv(fd, req, sizeof req - 1, 0);
  req[len > 0 ? len : 0] = '\0';
  quit = strncmp(req, "GET /quit", 9) == 0;

  if(quit) {
    n = snprintf(body, sizeof body, "PROSPERO-S1 QUIT\n");
  } else {
    n = format_report(body, sizeof body, ports, via_port, uptime);
  }
  if(n < 0) {
    n = 0;
  } else if((size_t)n >= sizeof body) {
    n = sizeof body - 1;
  }

  snprintf(head, sizeof head,
           "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
  send(fd, head, strlen(head), 0);
  send(fd, body, n, 0);
  close(fd);

  return quit;
}


int
main(void) {
  probe_port_t ports[NPORTS];
  char summary[256];
  time_t start = time(NULL);
  int open_count = 0;
  int quit = 0;
  int s = 0;

  signal(SIGPIPE, SIG_IGN);
  stop_previous_instance();

  for(int i=0; i<NPORTS; i++) {
    ports[i].port = PORTS[i];
    open_port(&ports[i]);
    if(ports[i].fd >= 0) {
      open_count++;
    }
    if(s >= 0 && (size_t)s < sizeof summary) {
      s += snprintf(summary+s, sizeof summary-s, "%s%u %s", i ? ", " : "",
                    ports[i].port, ports[i].fd >= 0 ? "OK" :
                    strerror(ports[i].bind_errno));
    }
  }

  notify("Prospero S1: %s (uid %d)", summary, (int)geteuid());
  if(!open_count) {
    return 1;
  }

  while(!quit && time(NULL) - start < RUN_SECONDS) {
    struct timeval tv = {1, 0};
    fd_set rfds;
    int maxfd = -1;

    FD_ZERO(&rfds);
    for(int i=0; i<NPORTS; i++) {
      if(ports[i].fd >= 0) {
        FD_SET(ports[i].fd, &rfds);
        if(ports[i].fd > maxfd) {
          maxfd = ports[i].fd;
        }
      }
    }

    if(select(maxfd + 1, &rfds, NULL, NULL, &tv) <= 0) {
      continue;
    }

    for(int i=0; i<NPORTS && !quit; i++) {
      int fd;

      if(ports[i].fd < 0 || !FD_ISSET(ports[i].fd, &rfds)) {
        continue;
      }
      if((fd=accept(ports[i].fd, NULL, NULL)) >= 0) {
        quit = serve_client(fd, ports, ports[i].port,
                            (long)(time(NULL) - start));
      }
    }
  }

  for(int i=0; i<NPORTS; i++) {
    if(ports[i].fd >= 0) {
      close(ports[i].fd);
    }
  }
  notify("Prospero S1: stopped");

  return 0;
}
