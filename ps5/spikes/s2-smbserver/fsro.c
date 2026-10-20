/* Prospero AutoDrive - Spike S2: read-only file system backend
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Serves one directory read-only through the libsmb2 server engine. The
 * engine handles negotiate, NTLMv2 session setup and signing; everything
 * that touches files is here, encoded as described in MS-SMB2 and MS-FSCC.
 *
 * Workarounds for the pinned libsmb2 (7e4ff97):
 *  - Passthrough mode on every connection. Directory listings and all
 *    QUERY_INFO data are encoded here: the library's directory encoder
 *    writes absolute NextEntryOffset values, and its QUERY_INFO encoders
 *    only cover some classes.
 *  - A handler error (< 0) always becomes STATUS_NOT_IMPLEMENTED, so every
 *    other status is sent as a self-queued error reply.
 *  - The library still reads a QUERY_DIRECTORY request after the handler
 *    returned, so its error replies wait for fsro_flush().
 *  - Compound requests arrive one at a time. Related operations carry the
 *    all-0xFF file id and use the last CREATE result of the connection.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>

#include "fsro.h"

/* Access mask bits (MS-DTYP 2.4.3, MS-SMB2 2.2.13.1). */
#define FILE_READ_DATA         0x00000001
#define FILE_WRITE_DATA        0x00000002
#define FILE_APPEND_DATA       0x00000004
#define FILE_WRITE_EA          0x00000010
#define FILE_DELETE_CHILD      0x00000040
#define FILE_WRITE_ATTRIBUTES  0x00000100
#define DELETE                 0x00010000
#define WRITE_DAC              0x00040000
#define WRITE_OWNER            0x00080000
#define ACCESS_SYSTEM_SECURITY 0x01000000
#define GENERIC_ALL            0x10000000
#define GENERIC_WRITE          0x40000000
#define WRITE_ACCESS (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |   \
                      FILE_DELETE_CHILD | FILE_WRITE_ATTRIBUTES | DELETE |   \
                      WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY |     \
                      GENERIC_ALL | GENERIC_WRITE)
/* FILE_READ_DATA, READ_EA, EXECUTE, READ_ATTRIBUTES, READ_CONTROL, SYNCHRONIZE */
#define READ_ACCESS            0x001200A9
#define IPC_ACCESS             0x001F01FF

/* CREATE dispositions and actions (MS-SMB2 2.2.13, 2.2.14). */
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OPENED       1

/* MS-FSCC 2.5.1 */
#define FILE_CASE_PRESERVED_NAMES 0x00000002
#define FILE_UNICODE_ON_DISK      0x00000004
#define FILE_READ_ONLY_VOLUME     0x00080000

#define STATUS_OBJECT_NAME_COLLISION 0xC0000035
#define STATUS_NETWORK_NAME_DELETED  0xC00000C9

#define FSCTL_DFS_GET_REFERRALS    0x00060194
#define FSCTL_DFS_GET_REFERRALS_EX 0x000601B0

#define MAX_TREES    8
#define MAX_DEFERRED 64
#define MAX_NOTES    32
#define MAX_ERRORS   4
#define PATH_SIZE    1024

enum tree_kind { TREE_NONE, TREE_IPC, TREE_DISK };

/* Socket and dialect variants for the S2b speed diagnosis. 'a' must stay
 * first and untouched: it is the setup S2 was measured with. */
typedef struct profile {
  char name;
  int sockbuf;                          /* SO_SNDBUF/SO_RCVBUF, 0 = default */
  int nodelay;                          /* TCP_NODELAY, -1 = as accepted */
  enum smb2_negotiate_version version;
  const char *what;
} profile_t;

static const profile_t PROFILES[] = {
  {'a', 0,       -1, SMB2_VERSION_ANY,  "socket defaults as in S2"},
  {'b', 1 << 20,  1, SMB2_VERSION_ANY,  "1 MiB socket buffers, TCP_NODELAY (as ftpsrv)"},
  {'c', 1 << 20,  1, SMB2_VERSION_0210, "as b, but SMB 2.1 (HMAC-SHA256 signing, no AES-CMAC)"},
};

typedef struct dir_entry {
  char *name;
  struct stat st;
} dir_entry_t;

typedef struct handle {
  struct handle *next;
  uint8_t fid[SMB2_FD_SIZE];
  uint32_t tree_id;
  char path[PATH_SIZE];   /* host path */
  char rel[PATH_SIZE];    /* share-relative, '\'-separated, "" = root */
  int fd;                 /* files only, -1 for directories */
  int is_dir;
  dir_entry_t *entries;   /* directory listing snapshot */
  size_t nentries;
  size_t pos;
  int listed;
  uint8_t *dir_buf;       /* last QUERY_DIRECTORY output */
} handle_t;

typedef struct conn {
  struct conn *next;
  struct smb2_context *smb2;
  struct {
    uint32_t id;
    int kind;
  } trees[MAX_TREES];
  uint32_t next_tree_id;
  uint64_t next_fid;
  handle_t *handles;
  uint8_t last_fid[SMB2_FD_SIZE];
  uint32_t last_create_status;
} conn_t;

typedef struct deferred {
  struct smb2_context *smb2;
  uint64_t message_id;
  uint8_t command;
  uint32_t status;
} deferred_t;

static struct {
  char root[PATH_SIZE];
  char share[64];
  char user[64];
  char password[128];
  struct stat root_st;

  deferred_t deferred[MAX_DEFERRED];
  int ndeferred;

  /* Create contexts of the reply in progress; the library copies them
   * right after create_cmd() returns. */
  uint8_t create_ctx[128];

  unsigned auth_attempts;
  unsigned sessions;
  unsigned creates;
  unsigned creates_ok;
  unsigned dir_queries;
  unsigned long long reads;
  unsigned long long bytes_read;
  unsigned denied_writes;
  unsigned lost_replies;

  struct {
    char what[48];
    unsigned count;
  } notes[MAX_NOTES];

  /* Why the last connections ended, as libsmb2 reports it. */
  char last_errors[MAX_ERRORS][120];
  unsigned nerrors;

  conn_t *conns;
  const profile_t *profile;
  int listener_fd;
  const profile_t *listener_profile;
  uint16_t last_dialect;

  /* Since the last fsro_reset_stats(), for the speed diagnosis. */
  uint64_t stats_start_ns;
  struct rusage ru_start;
  uint64_t read_ns;                     /* time spent in pread() */
  uint64_t first_read_ns;
  uint64_t last_read_ns;
} g;


/* ---------------------------------------------------------------- helpers */

static void
put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}


static void
put32(uint8_t *p, uint32_t v) {
  put16(p, (uint16_t)v);
  put16(p + 2, (uint16_t)(v >> 16));
}


static void
put64(uint8_t *p, uint64_t v) {
  put32(p, (uint32_t)v);
  put32(p + 4, (uint32_t)(v >> 32));
}


static uint32_t
get32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}


static uint16_t
get16(const uint8_t *p) {
  return (uint16_t)(p[0] | p[1] << 8);
}


/* Counts requests the backend does not support, for the status page. */
static void __attribute__((format(printf, 1, 2)))
note(const char *fmt, ...) {
  char what[48];
  va_list args;
  int i;

  va_start(args, fmt);
  vsnprintf(what, sizeof what, fmt, args);
  va_end(args);

  for(i=0; i<MAX_NOTES && g.notes[i].count; i++) {
    if(!strcmp(g.notes[i].what, what)) {
      g.notes[i].count++;
      return;
    }
  }
  if(i < MAX_NOTES) {
    memcpy(g.notes[i].what, what, sizeof what);
    g.notes[i].count = 1;
  }
}


