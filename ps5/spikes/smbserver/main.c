/* Prospero AutoDrive - Spikes S2 to S4 and S7: SMB server prototype
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Questions answered by these spikes: does the libsmb2 server engine with
 * our own file system backend serve a folder to the Windows 11 SMB client
 * (NTLMv2, signing required) correctly and completely - read-only (S2), and
 * read-write (S3)? Is it fast enough compared with ftpsrv (S4)? Can a
 * Windows program find it by UDP without a firewall prompt (S7, read-only
 * share s7 plus discovery.c on UDP 4446)?
 *
 * The payload creates a new test folder with deterministic files
 * (testdata.c) and shares exactly that folder as \\<ps5>\s2 (read-only),
 * \\<ps5>\s3 or \\<ps5>\s4 (read-write, S4 with an extra 256 MiB file),
 * depending on SPIKE at build time. Nothing outside that folder is read or
 * written.
 * User "prospero", random password shown as a PS5 notification. GET / on
 * TCP 4445 reports status and the requests the backend does not support,
 * GET /quit stops the server. It also stops by itself after RUN_SECONDS.
 * For the speed diagnosis (S2b), GET /profile/<a|b|c> switches socket and
 * dialect settings and GET /reset restarts the timing counters.
 * GET /clock reports what the console knows about its time zone.
 * GET /password hands the password out once, for the one-click test run
 * (tools\Run-Test.cmd); every later request is refused. The control port is
 * unauthenticated anyway, and the share holds test data only.
 *
 * Host build for the CI tests: sN-smbserver <base dir> <smb port> <password>
 */

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#ifdef __SCE__
#include <sys/sysctl.h>
#endif

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "backend.h"
#include "discovery.h"
#include "testdata.h"

#define CONTROL_PORT 4445
#define USER_NAME    "prospero"

/* S2 keeps profile 'a' as default, the setup it was measured with. S3 and
 * S4 start with 'b', the result of S2b. The S4 run takes about 30 minutes,
 * on a slow WLAN more than an hour. */
#if SPIKE == 7
#define NAME         "Prospero S7"
#define TAG          "PROSPERO-S7"
#define SHARE_NAME   "s7"
#define WRITABLE     0
#define PROFILE      'b'
#define RUN_SECONDS  1800
#define BIG_MIB      0
#define DISCOVERY    1
#elif SPIKE == 4
#define NAME         "Prospero S4"
#define TAG          "PROSPERO-S4"
#define SHARE_NAME   "s4"
#define WRITABLE     1
#define PROFILE      'b'
#define RUN_SECONDS  5400
#define BIG_MIB      256
#elif SPIKE == 3
#define NAME         "Prospero S3"
#define TAG          "PROSPERO-S3"
#define SHARE_NAME   "s3"
#define WRITABLE     1
#define PROFILE      'b'
#define RUN_SECONDS  1800
#define BIG_MIB      0
#else
#define NAME         "Prospero S2"
#define TAG          "PROSPERO-S2"
#define SHARE_NAME   "s2"
#define WRITABLE     0
#define PROFILE      'a'
#define RUN_SECONDS  900
#define BIG_MIB      0
#endif
#ifndef DISCOVERY
#define DISCOVERY    0
#endif


#ifdef __SCE__
typedef struct notify_request {
  char useless1[45];
  char message[3075];
} notify_request_t;

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

/* Exported by libkernel; the layout of its argument is not documented, so
 * it gets more room than a struct timezone needs. */
int sceKernelGettimezone(void *tz);
#endif


static struct smb2_server g_server;
static char g_folder[1024];
static time_t g_start;
static int g_ctl_fd = -1;
static int g_udp_fd = -1;
static uint16_t g_smb_port;
static unsigned g_connections;
static char g_password[32];
static int g_password_given;


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


/* Ask an earlier S1/S2 instance (if any) to exit, so that it frees 445. */
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