/* Sends an error reply with exactly this status. Returns the value a
 * handler must return after queuing its own reply. */
static int
reply_status(struct smb2_context *smb2, uint8_t command, uint32_t status) {
  struct smb2_error_reply err;
  struct smb2_pdu *pdu;

  memset(&err, 0, sizeof err);
  pdu = smb2_cmd_error_reply_async(smb2, &err, command, (int)status, NULL, NULL);
  if(!pdu) {
    g.lost_replies++;
    return -1;
  }
  smb2_set_pdu_message_id(smb2, pdu, smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}


/* Same, but sent from fsro_flush(), after the library is done with the
 * request. */
static int
defer_status(struct smb2_context *smb2, uint8_t command, uint32_t status) {
  deferred_t *d;

  if(g.ndeferred == MAX_DEFERRED) {
    g.lost_replies++;
    return -1;
  }
  d = &g.deferred[g.ndeferred++];
  d->smb2 = smb2;
  d->message_id = smb2_get_last_request_message_id(smb2);
  d->command = command;
  d->status = status;
  return 1;
}


void
fsro_flush(void) {
  for(int i=0; i<g.ndeferred; i++) {
    deferred_t *d = &g.deferred[i];
    struct smb2_error_reply err;
    struct smb2_pdu *pdu;

    memset(&err, 0, sizeof err);
    pdu = smb2_cmd_error_reply_async(d->smb2, &err, d->command,
                                     (int)d->status, NULL, NULL);
    if(!pdu) {
      g.lost_replies++;
      continue;
    }
    smb2_set_pdu_message_id(d->smb2, pdu, d->message_id);
    smb2_queue_pdu(d->smb2, pdu);
  }
  g.ndeferred = 0;
}


static uint32_t
errno_status(int err) {
  switch(err) {
  case ENOENT:       return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
  case ENOTDIR:      return SMB2_STATUS_OBJECT_PATH_NOT_FOUND;
  case EACCES:
  case EPERM:        return SMB2_STATUS_ACCESS_DENIED;
  case ENAMETOOLONG: return SMB2_STATUS_OBJECT_NAME_INVALID;
  case EISDIR:       return SMB2_STATUS_FILE_IS_A_DIRECTORY;
  case ENOMEM:       return SMB2_STATUS_NO_MEMORY;
  case EMFILE:
  case ENFILE:       return SMB2_STATUS_INSUFFICIENT_RESOURCES;
  case EIO:          return SMB2_STATUS_IO_DEVICE_ERROR;
  default:
    note("errno %d", err);
    return SMB2_STATUS_INTERNAL_ERROR;
  }
}


static uint64_t
filetime(const struct timespec *ts) {
  return ((uint64_t)ts->tv_sec + 11644473600ULL) * 10000000ULL +
         (uint64_t)ts->tv_nsec / 100;
}


static uint32_t
attributes(const struct stat *st) {
  return S_ISDIR(st->st_mode) ? SMB2_FILE_ATTRIBUTE_DIRECTORY
                              : SMB2_FILE_ATTRIBUTE_ARCHIVE;
}


static uint64_t
alloc_size(const struct stat *st) {
  return S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_blocks * 512;
}


static uint64_t
end_of_file(const struct stat *st) {
  return S_ISDIR(st->st_mode) ? 0 : (uint64_t)st->st_size;
}


/* Creation time is not portable in POSIX; the modification time stands in. */
static void
put_times(uint8_t *p, const struct stat *st) {
  put64(p, filetime(&st->st_mtim));
  put64(p + 8, filetime(&st->st_atim));
  put64(p + 16, filetime(&st->st_mtim));
  put64(p + 24, filetime(&st->st_ctim));
}


/* ------------------------------------------------------- names and paths */

/* Next code point of a UTF-8 string; invalid bytes stand for themselves. */
static uint32_t
next_cp(const char **s) {
  const uint8_t *p = (const uint8_t *)*s;
  uint32_t cp;
  int n;

  if(p[0] < 0x80) {
    cp = p[0];
    n = 1;
  } else if((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
    cp = (p[0] & 0x1Fu) << 6 | (p[1] & 0x3Fu);
    n = 2;
  } else if((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
            (p[2] & 0xC0) == 0x80) {
    cp = (p[0] & 0x0Fu) << 12 | (p[1] & 0x3Fu) << 6 | (p[2] & 0x3Fu);
    n = 3;
  } else if((p[0] & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 &&
            (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
    cp = (p[0] & 0x07u) << 18 | (p[1] & 0x3Fu) << 12 |
         (p[2] & 0x3Fu) << 6 | (p[3] & 0x3Fu);
    n = 4;
  } else {
    cp = p[0];
    n = 1;
  }
  *s += n;
  return cp;
}


/* Simple case folding: ASCII, Latin-1, Latin Extended-A pairs, Greek and
 * Cyrillic. Enough for the names Windows users type. */
static uint32_t
fold(uint32_t cp) {
  if(cp >= 'A' && cp <= 'Z') {
    return cp + 32;
  }
  if(cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) {
    return cp + 32;
  }
  if(((cp >= 0x100 && cp <= 0x137) || (cp >= 0x14A && cp <= 0x177)) &&
     !(cp & 1)) {
    return cp + 1;
  }
  if(cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) {
    return cp + 32;
  }
  if(cp >= 0x410 && cp <= 0x42F) {
    return cp + 32;
  }
  if(cp >= 0x400 && cp <= 0x40F) {
    return cp + 80;
  }
  return cp;
}


static int
name_equal(const char *a, const char *b) {
  while(*a && *b) {
    if(fold(next_cp(&a)) != fold(next_cp(&b))) {
      return 0;
    }
  }
  return !*a && !*b;
}


/* Wildcard match, case-insensitive. '*' and DOS_STAR '<' match any run,
 * '?' and DOS_QM '>' one character, DOS_DOT '"' a dot. */
static int
wild_match(const char *pat, const char *name) {
  const char *star_pat = NULL;
  const char *star_name = NULL;

  while(*name) {
    const char *p = pat;
    const char *n = name;
    uint32_t pc = *p ? next_cp(&p) : 0;
    uint32_t nc = next_cp(&n);

    if(pc == '*' || pc == '<') {
      star_pat = p;
      star_name = name;
      pat = p;
      continue;
    }
    if(pc && (pc == '?' || pc == '>' || (pc == '"' && nc == '.') ||
              fold(pc) == fold(nc))) {
      pat = p;
      name = n;
      continue;
    }
    if(!star_pat) {
      return 0;
    }
    pat = star_pat;
    next_cp(&star_name);
    name = star_name;
  }
  while(*pat == '*' || *pat == '<' || *pat == '>' || *pat == '"') {
    pat++;
  }
  return !*pat;
}


/* UTF-8 to UTF-16LE. Returns the byte length or -1 if room is too small. */
static int
utf16(const char *s, uint8_t *out, size_t room) {
  size_t n = 0;

  while(*s) {
    uint32_t cp = next_cp(&s);

    if(cp >= 0x10000) {
      if(n + 4 > room) {
        return -1;
      }
      cp -= 0x10000;
      put16(out + n, (uint16_t)(0xD800 | (cp >> 10)));
      put16(out + n + 2, (uint16_t)(0xDC00 | (cp & 0x3FF)));
      n += 4;
    } else {
      if(n + 2 > room) {
        return -1;
      }
      put16(out + n, (uint16_t)cp);
      n += 2;
    }
  }
  return (int)n;
}


static int
valid_component(const char *c) {
  if(!*c || !strcmp(c, ".") || !strcmp(c, "..")) {
    return 0;
  }
  for(; *c; c++) {
    if((unsigned char)*c < 0x20 || strchr("/:*?\"<>|", *c)) {
      return 0;
    }
  }
  return 1;
}


/* Finds the on-disk spelling of name in dir, ignoring case. */
static int
find_ci(const char *dir, const char *name, char *out, size_t size) {
  struct dirent *e;
  DIR *d;
  int found = 0;

  if(!(d=opendir(dir))) {
    return 0;
  }
  while(!found && (e=readdir(d))) {
    if(name_equal(e->d_name, name)) {
      snprintf(out, size, "%s", e->d_name);
      found = 1;
    }
  }
  closedir(d);
  return found;
}


/* Maps an SMB name (share-relative, '\'-separated) to a host path below the
 * share root, component by component and case-insensitively. Names that
 * could leave the share are invalid. Returns 0 or an NTSTATUS. */
static uint32_t
resolve(const char *name, char *path, char *rel, struct stat *st) {
  char buf[PATH_SIZE];
  char *comp;
  char *rest;
  size_t len;

  snprintf(path, PATH_SIZE, "%s", g.root);
  rel[0] = '\0';
  if(stat(path, st) != 0) {
    return errno_status(errno);
  }
  if(!name) {
    return 0;
  }

  while(*name == '\\') {
    name++;
  }
  if(snprintf(buf, sizeof buf, "%s", name) >= (int)sizeof buf) {
    return SMB2_STATUS_OBJECT_NAME_INVALID;
  }
  len = strlen(buf);
  if(len >= 7 && !strcasecmp(buf + len - 7, "::$DATA")) {
    buf[len -= 7] = '\0';
  }
  while(len && buf[len - 1] == '\\') {
    buf[--len] = '\0';
  }

  for(comp=buf; *comp; comp=rest) {
    char actual[256];
    size_t plen = strlen(path);
    size_t rlen = strlen(rel);

    if((rest=strchr(comp, '\\'))) {
      *rest++ = '\0';
    } else {
      rest = comp + strlen(comp);
    }
    if(!valid_component(comp)) {
      return SMB2_STATUS_OBJECT_NAME_INVALID;
    }
    if(!S_ISDIR(st->st_mode)) {
      return SMB2_STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if(snprintf(path + plen, PATH_SIZE - plen, "/%s", comp) >= (int)(PATH_SIZE - plen)) {
      return SMB2_STATUS_OBJECT_NAME_INVALID;
    }
    if(stat(path, st) != 0) {
      if(errno != ENOENT) {
        return errno_status(errno);
      }
      path[plen] = '\0';
      if(!find_ci(path, comp, actual, sizeof actual)) {
        return *rest ? SMB2_STATUS_OBJECT_PATH_NOT_FOUND
                     : SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
      }
      if(snprintf(path + plen, PATH_SIZE - plen, "/%s", actual) >= (int)(PATH_SIZE - plen)) {
        return SMB2_STATUS_OBJECT_NAME_INVALID;
      }
      if(stat(path, st) != 0) {
        return errno_status(errno);
      }
      comp = actual;
    }
    if(snprintf(rel + rlen, PATH_SIZE - rlen, "%s%s", rlen ? "\\" : "", comp)
       >= (int)(PATH_SIZE - rlen)) {
      return SMB2_STATUS_OBJECT_NAME_INVALID;
    }
  }
  return 0;
}


/* --------------------------------------------------- connections, handles */

static int
tree_kind(conn_t *c, uint32_t *tree_id) {
  uint32_t id;

  if(smb2_get_tree_id_for_pdu(c->smb2, NULL, &id) != 0) {
    return TREE_NONE;
  }
  for(int i=0; i<MAX_TREES; i++) {
    if(c->trees[i].kind && c->trees[i].id == id) {
      if(tree_id) {
        *tree_id = id;
      }
      return c->trees[i].kind;
    }
  }
  return TREE_NONE;
}


static void
free_listing(handle_t *h) {
  for(size_t i=0; i<h->nentries; i++) {
    free(h->entries[i].name);
  }
  free(h->entries);
  h->entries = NULL;
  h->nentries = 0;
  h->pos = 0;
  h->listed = 0;
}


static void
free_handle(handle_t *h) {
  if(h->fd >= 0) {
    close(h->fd);
  }
  free_listing(h);
  free(h->dir_buf);
  free(h);
}


/* Related compound operations carry the all-0xFF id and refer to the last
 * CREATE; if that failed, they fail with its status. */
static handle_t *
lookup(conn_t *c, const uint8_t *fid, uint32_t *status) {
  if(!memcmp(fid, compound_file_id, SMB2_FD_SIZE)) {
    if(c->last_create_status) {
      *status = c->last_create_status;
      return NULL;
    }
    fid = c->last_fid;
  }
  for(handle_t *h=c->handles; h; h=h->next) {
    if(!memcmp(h->fid, fid, SMB2_FD_SIZE)) {
      return h;
    }
  }
  *status = SMB2_STATUS_FILE_CLOSED;
  return NULL;
}


static void
unlink_handle(conn_t *c, handle_t *h) {
  for(handle_t **pp=&c->handles; *pp; pp=&(*pp)->next) {
    if(*pp == h) {
      *pp = h->next;
      return;
    }
  }
}


void
fsro_attach(struct smb2_context *smb2) {
  const profile_t *p = g.profile;
  conn_t *c = calloc(1, sizeof *c);
  int fd = smb2_get_fd(smb2);
  int one = 1;

  smb2_set_version(smb2, p->version);
  smb2_set_passthrough(smb2, 1);
  setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
  if(p->sockbuf) {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &p->sockbuf, sizeof p->sockbuf);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &p->sockbuf, sizeof p->sockbuf);
  }
  if(p->nodelay >= 0) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &p->nodelay, sizeof p->nodelay);
  }
  if(c) {
    c->smb2 = smb2;
    c->last_create_status = SMB2_STATUS_FILE_CLOSED;
    c->next = g.conns;
    g.conns = c;
  }
  smb2_set_opaque(smb2, c);
}


/* Window scaling is agreed in the handshake, so the receive buffer has to be
 * set on the listening socket to count. Profile 'a' never touches it. */
void
fsro_listener(int fd) {
  const profile_t *p = g.profile;

  if(fd < 0 || (fd == g.listener_fd && p == g.listener_profile)) {
    return;
  }
  if(p->sockbuf) {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &p->sockbuf, sizeof p->sockbuf);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &p->sockbuf, sizeof p->sockbuf);
  }
  g.listener_fd = fd;
  g.listener_profile = p;
}


int
fsro_set_profile(char name) {
  for(size_t i=0; i<sizeof PROFILES / sizeof PROFILES[0]; i++) {
    if(PROFILES[i].name == name) {
      g.profile = &PROFILES[i];
      /* Running connections keep their settings; ending them makes the
       * client's next connection start with the new ones. */
      for(conn_t *c=g.conns; c; c=c->next) {
        shutdown(smb2_get_fd(c->smb2), SHUT_RDWR);
      }
      return 0;
    }
  }
  return -1;
}


static uint64_t
now_ns(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}


static uint64_t
tv_ms(const struct timeval *tv) {
  return (uint64_t)tv->tv_sec * 1000 + (uint64_t)tv->tv_usec / 1000;
}


void
fsro_reset_stats(void) {
  g.reads = 0;
  g.bytes_read = 0;
  g.read_ns = 0;
  g.first_read_ns = 0;
  g.last_read_ns = 0;
  getrusage(RUSAGE_SELF, &g.ru_start);
  g.stats_start_ns = now_ns();
}


static int
destruction_event(struct smb2_server *srv, struct smb2_context *smb2) {
  conn_t *c = smb2_get_opaque(smb2);
  const char *err = smb2_get_error(smb2);
  int j = 0;

  (void)srv;
  if(err && *err) {
    snprintf(g.last_errors[g.nerrors++ % MAX_ERRORS], sizeof g.last_errors[0],
             "%s", err);
  }
  for(int i=0; i<g.ndeferred; i++) {
    if(g.deferred[i].smb2 != smb2) {
      g.deferred[j++] = g.deferred[i];
    }
  }
  g.ndeferred = j;

  if(c) {
    for(conn_t **pp=&g.conns; *pp; pp=&(*pp)->next) {
      if(*pp == c) {
        *pp = c->next;
        break;
      }
    }
    while(c->handles) {
      handle_t *h = c->handles;
      c->handles = h->next;
      free_handle(h);
    }
    free(c);
    smb2_set_opaque(smb2, NULL);
  }
  return 0;
}