static int
open_control_port(void) {
  struct sockaddr_in addr;
  int one = 1;
  int fd;

  if((fd=socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    return -1;
  }
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

  memset(&addr, 0, sizeof addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(CONTROL_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);

  if(bind(fd, (struct sockaddr*)&addr, sizeof addr) != 0 ||
     listen(fd, 8) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}


#ifdef __SCE__
/* Password for the notification: 3 groups of 4, no look-alike characters. */
static void
make_password(char *out, size_t size) {
  static const char alphabet[] = "abcdefghjkmnpqrstuvwxyz23456789";
  size_t n = 0;

  for(int i=0; i<12 && n+2 < size; i++) {
    if(i && i % 4 == 0) {
      out[n++] = '-';
    }
    out[n++] = alphabet[arc4random_uniform(sizeof alphabet - 1)];
  }
  out[n] = '\0';
}
#endif


/* GET /clock: what the console knows about its time zone, to explain why
 * its UTC ran one hour ahead in S2b and S3. Only libkernel exports are
 * called, and Run-Test.cmd asks for it after the test, so a crash here
 * cannot cost a test run. */
static int
clock_report(char *body, size_t size) {
  struct timezone tz;
  struct timeval tv;
  int n;

  memset(&tz, 0, sizeof tz);
  gettimeofday(&tv, &tz);
  n = snprintf(body, size,
               TAG " CLOCK\nnow_s=%lld\ngettimeofday_minuteswest=%d\n"
               "gettimeofday_dsttime=%d\n",
               (long long)tv.tv_sec, tz.tz_minuteswest, tz.tz_dsttime);
#ifdef __SCE__
  {
    int ktz[8];
    int adj = 0;
    int wall = 0;
    size_t len;
    int rc;

    memset(ktz, 0, sizeof ktz);
    rc = sceKernelGettimezone(ktz);
    n += snprintf(body + n, size - n,
                  "sceKernelGettimezone=%d words=%d %d %d %d\n",
                  rc, ktz[0], ktz[1], ktz[2], ktz[3]);
    len = sizeof adj;
    rc = sysctlbyname("machdep.adjkerntz", &adj, &len, NULL, 0);
    n += snprintf(body + n, size - n, "machdep.adjkerntz=%d (rc %d)\n", adj, rc);
    len = sizeof wall;
    rc = sysctlbyname("machdep.wall_cmos_clock", &wall, &len, NULL, 0);
    n += snprintf(body + n, size - n, "machdep.wall_cmos_clock=%d (rc %d)\n", wall, rc);
  }
#endif
  return n;
}


/* Serve one HTTP request on the control port. */
static void
serve_control(int fd) {
  struct timeval tv = {2, 0};
  char req[1024];
  char body[8192];
  char head[128];
  ssize_t len;
  int n;

  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  len = recv(fd, req, sizeof req - 1, 0);
  req[len > 0 ? len : 0] = '\0';

  if(strncmp(req, "GET /quit", 9) == 0) {
    g_server.stop_requested = 1;
    n = snprintf(body, sizeof body, TAG " QUIT\n");
  } else if(strncmp(req, "GET /reset", 10) == 0) {
    backend_reset_stats();
    n = snprintf(body, sizeof body, TAG " RESET\n");
  } else if(strncmp(req, "GET /profile/", 13) == 0) {
    n = snprintf(body, sizeof body, TAG " %s\n",
                 backend_set_profile(req[13]) == 0 ? "PROFILE" : "UNKNOWN PROFILE");
  } else if(strncmp(req, "GET /clock", 10) == 0) {
    n = clock_report(body, sizeof body);
  } else if(strncmp(req, "GET /password", 13) == 0) {
    if(g_password_given) {
      n = snprintf(body, sizeof body, TAG " PASSWORD ALREADY GIVEN\n");
    } else {
      g_password_given = 1;
      n = snprintf(body, sizeof body, TAG " PASSWORD\npassword=%s\n",
                   g_password);
    }
  } else {
    /* The console's own clock, for comparison with a reference: in S2b
     * its UTC ran one hour ahead. No localtime_r() here: the first S3
     * build called it at this point and stopped answering on the console
     * (likely the cause, not confirmed), while gmtime_r() is proven. */
    time_t now = time(NULL);
    long up = (long)(now - g_start);
    char now_utc[32];
    struct tm tm;

    memset(&tm, 0, sizeof tm);
    gmtime_r(&now, &tm);
    strftime(now_utc, sizeof now_utc, "%Y-%m-%dT%H:%M:%SZ", &tm);
    n = snprintf(body, sizeof body,
                 TAG " OK\nfolder=%s\nshare=%s\nuser=%s\nwritable=%d\n"
                 "uptime_s=%ld\nremaining_s=%ld\nconnections=%u\n"
                 "password_given=%d\nnow_utc=%s\n",
                 g_folder, SHARE_NAME, USER_NAME, WRITABLE, up,
                 (long)RUN_SECONDS - up, g_connections, g_password_given,
                 now_utc);
    if(n > 0 && (size_t)n < sizeof body) {
      n += backend_status(body + n, sizeof body - n);
    }
    if(g_udp_fd >= 0 && n > 0 && (size_t)n < sizeof body) {
      n += discovery_status(body + n, sizeof body - n);
    }
  }
  if(n < 0) {
    n = 0;
  } else if((size_t)n >= sizeof body) {
    n = sizeof body - 1;
  }

  snprintf(head, sizeof head,
           "HTTP/1.0 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n"
           "Content-Length: %d\r\nConnection: close\r\n\r\n", n);
  send(fd, head, strlen(head), 0);
  send(fd, body, n, 0);
  close(fd);
}


/* Hooks into smb2_serve_port(): it runs them at least every 100 ms. */
static void
extra_fdset(struct smb2_server *srv, fd_set *rfds, fd_set *wfds, int *maxfd) {
  (void)wfds;
  backend_listener(srv->fd);
  FD_SET(g_ctl_fd, rfds);
  if(g_ctl_fd > *maxfd) {
    *maxfd = g_ctl_fd;
  }
  if(g_udp_fd >= 0) {
    FD_SET(g_udp_fd, rfds);
    if(g_udp_fd > *maxfd) {
      *maxfd = g_udp_fd;
    }
  }
}


static void
extra_service(struct smb2_server *srv, fd_set *rfds, fd_set *wfds) {
  (void)wfds;
  backend_flush();
  if(FD_ISSET(g_ctl_fd, rfds)) {
    int fd = accept(g_ctl_fd, NULL, NULL);
    if(fd >= 0) {
      serve_control(fd);
    }
  }
  if(g_udp_fd >= 0 && FD_ISSET(g_udp_fd, rfds)) {
    discovery_serve(g_udp_fd, NAME, g_smb_port);
  }
  if(time(NULL) - g_start >= RUN_SECONDS) {
    srv->stop_requested = 1;
  }
}


static void
on_new_client(struct smb2_context *smb2, void *cb_data) {
  (void)cb_data;
  g_connections++;
  backend_attach(smb2);
}


int
main(int argc, char **argv) {
  const char *base = "/data";
  uint16_t port = 445;
  int rc;

#ifndef __SCE__
  if(argc != 4) {
    fprintf(stderr, "usage: %s <base dir> <smb port> <password>\n", argv[0]);
    return 2;
  }
  base = argv[1];
  port = (uint16_t)atoi(argv[2]);
  snprintf(g_password, sizeof g_password, "%s", argv[3]);
#else
  (void)argc;
  (void)argv;
  make_password(g_password, sizeof g_password);
#endif

  signal(SIGPIPE, SIG_IGN);
  g_start = time(NULL);
  stop_previous_instance();

  if((g_ctl_fd=open_control_port()) < 0) {
    notify(NAME ": port %d is in use: %s", CONTROL_PORT, strerror(errno));
    return 1;
  }

  notify(NAME ": lege Testordner an ...");
  if((rc=testdata_create(base, g_folder, sizeof g_folder, BIG_MIB))) {
    notify(NAME ": FEHLER Testordner: %s (%s)", strerror(-rc), g_folder);
    return 1;
  }
  if((rc=backend_init(g_folder, SHARE_NAME, USER_NAME, g_password,
                      WRITABLE, PROFILE))) {
    notify(NAME ": ERROR: cannot set up the share: %s", strerror(-rc));
    return 1;
  }

  if(DISCOVERY) {
    if((g_udp_fd=discovery_open(DISCOVERY_PORT)) < 0) {
      notify(NAME ": UDP port %d is in use: %s", DISCOVERY_PORT, strerror(-g_udp_fd));
      return 1;
    }
  }

  snprintf(g_server.hostname, sizeof g_server.hostname, "%s", TAG);
  g_smb_port = port;
  g_server.port = port;
  g_server.signing_enabled = 1;
  g_server.allow_anonymous = 0;
  g_server.handlers = &backend_handlers;
  g_server.extra_fdset = extra_fdset;
  g_server.extra_service = extra_service;

  notify(NAME " ready: share %s (%s), user %s, password %s (%d min)",
         SHARE_NAME, WRITABLE ? "read and write" : "read only",
         USER_NAME, g_password, RUN_SECONDS / 60);
  rc = smb2_serve_port(&g_server, 8, on_new_client, NULL);
  if(rc < 0) {
    notify(NAME ": SMB port %u is not available (%d)", port, rc);
  } else {
    notify(NAME ": stopped (%u SMB connections served)", g_connections);
  }

  close(g_ctl_fd);
  if(g_udp_fd >= 0) {
    close(g_udp_fd);
  }
  return rc < 0 ? 1 : 0;
}