/* ------------------------------------------------------ session and trees */

static int
authorize_user(struct smb2_server *srv, struct smb2_context *smb2,
               const char *user, const char *domain, const char *workstation) {
  (void)srv;
  (void)domain;
  (void)workstation;
  g.auth_attempts++;
  /* Without a password set here the library rejects the session. */
  if(user && !strcasecmp(user, g.user)) {
    smb2_set_password(smb2, g.password);
  }
  return 0;
}


static int
session_established(struct smb2_server *srv, struct smb2_context *smb2) {
  (void)srv;
  g.sessions++;
  g.last_dialect = smb2_get_dialect(smb2);
  return 0;
}


static int
logoff_cmd(struct smb2_server *srv, struct smb2_context *smb2) {
  (void)srv;
  (void)smb2;
  return 0;
}


static int
tree_connect_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                 struct smb2_tree_connect_request *req,
                 struct smb2_tree_connect_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  const char *path = NULL;
  const char *share;
  struct smb2_pdu *pdu;
  int kind = TREE_NONE;
  int slot = -1;

  (void)srv;
  if(!c) {
    return reply_status(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }
  if(req->path && req->path_length) {
    path = smb2_utf16_to_utf8(req->path, req->path_length / 2);
  }
  if(path) {
    share = strrchr(path, '\\');
    share = share ? share + 1 : path;
    if(!strcasecmp(share, "IPC$")) {
      kind = TREE_IPC;
    } else if(!strcasecmp(share, g.share)) {
      kind = TREE_DISK;
    }
    free((void *)path);
  }
  if(kind == TREE_NONE) {
    return reply_status(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_BAD_NETWORK_NAME);
  }
  for(int i=0; i<MAX_TREES && slot < 0; i++) {
    if(!c->trees[i].kind) {
      slot = i;
    }
  }
  if(slot < 0) {
    return reply_status(smb2, SMB2_TREE_CONNECT, SMB2_STATUS_INSUFFICIENT_RESOURCES);
  }

  memset(rep, 0, sizeof *rep);
  rep->share_flags = SMB2_SHAREFLAG_NO_CACHING;
  if(kind == TREE_IPC) {
    rep->share_type = SMB2_SHARE_TYPE_PIPE;
    rep->maximal_access = IPC_ACCESS;
  } else {
    rep->share_type = SMB2_SHARE_TYPE_DISK;
    rep->maximal_access = READ_ACCESS;
  }

  /* Our own tree id, so that later requests can be told apart. */
  c->trees[slot].id = ++c->next_tree_id;
  pdu = smb2_cmd_tree_connect_reply_async(smb2, rep, c->trees[slot].id, NULL, NULL);
  if(!pdu) {
    g.lost_replies++;
    return -1;
  }
  c->trees[slot].kind = kind;
  smb2_set_pdu_message_id(smb2, pdu, smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}


static int
tree_disconnect_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                    const uint32_t tree_id) {
  conn_t *c = smb2_get_opaque(smb2);

  (void)srv;
  if(!c) {
    return 0;
  }
  for(int i=0; i<MAX_TREES; i++) {
    if(c->trees[i].kind && c->trees[i].id == tree_id) {
      c->trees[i].kind = TREE_NONE;
    }
  }
  for(handle_t **pp=&c->handles; *pp; ) {
    handle_t *h = *pp;
    if(h->tree_id == tree_id) {
      *pp = h->next;
      free_handle(h);
    } else {
      pp = &h->next;
    }
  }
  return 0;
}


/* ----------------------------------------------------------------- create */

/* Appends one create context to the reply (MS-SMB2 2.2.14.2). */
static size_t
add_context(size_t off, const char *name, const uint8_t *data, uint32_t len) {
  uint8_t *p = g.create_ctx + off;
  size_t size = 24 + len;

  if(off + size > sizeof g.create_ctx) {
    return off;
  }
  memset(p, 0, size);
  put16(p + 4, 16);         /* NameOffset */
  put16(p + 6, 4);          /* NameLength */
  put16(p + 10, 24);        /* DataOffset */
  put32(p + 12, len);       /* DataLength */
  memcpy(p + 16, name, 4);
  memcpy(p + 24, data, len);
  return off + ((size + 7) & ~(size_t)7);
}


/* Answers the MxAc and QFid requests; everything else (durable handles,
 * leases, ...) is not granted by leaving it unanswered. */
static uint32_t
create_contexts(const struct smb2_create_request *req, const struct stat *st) {
  const uint8_t *buf = req->create_context;
  uint32_t len = req->create_context_length;
  size_t out = 0;
  size_t last = 0;
  uint32_t off = 0;

  if(!buf || !len) {
    return 0;
  }
  while(off + 16 <= len) {
    uint32_t next = get32(buf + off);
    uint16_t name_off = get16(buf + off + 4);
    uint16_t name_len = get16(buf + off + 6);

    if(name_len == 4 && off + name_off + 4 <= len) {
      const uint8_t *name = buf + off + name_off;
      uint8_t data[32];
      size_t before = out;

      memset(data, 0, sizeof data);
      if(!memcmp(name, "MxAc", 4)) {
        put32(data + 4, READ_ACCESS);             /* QueryStatus 0 */
        out = add_context(out, "MxAc", data, 8);
      } else if(!memcmp(name, "QFid", 4)) {
        put64(data, (uint64_t)st->st_ino);        /* DiskFileId */
        put64(data + 16, (uint64_t)st->st_dev);   /* VolumeId */
        out = add_context(out, "QFid", data, 32);
      }
      if(out != before) {
        if(before) {
          put32(g.create_ctx + last, (uint32_t)(before - last));
        }
        last = before;
      }
    }
    if(!next || next < 16) {
      break;
    }
    off += next;
  }
  if(!out) {
    return 0;
  }
  /* The last context ends without padding. */
  return (uint32_t)(last + 24 + get32(g.create_ctx + last + 12));
}


static uint32_t
do_create(conn_t *c, struct smb2_create_request *req,
          struct smb2_create_reply *rep) {
  uint32_t tree_id = 0;
  uint32_t status;
  struct stat st;
  handle_t *h;
  int kind = tree_kind(c, &tree_id);
  int is_dir;

  memset(&st, 0, sizeof st);
  if(kind == TREE_IPC) {
    note("CREATE on IPC$ %.30s", req->name ? req->name : "");
    return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  if(kind != TREE_DISK) {
    return STATUS_NETWORK_NAME_DELETED;
  }
  if((req->desired_access & WRITE_ACCESS) ||
     (req->create_options & SMB2_FILE_DELETE_ON_CLOSE)) {
    g.denied_writes++;
    return SMB2_STATUS_ACCESS_DENIED;
  }
  if(!(h=calloc(1, sizeof *h))) {
    return SMB2_STATUS_NO_MEMORY;
  }
  h->fd = -1;

  status = resolve(req->name, h->path, h->rel, &st);
  if(status == SMB2_STATUS_OBJECT_NAME_NOT_FOUND &&
     req->create_disposition != FILE_OPEN &&
     req->create_disposition != FILE_OVERWRITE) {
    g.denied_writes++;
    status = SMB2_STATUS_ACCESS_DENIED;   /* would create a file */
  } else if(!status && req->create_disposition == FILE_CREATE) {
    status = STATUS_OBJECT_NAME_COLLISION;
  } else if(!status && req->create_disposition != FILE_OPEN &&
            req->create_disposition != FILE_OPEN_IF) {
    g.denied_writes++;
    status = SMB2_STATUS_ACCESS_DENIED;   /* supersede or overwrite */
  }
  is_dir = S_ISDIR(st.st_mode);
  if(!status && is_dir && (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE)) {
    status = SMB2_STATUS_FILE_IS_A_DIRECTORY;
  }
  if(!status && !is_dir && (req->create_options & SMB2_FILE_DIRECTORY_FILE)) {
    status = SMB2_STATUS_NOT_A_DIRECTORY;
  }
  if(!status && !is_dir && (h->fd=open(h->path, O_RDONLY)) < 0) {
    status = errno_status(errno);
  }
  if(status) {
    free_handle(h);
    return status;
  }

  h->is_dir = is_dir;
  h->tree_id = tree_id;
  c->next_fid++;
  put64(h->fid, c->next_fid);
  put64(h->fid + 8, c->next_fid ^ 0x5053355350533553ULL);
  h->next = c->handles;
  c->handles = h;

  memset(rep, 0, sizeof *rep);
  rep->create_action = FILE_OPENED;
  rep->creation_time = filetime(&st.st_mtim);
  rep->last_access_time = filetime(&st.st_atim);
  rep->last_write_time = filetime(&st.st_mtim);
  rep->change_time = filetime(&st.st_ctim);
  rep->allocation_size = alloc_size(&st);
  rep->end_of_file = end_of_file(&st);
  rep->file_attributes = attributes(&st);
  memcpy(rep->file_id, h->fid, SMB2_FD_SIZE);
  rep->create_context_length = create_contexts(req, &st);
  rep->create_context = rep->create_context_length ? g.create_ctx : NULL;
  return 0;
}


static int
create_cmd(struct smb2_server *srv, struct smb2_context *smb2,
           struct smb2_create_request *req, struct smb2_create_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status;

  (void)srv;
  g.creates++;
  status = c ? do_create(c, req, rep) : SMB2_STATUS_INSUFFICIENT_RESOURCES;

  /* The library frees the name only on its own reply path. */
  if(req->name) {
    smb2_free_data(smb2, (void *)req->name);
    req->name = NULL;
  }
  if(c) {
    c->last_create_status = status;
    if(!status) {
      memcpy(c->last_fid, rep->file_id, SMB2_FD_SIZE);
    }
  }
  if(status) {
    return reply_status(smb2, SMB2_CREATE, status);
  }
  g.creates_ok++;
  return 0;
}


static int
close_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_close_request *req, struct smb2_close_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  struct stat st;

  (void)srv;
  if(!h) {
    return reply_status(smb2, SMB2_CLOSE, status);
  }
  memset(rep, 0, sizeof *rep);
  if((req->flags & SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB) && stat(h->path, &st) == 0) {
    rep->flags = SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB;
    rep->creation_time = filetime(&st.st_mtim);
    rep->last_access_time = filetime(&st.st_atim);
    rep->last_write_time = filetime(&st.st_mtim);
    rep->change_time = filetime(&st.st_ctim);
    rep->allocation_size = alloc_size(&st);
    rep->end_of_file = end_of_file(&st);
    rep->file_attributes = attributes(&st);
  }
  unlink_handle(c, h);
  free_handle(h);
  return 0;
}


/* ------------------------------------------------------------------- read */

static int
read_cmd(struct smb2_server *srv, struct smb2_context *smb2,
         struct smb2_read_request *req, struct smb2_read_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  uint32_t len = req->length;
  uint64_t t0;
  uint8_t *buf;
  ssize_t n;

  if(!h) {
    return reply_status(smb2, SMB2_READ, status);
  }
  if(h->is_dir) {
    return reply_status(smb2, SMB2_READ, SMB2_STATUS_INVALID_DEVICE_REQUEST);
  }
  if(len > srv->max_read_size) {
    return reply_status(smb2, SMB2_READ, SMB2_STATUS_INVALID_PARAMETER);
  }
  memset(rep, 0, sizeof *rep);
  if(!len) {
    return 0;
  }
  if(!(buf=malloc(len))) {
    return reply_status(smb2, SMB2_READ, SMB2_STATUS_NO_MEMORY);
  }
  t0 = now_ns();
  n = pread(h->fd, buf, len, (off_t)req->offset);
  g.last_read_ns = now_ns();
  g.read_ns += g.last_read_ns - t0;
  if(!g.first_read_ns) {
    g.first_read_ns = t0;
  }
  if(n < 0) {
    status = errno_status(errno);
  } else if(n == 0 || (uint32_t)n < req->minimum_count) {
    status = SMB2_STATUS_END_OF_FILE;
  } else {
    status = 0;
  }
  if(status) {
    free(buf);
    return reply_status(smb2, SMB2_READ, status);
  }
  g.reads++;
  g.bytes_read += (unsigned long long)n;
  rep->data = buf;          /* freed by the library */
  rep->data_length = (uint32_t)n;
  return 0;
}


/* ---------------------------------------------------- directory listings */

static int
list_directory(handle_t *h, const char *pattern) {
  struct dirent *e;
  size_t cap = 64;
  DIR *d;

  free_listing(h);
  h->listed = 1;
  if(!(h->entries=calloc(cap, sizeof *h->entries))) {
    return -ENOMEM;
  }
  if(!(d=opendir(h->path))) {
    return -errno;
  }
  /* "." and ".." first, like Windows servers; ".." of the share root is
   * the root itself, so nothing outside the share is described. */
  for(int dots=0; dots<2; dots++) {
    const char *name = dots ? ".." : ".";
    char parent[PATH_SIZE];

    if(!wild_match(pattern, name) ||
       snprintf(parent, sizeof parent, "%s%s", h->path,
                (dots && h->rel[0]) ? "/.." : "") >= (int)sizeof parent) {
      continue;
    }
    if(stat(parent, &h->entries[h->nentries].st) == 0 &&
       (h->entries[h->nentries].name=strdup(name))) {
      h->nentries++;
    }
  }
  while((e=readdir(d))) {
    char path[PATH_SIZE];
    dir_entry_t *ent;

    if(!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") ||
       !wild_match(pattern, e->d_name)) {
      continue;
    }
    if(h->nentries == cap) {
      dir_entry_t *bigger = realloc(h->entries, 2 * cap * sizeof *bigger);
      if(!bigger) {
        break;
      }
      h->entries = bigger;
      cap *= 2;
    }
    ent = &h->entries[h->nentries];
    if(snprintf(path, sizeof path, "%s/%s", h->path, e->d_name) >= (int)sizeof path ||
       stat(path, &ent->st) != 0 || !(ent->name=strdup(e->d_name))) {
      continue;
    }
    h->nentries++;
  }
  closedir(d);
  return 0;
}


/* Encodes one entry of the given class at p (MS-FSCC 2.4). Returns its
 * unpadded size, or 0 if it does not fit into room. */
static size_t
encode_entry(uint8_t cls, const dir_entry_t *e, uint8_t *p, size_t room) {
  size_t fixed;
  int name_len;

  switch(cls) {
  case SMB2_FILE_DIRECTORY_INFORMATION:         fixed = 64;  break;
  case SMB2_FILE_FULL_DIRECTORY_INFORMATION:    fixed = 68;  break;
  case SMB2_FILE_BOTH_DIRECTORY_INFORMATION:    fixed = 94;  break;
  case SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION: fixed = 104; break;
  case SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION: fixed = 80;  break;
  case SMB2_FILE_NAMES_INFORMATION:             fixed = 12;  break;
  default:                                      return 0;
  }
  if(room < fixed) {
    return 0;
  }
  if((name_len=utf16(e->name, p + fixed, room - fixed)) < 0) {
    return 0;
  }
  memset(p, 0, fixed);
  if(cls == SMB2_FILE_NAMES_INFORMATION) {
    put32(p + 8, (uint32_t)name_len);
    return fixed + (size_t)name_len;
  }
  put_times(p + 8, &e->st);
  put64(p + 40, end_of_file(&e->st));
  put64(p + 48, alloc_size(&e->st));
  put32(p + 56, attributes(&e->st));
  put32(p + 60, (uint32_t)name_len);
  if(cls == SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION) {
    put64(p + 96, (uint64_t)e->st.st_ino);
  } else if(cls == SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION) {
    put64(p + 72, (uint64_t)e->st.st_ino);
  }
  return fixed + (size_t)name_len;
}


static int
query_directory_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                    struct smb2_query_directory_request *req,
                    struct smb2_query_directory_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  /* req->name is uninitialised when the request carries no pattern. */
  const char *pattern = (req->file_name_length && req->name) ? req->name : "*";
  size_t room = req->output_buffer_length;
  size_t off = 0;
  size_t last = 0;
  int fresh = 0;
  uint8_t *buf;

  (void)srv;
  g.dir_queries++;
  memset(rep, 0, sizeof *rep);
  if(!h) {
    return defer_status(smb2, SMB2_QUERY_DIRECTORY, status);
  }
  if(!h->is_dir) {
    return defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INVALID_PARAMETER);
  }
  switch(req->file_information_class) {
  case SMB2_FILE_DIRECTORY_INFORMATION:
  case SMB2_FILE_FULL_DIRECTORY_INFORMATION:
  case SMB2_FILE_BOTH_DIRECTORY_INFORMATION:
  case SMB2_FILE_ID_BOTH_DIRECTORY_INFORMATION:
  case SMB2_FILE_ID_FULL_DIRECTORY_INFORMATION:
  case SMB2_FILE_NAMES_INFORMATION:
    break;
  default:
    note("QUERY_DIRECTORY class 0x%02x", req->file_information_class);
    return defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INVALID_INFO_CLASS);
  }

  if(!h->listed || (req->flags & (SMB2_RESTART_SCANS | SMB2_REOPEN))) {
    int rc = list_directory(h, !strcmp(pattern, "*.*") ? "*" : pattern);
    if(rc) {
      return defer_status(smb2, SMB2_QUERY_DIRECTORY, errno_status(-rc));
    }
    fresh = 1;
  }
  if(h->pos >= h->nentries) {
    if(fresh) {
      return defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_SUCH_FILE);
    }
    return 0;    /* the library answers STATUS_NO_MORE_FILES */
  }

  if(room > srv->max_transact_size) {
    room = srv->max_transact_size;
  }
  /* calloc: the padding between entries goes out as it is. */
  if(!(buf=calloc(1, room ? room : 1))) {
    return defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_MEMORY);
  }
  /* Entries start 8-byte aligned; NextEntryOffset is relative to the
   * entry, 0 in the last one. */
  while(h->pos < h->nentries) {
    size_t start = (off + 7) & ~(size_t)7;
    size_t size;

    if(off && start >= room) {
      break;
    }
    size = encode_entry(req->file_information_class, &h->entries[h->pos],
                        buf + start, room - start);
    if(!size) {
      break;
    }
    if(off) {
      put32(buf + last, (uint32_t)(start - last));
    }
    last = start;
    off = start + size;
    h->pos++;
    if(req->flags & SMB2_RETURN_SINGLE_ENTRY) {
      break;
    }
  }
  if(!off) {
    free(buf);
    return defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_INFO_LENGTH_MISMATCH);
  }

  /* The library copies the buffer after we return. */
  free(h->dir_buf);
  h->dir_buf = buf;
  rep->output_buffer = buf;
  rep->output_buffer_length = (uint32_t)off;
  return 0;
}


/* ------------------------------------------------------------- query info */

/* FILE_ALL_INFORMATION and friends name the file relative to the share. */
static int
put_name(uint8_t *p, size_t room, const char *rel, int leading) {
  char name[PATH_SIZE + 1];
  int n;

  snprintf(name, sizeof name, "%s%s", leading ? "\\" : "", rel);
  n = utf16(name, p + 4, room > 4 ? room - 4 : 0);
  if(n < 0) {
    return -1;
  }
  put32(p, (uint32_t)n);
  return 4 + n;
}


/* Encodes FILE info class cls for h into buf. Returns the full length
 * (which may exceed room for variable classes), 0 with *status set on
 * error. */
static size_t
file_info(uint8_t cls, const handle_t *h, const struct stat *st,
          uint8_t *buf, size_t size, uint32_t *status) {
  int n;

  memset(buf, 0, size);
  switch(cls) {
  case SMB2_FILE_BASIC_INFORMATION:
    put_times(buf, st);
    put32(buf + 32, attributes(st));
    return 40;
  case SMB2_FILE_STANDARD_INFORMATION:
    put64(buf, alloc_size(st));
    put64(buf + 8, end_of_file(st));
    put32(buf + 16, (uint32_t)st->st_nlink);
    buf[21] = h->is_dir;
    return 24;
  case SMB2_FILE_INTERNAL_INFORMATION:
    put64(buf, (uint64_t)st->st_ino);
    return 8;
  case SMB2_FILE_EA_INFORMATION:
    return 4;
  case SMB2_FILE_ACCESS_INFORMATION:
    put32(buf, READ_ACCESS);
    return 4;
  case SMB2_FILE_POSITION_INFORMATION:
  case SMB2_FILE_MODE_INFORMATION:
  case SMB2_FILE_ALIGNMENT_INFORMATION:
    return cls == SMB2_FILE_POSITION_INFORMATION ? 8 : 4;
  case SMB2_FILE_ALL_INFORMATION:
    put_times(buf, st);
    put32(buf + 32, attributes(st));
    put64(buf + 40, alloc_size(st));
    put64(buf + 48, end_of_file(st));
    put32(buf + 56, (uint32_t)st->st_nlink);
    buf[61] = h->is_dir;
    put64(buf + 64, (uint64_t)st->st_ino);
    put32(buf + 76, READ_ACCESS);
    if((n=put_name(buf + 96, size - 96, h->rel, 1)) < 0) {
      break;
    }
    return 96 + (size_t)n;
  case SMB2_FILE_NETWORK_OPEN_INFORMATION:
    put_times(buf, st);
    put64(buf + 32, alloc_size(st));
    put64(buf + 40, end_of_file(st));
    put32(buf + 48, attributes(st));
    return 56;
  case SMB2_FILE_ATTRIBUTE_TAG_INFORMATION:
    put32(buf, attributes(st));
    return 8;
  case SMB2_FILE_STREAM_INFORMATION:
    if(h->is_dir) {
      return 0;    /* directories have no data stream */
    }
    put32(buf + 4, 14);
    put64(buf + 8, end_of_file(st));
    put64(buf + 16, alloc_size(st));
    utf16("::$DATA", buf + 24, 14);
    return 38;
  case SMB2_FILE_NORMALIZED_NAME_INFORMATION:
    if((n=put_name(buf, size, h->rel, 0)) < 0) {
      break;
    }
    return (size_t)n;
  case SMB2_FILE_ID_INFORMATION:
    put64(buf, (uint64_t)g.root_st.st_dev);
    put64(buf + 8, (uint64_t)st->st_ino);
    return 24;
  case SMB2_FILE_ALTERNATE_NAME_INFORMATION:
    /* No 8.3 names. NOT_SUPPORTED is the answer clients skip quietly;
     * smbclient's allinfo aborts on OBJECT_NAME_NOT_FOUND. */
    *status = SMB2_STATUS_NOT_SUPPORTED;
    return 0;
  default:
    note("QUERY_INFO file 0x%02x", cls);
    *status = SMB2_STATUS_NOT_SUPPORTED;
    return 0;
  }
  *status = SMB2_STATUS_OBJECT_NAME_INVALID;
  return 0;
}


static size_t
fs_info(uint8_t cls, uint8_t *buf, size_t size, uint32_t *status) {
  struct statvfs vfs;
  uint64_t unit;

  memset(buf, 0, size);
  if(statvfs(g.root, &vfs) != 0) {
    *status = errno_status(errno);
    return 0;
  }
  unit = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
  switch(cls) {
  case SMB2_FILE_FS_VOLUME_INFORMATION:
    put64(buf, filetime(&g.root_st.st_mtim));
    put32(buf + 8, (uint32_t)g.root_st.st_dev);
    put32(buf + 12, (uint32_t)utf16("PS5", buf + 18, size - 18));
    return 18 + 6;
  case SMB2_FILE_FS_SIZE_INFORMATION:
    put64(buf, (uint64_t)vfs.f_blocks);
    put64(buf + 8, (uint64_t)vfs.f_bavail);
    put32(buf + 16, (uint32_t)(unit / 512 ? unit / 512 : 1));
    put32(buf + 20, 512);
    return 24;
  case SMB2_FILE_FS_FULL_SIZE_INFORMATION:
    put64(buf, (uint64_t)vfs.f_blocks);
    put64(buf + 8, (uint64_t)vfs.f_bavail);
    put64(buf + 16, (uint64_t)vfs.f_bfree);
    put32(buf + 24, (uint32_t)(unit / 512 ? unit / 512 : 1));
    put32(buf + 28, 512);
    return 32;
  case SMB2_FILE_FS_DEVICE_INFORMATION:
    put32(buf, FILE_DEVICE_DISK);
    put32(buf + 4, FILE_DEVICE_IS_MOUNTED);
    return 8;
  case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:
    put32(buf, FILE_CASE_PRESERVED_NAMES | FILE_UNICODE_ON_DISK |
               FILE_READ_ONLY_VOLUME);
    put32(buf + 4, 255);
    put32(buf + 8, (uint32_t)utf16("NTFS", buf + 12, size - 12));
    return 12 + 8;
  case SMB2_FILE_FS_SECTOR_SIZE_INFORMATION:
    put32(buf, 512);
    put32(buf + 4, 512);
    put32(buf + 8, 512);
    put32(buf + 12, 512);
    put32(buf + 16, SSINFO_FLAGS_ALIGNED_DEVICE |
                    SSINFO_FLAGS_PARTITION_ALIGNED_ON_DEVICE);
    return 28;
  default:
    note("QUERY_INFO fs 0x%02x", cls);
    *status = SMB2_STATUS_NOT_SUPPORTED;
    return 0;
  }
}


/* Minimum output buffer for each class: shorter buffers get
 * STATUS_INFO_LENGTH_MISMATCH, longer variable data is truncated with
 * STATUS_BUFFER_OVERFLOW (MS-SMB2 3.3.5.20.1). */
static size_t
min_size(uint8_t type, uint8_t cls, size_t full) {
  if(type == SMB2_0_INFO_FILE) {
    switch(cls) {
    case SMB2_FILE_ALL_INFORMATION:             return 100;
    case SMB2_FILE_NORMALIZED_NAME_INFORMATION: return 4;
    case SMB2_FILE_STREAM_INFORMATION:          return 24;
    }
  } else {
    switch(cls) {
    case SMB2_FILE_FS_VOLUME_INFORMATION:       return 18;
    case SMB2_FILE_FS_ATTRIBUTE_INFORMATION:    return 12;
    }
  }
  return full;
}


static int
query_info_cmd(struct smb2_server *srv, struct smb2_context *smb2,
               struct smb2_query_info_request *req,
               struct smb2_query_info_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  struct smb2_query_info_request as_raw;
  uint8_t buf[2 * PATH_SIZE + 128];
  struct smb2_pdu *pdu;
  struct stat st;
  size_t len = 0;
  int overflow = 0;

  (void)srv;
  if(!h) {
    return reply_status(smb2, SMB2_QUERY_INFO, status);
  }
  if(stat(h->path, &st) != 0) {
    return reply_status(smb2, SMB2_QUERY_INFO, errno_status(errno));
  }
  status = 0;
  switch(req->info_type) {
  case SMB2_0_INFO_FILE:
    len = file_info(req->file_info_class, h, &st, buf, sizeof buf, &status);
    break;
  case SMB2_0_INFO_FILESYSTEM:
    len = fs_info(req->file_info_class, buf, sizeof buf, &status);
    break;
  default:
    note("QUERY_INFO type %u", req->info_type);
    status = SMB2_STATUS_NOT_SUPPORTED;
    break;
  }
  if(status) {
    return reply_status(smb2, SMB2_QUERY_INFO, status);
  }
  if(req->output_buffer_length < min_size(req->info_type, req->file_info_class, len)) {
    return reply_status(smb2, SMB2_QUERY_INFO, SMB2_STATUS_INFO_LENGTH_MISMATCH);
  }
  if(len > req->output_buffer_length) {
    len = req->output_buffer_length;
    overflow = 1;
  }

  /* An info class without a library encoder makes passthrough copy our
   * bytes unchanged. */
  as_raw = *req;
  as_raw.info_type = SMB2_0_INFO_FILE;
  as_raw.file_info_class = SMB2_FILE_INFO_CLASS_RESERVED;
  memset(rep, 0, sizeof *rep);
  rep->output_buffer = buf;
  rep->output_buffer_length = (uint32_t)len;
  pdu = smb2_cmd_query_info_reply_async(smb2, &as_raw, rep, NULL, NULL);
  if(!pdu) {
    g.lost_replies++;
    return -1;
  }
  if(overflow) {
    smb2_set_pdu_status(smb2, pdu, SMB2_STATUS_BUFFER_OVERFLOW);
  }
  smb2_set_pdu_message_id(smb2, pdu, smb2_get_last_request_message_id(smb2));
  smb2_queue_pdu(smb2, pdu);
  return 1;
}


/* ------------------------------------------------- everything read-only */

static int
write_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_write_request *req, struct smb2_write_reply *rep) {
  (void)srv;
  (void)req;
  (void)rep;
  g.denied_writes++;
  return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
}


static int
set_info_cmd(struct smb2_server *srv, struct smb2_context *smb2,
             struct smb2_set_info_request *req) {
  (void)srv;
  (void)req;
  g.denied_writes++;
  return reply_status(smb2, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
}


static int
flush_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_flush_request *req) {
  (void)srv;
  (void)smb2;
  (void)req;
  return 0;
}


/* Byte-range locks are not enforced on a read-only share. */
static int
lock_cmd(struct smb2_server *srv, struct smb2_context *smb2,
         struct smb2_lock_request *req) {
  (void)srv;
  (void)smb2;
  (void)req;
  return 0;
}


/* No oplocks or leases are granted, so there is nothing to acknowledge. */
static int
oplock_break_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                 struct smb2_oplock_break_acknowledgement *req) {
  (void)srv;
  (void)req;
  return reply_status(smb2, SMB2_OPLOCK_BREAK, SMB2_STATUS_INVALID_OPLOCK_PROTOCOL);
}


static int
lease_break_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                struct smb2_lease_break_acknowledgement *req) {
  (void)srv;
  (void)req;
  return reply_status(smb2, SMB2_OPLOCK_BREAK, SMB2_STATUS_INVALID_OPLOCK_PROTOCOL);
}


static int
ioctl_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_ioctl_request *req, struct smb2_ioctl_reply *rep) {
  (void)srv;
  (void)rep;
  switch(req->ctl_code) {
  case FSCTL_DFS_GET_REFERRALS:
  case FSCTL_DFS_GET_REFERRALS_EX:
    /* MS-SMB2 3.3.5.15.2: a server without DFS. */
    return reply_status(smb2, SMB2_IOCTL, SMB2_STATUS_FS_DRIVER_REQUIRED);
  default:
    note("IOCTL 0x%08x", (unsigned)req->ctl_code);
    return reply_status(smb2, SMB2_IOCTL, SMB2_STATUS_INVALID_DEVICE_REQUEST);
  }
}


/* Nothing changes on a read-only share. Spike S3 decides whether the
 * product keeps notifications pending. */
static int
change_notify_cmd(struct smb2_server *srv, struct smb2_context *smb2,
                  struct smb2_change_notify_request *req,
                  struct smb2_change_notify_reply *rep) {
  (void)srv;
  (void)req;
  (void)rep;
  note("CHANGE_NOTIFY");
  return reply_status(smb2, SMB2_CHANGE_NOTIFY, SMB2_STATUS_NOT_SUPPORTED);
}


/* A cancel never gets a reply of its own. */
static int
cancel_cmd(struct smb2_server *srv, struct smb2_context *smb2) {
  (void)srv;
  (void)smb2;
  return 0;
}


static int
echo_cmd(struct smb2_server *srv, struct smb2_context *smb2) {
  (void)srv;
  (void)smb2;
  return 0;
}


/* Every pointer is set: the library calls some without a NULL check and
 * answers missing ones with STATUS_NOT_IMPLEMENTED. */
struct smb2_server_request_handlers fsro_handlers = {
  .destruction_event   = destruction_event,
  .authorize_user      = authorize_user,
  .session_established = session_established,
  .logoff_cmd          = logoff_cmd,
  .tree_connect_cmd    = tree_connect_cmd,
  .tree_disconnect_cmd = tree_disconnect_cmd,
  .create_cmd          = create_cmd,
  .close_cmd           = close_cmd,
  .flush_cmd           = flush_cmd,
  .read_cmd            = read_cmd,
  .write_cmd           = write_cmd,
  .oplock_break_cmd    = oplock_break_cmd,
  .lease_break_cmd     = lease_break_cmd,
  .lock_cmd            = lock_cmd,
  .ioctl_cmd           = ioctl_cmd,
  .cancel_cmd          = cancel_cmd,
  .echo_cmd            = echo_cmd,
  .query_directory_cmd = query_directory_cmd,
  .change_notify_cmd   = change_notify_cmd,
  .query_info_cmd      = query_info_cmd,
  .set_info_cmd        = set_info_cmd,
};


int
fsro_init(const char *root, const char *share_name,
          const char *user, const char *password) {
  snprintf(g.root, sizeof g.root, "%s", root);
  snprintf(g.share, sizeof g.share, "%s", share_name);
  snprintf(g.user, sizeof g.user, "%s", user);
  snprintf(g.password, sizeof g.password, "%s", password);
  g.profile = &PROFILES[0];
  g.listener_fd = -1;
  fsro_reset_stats();
  if(stat(g.root, &g.root_st) != 0) {
    return -errno;
  }
  return S_ISDIR(g.root_st.st_mode) ? 0 : -ENOTDIR;
}


int
fsro_status(char *buf, size_t size) {
  struct rusage ru;
  uint64_t window = g.first_read_ns ? g.last_read_ns - g.first_read_ns : 0;
  int n;

  getrusage(RUSAGE_SELF, &ru);
  n = snprintf(buf, size,
               "profile=%c (%s)\ndialect=0x%04x\n"
               "auth_attempts=%u\nsessions=%u\ncreates=%u\ncreates_ok=%u\n"
               "denied_writes=%u\ndir_queries=%u\nlost_replies=%u\n"
               "since_reset_ms=%llu\ncpu_user_ms=%llu\ncpu_sys_ms=%llu\n"
               "reads=%llu\nbytes_read=%llu\nread_window_ms=%llu\npread_ms=%llu\n",
               g.profile->name, g.profile->what, g.last_dialect,
               g.auth_attempts, g.sessions, g.creates, g.creates_ok,
               g.denied_writes, g.dir_queries, g.lost_replies,
               (unsigned long long)((now_ns() - g.stats_start_ns) / 1000000),
               (unsigned long long)(tv_ms(&ru.ru_utime) - tv_ms(&g.ru_start.ru_utime)),
               (unsigned long long)(tv_ms(&ru.ru_stime) - tv_ms(&g.ru_start.ru_stime)),
               g.reads, g.bytes_read,
               (unsigned long long)(window / 1000000),
               (unsigned long long)(g.read_ns / 1000000));

  for(int i=0; i<MAX_NOTES && g.notes[i].count && n > 0 && (size_t)n < size; i++) {
    n += snprintf(buf + n, size - n, "unsupported: %s x%u\n",
                  g.notes[i].what, g.notes[i].count);
  }
  for(unsigned i=0; i<MAX_ERRORS && i<g.nerrors && n > 0 && (size_t)n < size; i++) {
    n += snprintf(buf + n, size - n, "connection_end: %s\n",
                  g.last_errors[(g.nerrors - 1 - i) % MAX_ERRORS]);
  }
  return (n < 0 || (size_t)n >= size) ? (int)(size ? size - 1 : 0) : n;
}
