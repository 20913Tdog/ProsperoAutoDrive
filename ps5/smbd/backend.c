/* Prospero AutoDrive - prospero-smbd: file system backend
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Serves one directory through the libsmb2 server engine, read-write (on
 * the console the root "/"). Taken over from the S2/S3 spike prototype
 * (ps5/spikes/smbserver), which stays as it was measured. Only regular
 * files and directories are opened: a device, FIFO or socket could block
 * the single-threaded server on open() or read(), so it is refused.
 * The engine handles negotiate, NTLMv2 session setup and
 * signing; everything that touches files is here, encoded as described in
 * MS-SMB2 and MS-FSCC. Writes go straight to the file (no write-behind);
 * FLUSH and write-through requests are made durable with fsync().
 *
 * Not yet (phase 2, P2.5): share modes and byte-range locks are not
 * enforced, attributes other than directory/archive are not stored, and
 * named streams are not offered.
 *
 * Workarounds for the pinned libsmb2 (7e4ff97):
 *  - Passthrough mode on every connection. Directory listings and all
 *    QUERY_INFO data are encoded here: the library's directory encoder
 *    writes absolute NextEntryOffset values, and its QUERY_INFO encoders
 *    only cover some classes.
 *  - A handler error (< 0) always becomes STATUS_NOT_IMPLEMENTED, so every
 *    other status is sent as a self-queued error reply.
 *  - The library still reads a QUERY_DIRECTORY request after the handler
 *    returned, so its error replies wait for backend_flush().
 *  - Compound requests arrive one at a time. Related operations carry the
 *    all-0xFF file id and use the last CREATE result of the connection.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>
#include <smb2/smb2-errors.h>

#include "backend.h"

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
#define FULL_ACCESS            0x001F01FF
#define IPC_ACCESS             FULL_ACCESS
#define MAXIMUM_ALLOWED        0x02000000
/* Access that needs the file opened for writing. */
#define DATA_WRITE_ACCESS (FILE_WRITE_DATA | FILE_APPEND_DATA | GENERIC_WRITE | \
                           GENERIC_ALL | MAXIMUM_ALLOWED)

/* CREATE dispositions and actions (MS-SMB2 2.2.13, 2.2.14). */
#define FILE_SUPERSEDE    0
#define FILE_OPEN         1
#define FILE_CREATE       2
#define FILE_OPEN_IF      3
#define FILE_OVERWRITE    4
#define FILE_OVERWRITE_IF 5
#define FILE_SUPERSEDED   0
#define FILE_OPENED       1
#define FILE_CREATED      2
#define FILE_OVERWRITTEN  3

/* SET_INFO classes without a libsmb2 name (MS-FSCC 2.4). */
#define FILE_DISPOSITION_INFORMATION_EX 0x40
#define FILE_RENAME_INFORMATION_EX      0x41
#define FILE_DISPOSITION_DELETE         0x00000001

/* WRITE at this offset appends (MS-SMB2 3.3.5.13). */
#define WRITE_TO_END_OF_FILE UINT64_MAX
/* Where the data of a WRITE starts: header plus fixed part. libsmb2 hands
 * over the bytes right after the fixed part, so other offsets are refused. */
#define WRITE_DATA_OFFSET    (64 + 48)
#define SET_INFO_DATA_OFFSET (64 + 32)

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
  int have_st;            /* st is filled; else stat() when it is sent */
} dir_entry_t;

typedef struct handle {
  struct handle *next;
  uint8_t fid[SMB2_FD_SIZE];
  uint32_t tree_id;
  char path[PATH_SIZE];   /* host path */
  char rel[PATH_SIZE];    /* share-relative, '\'-separated, "" = root */
  int fd;                 /* files only, -1 for directories */
  int is_dir;
  int can_write;          /* fd is open for writing */
  int write_through;      /* FILE_WRITE_THROUGH: fsync() every write */
  int delete_pending;     /* delete when this handle closes */
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
  int writable;
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
  unsigned special_refused;               /* devices, FIFOs, sockets */

  /* Write side (S3). */
  unsigned files_created;
  unsigned dirs_created;
  unsigned renames;
  unsigned deletes;
  unsigned delete_failures;
  unsigned truncates;
  unsigned times_set;
  unsigned creation_times_ignored;
  unsigned attributes_ignored;
  unsigned long long fsyncs;
  /* Owner and mode the console gives new entries. */
  char last_created[PATH_SIZE];
  struct stat last_created_st;

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

  /* Since the last backend_reset_stats(), for the speed diagnosis. */
  uint64_t stats_start_ns;
  struct rusage ru_start;
  uint64_t read_ns;                     /* time spent in pread() */
  uint64_t first_read_ns;
  uint64_t last_read_ns;
  unsigned long long writes;
  unsigned long long bytes_written;
  uint64_t write_ns;                    /* time spent in pwrite() */
  uint64_t fsync_ns;                    /* time spent in fsync() */
  unsigned long long writes_async;      /* of those, in the write thread */
  uint64_t write_wait_ns;               /* main loop waiting for that thread */
} g;


/* A big WRITE runs pwrite() in a thread, while the main loop already
 * receives the next request: on the console pwrite() takes about 6 ms per
 * MiB and receiving the next MiB about 9 ms, one after the other the
 * share wrote about 79 MB/s against 89 MB/s of ftpsrv. The reply still
 * goes out only once the data is written, as before. One write at a time:
 * every other request (lookup(), create) first waits for it, so it sees
 * the data, and the replies keep their order. */
typedef struct {
  int active;
  pthread_t thread;
  struct smb2_context *smb2;      /* NULL once the connection is gone */
  uint64_t message_id;
  int fd;
  uint8_t *buf;
  size_t len;
  uint64_t offset;
  /* Set by the thread. */
  size_t done;
  uint32_t status;
  uint64_t ns;
  int finished;
} pending_write_t;

static pending_write_t g_pw;
static int g_write_thread_ok = 1;     /* off after pthread_create() failed */
/* The thread writes one byte here when it is done; the read end is in the
 * main loop's select(), so the reply goes out right away. */
static int g_wake[2] = {-1, -1};

#define ASYNC_WRITE_MIN (64 * 1024)

static void write_poll(void);


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


static uint64_t
get64(const uint8_t *p) {
  return get32(p) | (uint64_t)get32(p + 4) << 32;
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


/* Same, but sent from backend_flush(), after the library is done with the
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
backend_flush(void) {
  write_poll();
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
  case EEXIST:       return SMB2_STATUS_OBJECT_NAME_COLLISION;
  case ENOTEMPTY:    return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
  case ENOSPC:
  case EFBIG:
  case EDQUOT:       return SMB2_STATUS_DISK_FULL;
  case EROFS:        return SMB2_STATUS_MEDIA_WRITE_PROTECTED;
  case EXDEV:        return SMB2_STATUS_NOT_SAME_DEVICE;
  case EBUSY:        return SMB2_STATUS_SHARING_VIOLATION;
  case EINVAL:       return SMB2_STATUS_INVALID_PARAMETER;
  default:
    note("errno %d", err);
    return SMB2_STATUS_INTERNAL_ERROR;
  }
}


#define FILETIME_EPOCH_DIFF 11644473600ULL   /* 1601 to 1970, in seconds */

/* How far the console clock runs ahead of the PC, in whole seconds
 * (decision "variant 1", P2.3). Times on disk stay as the console writes
 * them; Windows sees them minus the skew and sets them plus the skew. */
static int64_t g_skew_s;

static uint64_t
filetime(const struct timespec *ts) {
  int64_t s = (int64_t)ts->tv_sec - g_skew_s + (int64_t)FILETIME_EPOCH_DIFF;

  if(s < 0) {
    return 0;
  }
  return (uint64_t)s * 10000000ULL + (uint64_t)ts->tv_nsec / 100;
}


/* FILETIME from a client, to microseconds. 0 and -1/-2 (MS-FSCC 2.4.7)
 * leave the time as it is, so do values before 1970: then tv keeps the
 * current time and 0 is returned. */
static int
from_filetime(uint64_t ft, struct timeval *tv) {
  if(ft == 0 || ft == UINT64_MAX || ft == UINT64_MAX - 1 ||
     ft < FILETIME_EPOCH_DIFF * 10000000ULL) {
    return 0;
  }
  tv->tv_sec = (time_t)((int64_t)(ft / 10000000ULL - FILETIME_EPOCH_DIFF) + g_skew_s);
  tv->tv_usec = (suseconds_t)(ft % 10000000ULL / 10);
  return 1;
}


/* Creation time: the birth time where the file system keeps one (the PS5
 * is FreeBSD-based), the modification time elsewhere. */
static const struct timespec *
birth(const struct stat *st) {
#if defined(__SCE__) || defined(__FreeBSD__)
  if(st->st_birthtim.tv_sec > 0) {
    return &st->st_birthtim;
  }
#endif
  return &st->st_mtim;
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


static void
put_times(uint8_t *p, const struct stat *st) {
  put64(p, filetime(birth(st)));
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


/* Names of one folder, by case-folded hash, for find_ci. Each new name
 * that does not exist in the client's spelling must be looked for in every
 * other spelling, which meant reading the whole folder per file: creating
 * 20,000 files took twice as long per file at the end as at the start
 * (live, 25.10.2026). The cache holds the folder last searched. It is valid
 * while the folder's identity and mtime are unchanged and for at most
 * NC_MAX_AGE_S, so a change by another program within one mtime tick is
 * seen eventually. Our own creates, deletes and renames update it
 * (nc_note). 30 s, not 2: reading 20,000 names takes about 0.45 s on the
 * console, and every 2 s that still made each new file 2 ms slower at the
 * end (live, 25.10.2026). */
#define NC_MAX_AGE_S 30

static struct {
  char dir[PATH_SIZE];
  dev_t dev;
  ino_t ino;
  struct timespec mtime;
  time_t built;               /* CLOCK_MONOTONIC seconds, 0 = empty */
  char **names;               /* NULL where a name was removed */
  size_t n, cap;
  uint32_t *slots;            /* index + 1 into names, 0 = free */
  size_t nslots;              /* power of two */
} nc;


static time_t
mono_s(void) {
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + 1;       /* never 0 */
}


static uint32_t
nc_hash(const char *name) {
  uint32_t h = 2166136261u;

  while(*name) {
    h = (h ^ fold(next_cp(&name))) * 16777619u;
  }
  return h;
}


static void
nc_clear(void) {
  for(size_t i=0; i<nc.n; i++) {
    free(nc.names[i]);
  }
  free(nc.names);
  free(nc.slots);
  memset(&nc, 0, sizeof nc);
}


/* Returns 0, or -1 when out of memory (then the cache is dropped). */
static int
nc_insert(const char *name) {
  if(nc.n == nc.cap) {
    size_t cap = nc.cap ? 2 * nc.cap : 256;
    char **bigger = realloc(nc.names, cap * sizeof *bigger);

    if(!bigger) {
      return -1;
    }
    nc.names = bigger;
    nc.cap = cap;
  }
  if(2 * (nc.n + 1) > nc.nslots) {
    size_t nslots = nc.nslots ? 2 * nc.nslots : 512;
    uint32_t *slots = calloc(nslots, sizeof *slots);

    if(!slots) {
      return -1;
    }
    for(size_t i=0; i<nc.n; i++) {
      size_t s;

      if(!nc.names[i]) {
        continue;
      }
      s = nc_hash(nc.names[i]) & (nslots - 1);
      while(slots[s]) {
        s = (s + 1) & (nslots - 1);
      }
      slots[s] = (uint32_t)i + 1;
    }
    free(nc.slots);
    nc.slots = slots;
    nc.nslots = nslots;
  }
  if(!(nc.names[nc.n]=strdup(name))) {
    return -1;
  }
  {
    size_t s = nc_hash(name) & (nc.nslots - 1);

    while(nc.slots[s]) {
      s = (s + 1) & (nc.nslots - 1);
    }
    nc.slots[s] = (uint32_t)nc.n + 1;
  }
  nc.n++;
  return 0;
}


static char **
nc_find(const char *name) {
  size_t s;

  if(!nc.nslots) {
    return NULL;
  }
  for(s=nc_hash(name) & (nc.nslots - 1); nc.slots[s]; s=(s + 1) & (nc.nslots - 1)) {
    char **p = &nc.names[nc.slots[s] - 1];

    if(*p && name_equal(*p, name)) {
      return p;
    }
  }
  return NULL;
}


static int
nc_valid(const char *dir, const struct stat *st) {
  return nc.built && mono_s() - nc.built < NC_MAX_AGE_S && !strcmp(nc.dir, dir) &&
         nc.dev == st->st_dev && nc.ino == st->st_ino &&
         nc.mtime.tv_sec == st->st_mtim.tv_sec && nc.mtime.tv_nsec == st->st_mtim.tv_nsec;
}


/* Reads dir into the cache; its stat st is taken before, so a change during
 * the read makes the next check fail. Returns 0 or -1. */
static int
nc_build(const char *dir, const struct stat *st) {
  struct dirent *e;
  DIR *d;

  nc_clear();
  if(strlen(dir) >= sizeof nc.dir || !(d=opendir(dir))) {
    return -1;
  }
  while((e=readdir(d))) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..") && nc_insert(e->d_name) != 0) {
      closedir(d);
      nc_clear();
      return -1;
    }
  }
  closedir(d);
  snprintf(nc.dir, sizeof nc.dir, "%s", dir);
  nc.dev = st->st_dev;
  nc.ino = st->st_ino;
  nc.mtime = st->st_mtim;
  nc.built = mono_s();
  return 0;
}


/* After this server added or removed the entry at path: keeps the cache of
 * its folder valid, with the folder's new mtime. */
static void
nc_note(const char *path, int added) {
  char dir[PATH_SIZE];
  const char *leaf = strrchr(path, '/');
  struct stat st;
  size_t dlen;

  if(!nc.built || !leaf) {
    return;
  }
  dlen = leaf == path ? 1 : (size_t)(leaf - path);
  if(dlen >= sizeof dir) {
    return;
  }
  memcpy(dir, path, dlen);
  dir[dlen] = '\0';
  leaf++;
  if(strcmp(dir, nc.dir)) {
    return;
  }
  if(added) {
    if(!nc_find(leaf) && nc_insert(leaf) != 0) {
      nc_clear();
      return;
    }
  } else {
    char **p = nc_find(leaf);

    if(p && !strcmp(*p, leaf)) {
      free(*p);
      *p = NULL;
    }
  }
  if(stat(dir, &st) != 0) {
    nc_clear();
    return;
  }
  nc.mtime = st.st_mtim;
}


/* Finds the on-disk spelling of name in dir, ignoring case. */
static int
find_ci(const char *dir, const char *name, char *out, size_t size) {
  struct stat st;
  char **p;

  if(stat(dir, &st) != 0) {
    return 0;
  }
  if(!nc_valid(dir, &st) && nc_build(dir, &st) != 0) {
    /* Without memory: the slow way. */
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
  if(!(p=nc_find(name))) {
    return 0;
  }
  snprintf(out, size, "%s", *p);
  return 1;
}


/* Separator between dir and a name below it: none if dir already ends in
 * "/", which is the case for the root "/". */
static const char *
sep(const char *dir) {
  size_t n = strlen(dir);

  return (n && dir[n - 1] == '/') ? "" : "/";
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
    if(snprintf(path + plen, PATH_SIZE - plen, "%s%s", sep(path), comp) >= (int)(PATH_SIZE - plen)) {
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
      if(snprintf(path + plen, PATH_SIZE - plen, "%s%s", sep(path), actual) >= (int)(PATH_SIZE - plen)) {
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


/* A name whose last component need not exist yet: for creating and as a
 * rename target. */
typedef struct target {
  char leaf[256];         /* last component as the client spelled it */
  char path[PATH_SIZE];   /* the existing entry (in its spelling), else new */
  char rel[PATH_SIZE];
  char asked[PATH_SIZE];  /* parent plus leaf in the client's spelling */
  char asked_rel[PATH_SIZE];
  int exists;
  struct stat st;         /* of the existing entry */
} target_t;


/* Resolves the parent directory of name, which must exist, and finds the
 * last component in it, ignoring case. The share root itself exists and
 * has an empty leaf. Returns 0 or an NTSTATUS. */
static uint32_t
resolve_target(const char *name, target_t *t) {
  char parent[PATH_SIZE];
  char buf[PATH_SIZE];
  char actual[256];
  char *leaf;
  uint32_t status;
  size_t len;

  memset(t, 0, sizeof *t);
  while(name && *name == '\\') {
    name++;
  }
  if(snprintf(buf, sizeof buf, "%s", name ? name : "") >= (int)sizeof buf) {
    return SMB2_STATUS_OBJECT_NAME_INVALID;
  }
  len = strlen(buf);
  if(len >= 7 && !strcasecmp(buf + len - 7, "::$DATA")) {
    buf[len -= 7] = '\0';
  }
  while(len && buf[len - 1] == '\\') {
    buf[--len] = '\0';
  }
  if(!len) {
    t->exists = 1;
    return resolve(NULL, t->path, t->rel, &t->st);
  }

  if((leaf=strrchr(buf, '\\'))) {
    *leaf++ = '\0';
  } else {
    leaf = buf;
  }
  if(!valid_component(leaf) || strlen(leaf) >= sizeof t->leaf) {
    return SMB2_STATUS_OBJECT_NAME_INVALID;
  }
  snprintf(t->leaf, sizeof t->leaf, "%s", leaf);

  status = resolve(leaf == buf ? NULL : buf, parent, t->rel, &t->st);
  if(status == SMB2_STATUS_OBJECT_NAME_NOT_FOUND) {
    return SMB2_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  if(status) {
    return status;
  }
  if(!S_ISDIR(t->st.st_mode)) {
    return SMB2_STATUS_OBJECT_PATH_NOT_FOUND;
  }
  len = strlen(t->rel);
  if(snprintf(t->asked, sizeof t->asked, "%s%s%s", parent, sep(parent), t->leaf) >= (int)sizeof t->asked ||
     snprintf(t->asked_rel, sizeof t->asked_rel, "%s%s%s", t->rel, len ? "\\" : "",
              t->leaf) >= (int)sizeof t->asked_rel) {
    return SMB2_STATUS_OBJECT_NAME_INVALID;
  }

  if(stat(t->asked, &t->st) == 0) {
    t->exists = 1;
    snprintf(actual, sizeof actual, "%s", t->leaf);
  } else if(errno != ENOENT) {
    return errno_status(errno);
  } else if(find_ci(parent, t->leaf, actual, sizeof actual)) {
    char path[PATH_SIZE];

    if(snprintf(path, sizeof path, "%s%s%s", parent, sep(parent), actual) >= (int)sizeof path) {
      return SMB2_STATUS_OBJECT_NAME_INVALID;
    }
    if(stat(path, &t->st) != 0) {
      return errno_status(errno);
    }
    t->exists = 1;
  } else {
    snprintf(actual, sizeof actual, "%s", t->leaf);
  }
  if(snprintf(t->path, sizeof t->path, "%s%s%s", parent, sep(parent), actual) >= (int)sizeof t->path ||
     snprintf(t->rel + len, sizeof t->rel - len, "%s%s", len ? "\\" : "", actual)
     >= (int)(sizeof t->rel - len)) {
    return SMB2_STATUS_OBJECT_NAME_INVALID;
  }
  return 0;
}


static int
dir_is_empty(const char *path) {
  struct dirent *e;
  DIR *d;
  int empty = 1;

  if(!(d=opendir(path))) {
    return 0;
  }
  while(empty && (e=readdir(d))) {
    if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) {
      empty = 0;
    }
  }
  closedir(d);
  return empty;
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


/* A pending delete happens when the handle goes, also when the client
 * disconnects without closing it. Nothing can fail at that point, so a
 * failed delete only shows in the status. */
static void
finish_delete(handle_t *h) {
  if(!h->delete_pending) {
    return;
  }
  h->delete_pending = 0;
  if(h->fd >= 0) {
    close(h->fd);
    h->fd = -1;
  }
  if((h->is_dir ? rmdir(h->path) : unlink(h->path)) == 0) {
    g.deletes++;
    nc_note(h->path, 0);
  } else {
    g.delete_failures++;
    note("delete errno %d", errno);
  }
}


static uint32_t errno_status(int err);
static uint64_t now_ns(void);

static void *
write_thread(void *arg) {
  pending_write_t *w = arg;
  uint64_t t0 = now_ns();

  w->done = 0;
  w->status = 0;
  while(w->done < w->len) {
    ssize_t n = pwrite(w->fd, w->buf + w->done, w->len - w->done, (off_t)(w->offset + w->done));
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      w->status = n < 0 ? errno_status(errno) : SMB2_STATUS_DISK_FULL;
      break;
    }
    w->done += (size_t)n;
  }
  w->ns = now_ns() - t0;
  __atomic_store_n(&w->finished, 1, __ATOMIC_RELEASE);
  (void)!write(g_wake[1], "w", 1);
  return NULL;
}


/* Waits for the write in the thread, if any, and sends its reply. */
static void
write_finish(void) {
  struct smb2_pdu *pdu = NULL;
  uint64_t t0;

  if(!g_pw.active) {
    return;
  }
  t0 = now_ns();
  pthread_join(g_pw.thread, NULL);
  g.write_wait_ns += now_ns() - t0;
  g_pw.active = 0;
  {
    char b;
    (void)!read(g_wake[0], &b, 1);      /* written before the thread ended */
  }
  g.write_ns += g_pw.ns;
  free(g_pw.buf);
  g_pw.buf = NULL;
  if(!g_pw.smb2) {
    return;
  }
  if(g_pw.done) {
    struct smb2_write_reply rep;

    g.writes++;
    g.writes_async++;
    g.bytes_written += g_pw.done;
    memset(&rep, 0, sizeof rep);
    rep.count = (uint32_t)g_pw.done;    /* a short count tells the client the rest failed */
    pdu = smb2_cmd_write_reply_async(g_pw.smb2, &rep, NULL, NULL);
  } else {
    struct smb2_error_reply err;

    memset(&err, 0, sizeof err);
    pdu = smb2_cmd_error_reply_async(g_pw.smb2, &err, SMB2_WRITE, (int)g_pw.status, NULL, NULL);
  }
  if(!pdu) {
    g.lost_replies++;
    return;
  }
  smb2_set_pdu_message_id(g_pw.smb2, pdu, g_pw.message_id);
  smb2_queue_pdu(g_pw.smb2, pdu);
}


/* From the main loop: sends the reply once the write is done. Never
 * waits: waiting here stopped the receiving between two parts of the
 * next request, and the writes ran one after the other again. */
static void
write_poll(void) {
  if(g_pw.active && __atomic_load_n(&g_pw.finished, __ATOMIC_ACQUIRE)) {
    write_finish();
  }
}


void
backend_fdset(fd_set *rfds, int *maxfd) {
  if(g_pw.active && g_wake[0] >= 0) {
    FD_SET(g_wake[0], rfds);
    if(g_wake[0] > *maxfd) {
      *maxfd = g_wake[0];
    }
  }
}


/* Related compound operations carry the all-0xFF id and refer to the last
 * CREATE; if that failed, they fail with its status. */
static handle_t *
lookup(conn_t *c, const uint8_t *fid, uint32_t *status) {
  write_finish();
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


/* Send buffer chosen at run time (P2.4 measurement), 0 = the profile's. */
static int g_sndbuf;


void
backend_set_sndbuf(int bytes) {
  g_sndbuf = bytes;
  for(conn_t *c = g.conns; c; c = c->next) {
    int fd = smb2_get_fd(c->smb2);
    int v = bytes ? bytes : g.profile->sockbuf;

    if(fd >= 0 && v) {
      setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &v, sizeof v);
    }
  }
}


void
backend_attach(struct smb2_context *smb2) {
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
  if(g_sndbuf) {
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &g_sndbuf, sizeof g_sndbuf);
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
backend_listener(int fd) {
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
backend_set_profile(char name) {
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
backend_reset_stats(void) {
  g.reads = 0;
  g.bytes_read = 0;
  g.read_ns = 0;
  g.first_read_ns = 0;
  g.last_read_ns = 0;
  g.writes = 0;
  g.bytes_written = 0;
  g.write_ns = 0;
  g.fsyncs = 0;
  g.fsync_ns = 0;
  g.writes_async = 0;
  g.write_wait_ns = 0;
  getrusage(RUSAGE_SELF, &g.ru_start);
  g.stats_start_ns = now_ns();
}


static int
destruction_event(struct smb2_server *srv, struct smb2_context *smb2) {
  conn_t *c = smb2_get_opaque(smb2);
  const char *err = smb2_get_error(smb2);
  int j = 0;

  (void)srv;
  if(g_pw.active && g_pw.smb2 == smb2) {
    g_pw.smb2 = NULL;     /* no reply, but the file stays open until written */
  }
  write_finish();
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
      finish_delete(h);
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
    rep->maximal_access = g.writable ? FULL_ACCESS : READ_ACCESS;
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
      finish_delete(h);
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
        put32(data + 4, g.writable ? FULL_ACCESS : READ_ACCESS); /* QueryStatus 0 */
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


/* Read-only share: opens existing entries for reading, anything else is
 * denied. */
static uint32_t
open_ro(handle_t *h, const struct smb2_create_request *req, struct stat *st) {
  uint32_t status;

  if((req->desired_access & WRITE_ACCESS) ||
     (req->create_options & SMB2_FILE_DELETE_ON_CLOSE)) {
    g.denied_writes++;
    return SMB2_STATUS_ACCESS_DENIED;
  }
  status = resolve(req->name, h->path, h->rel, st);
  if(status == SMB2_STATUS_OBJECT_NAME_NOT_FOUND &&
     req->create_disposition != FILE_OPEN &&
     req->create_disposition != FILE_OVERWRITE) {
    g.denied_writes++;
    return SMB2_STATUS_ACCESS_DENIED;     /* would create a file */
  }
  if(status) {
    return status;
  }
  if(req->create_disposition == FILE_CREATE) {
    return STATUS_OBJECT_NAME_COLLISION;
  }
  if(req->create_disposition != FILE_OPEN &&
     req->create_disposition != FILE_OPEN_IF) {
    g.denied_writes++;
    return SMB2_STATUS_ACCESS_DENIED;     /* supersede or overwrite */
  }
  h->is_dir = S_ISDIR(st->st_mode);
  if(h->is_dir && (req->create_options & SMB2_FILE_NON_DIRECTORY_FILE)) {
    return SMB2_STATUS_FILE_IS_A_DIRECTORY;
  }
  if(!h->is_dir && (req->create_options & SMB2_FILE_DIRECTORY_FILE)) {
    return SMB2_STATUS_NOT_A_DIRECTORY;
  }
  if(!h->is_dir && !S_ISREG(st->st_mode)) {
    g.special_refused++;
    return SMB2_STATUS_ACCESS_DENIED;
  }
  if(!h->is_dir && (h->fd=open(h->path, O_RDONLY)) < 0) {
    return errno_status(errno);
  }
  return 0;
}


/* Read-write share: all dispositions of MS-SMB2 2.2.13. Sets the create
 * action. */
static uint32_t
open_rw(handle_t *h, const struct smb2_create_request *req, struct stat *st,
        uint32_t *action) {
  uint32_t disp = req->create_disposition;
  uint32_t opts = req->create_options;
  int writing = (req->desired_access & DATA_WRITE_ACCESS) != 0;
  uint32_t status;
  target_t t;

  if((opts & SMB2_FILE_DELETE_ON_CLOSE) &&
     !(req->desired_access & (DELETE | GENERIC_ALL | MAXIMUM_ALLOWED))) {
    return SMB2_STATUS_ACCESS_DENIED;
  }
  if((status=resolve_target(req->name, &t))) {
    return status;
  }
  snprintf(h->path, sizeof h->path, "%s", t.path);
  snprintf(h->rel, sizeof h->rel, "%s", t.rel);

  if(t.exists) {
    int truncate = disp == FILE_SUPERSEDE || disp == FILE_OVERWRITE ||
                   disp == FILE_OVERWRITE_IF;

    h->is_dir = S_ISDIR(t.st.st_mode);
    if(disp == FILE_CREATE) {
      return STATUS_OBJECT_NAME_COLLISION;
    }
    if(h->is_dir && (opts & SMB2_FILE_NON_DIRECTORY_FILE)) {
      return SMB2_STATUS_FILE_IS_A_DIRECTORY;
    }
    if(!h->is_dir && (opts & SMB2_FILE_DIRECTORY_FILE)) {
      return SMB2_STATUS_NOT_A_DIRECTORY;
    }
    if(h->is_dir && truncate) {
      return SMB2_STATUS_INVALID_PARAMETER;
    }
    if(!h->is_dir && !S_ISREG(t.st.st_mode)) {
      g.special_refused++;
      return SMB2_STATUS_ACCESS_DENIED;
    }
    if(!h->rel[0] && (opts & SMB2_FILE_DELETE_ON_CLOSE)) {
      return SMB2_STATUS_ACCESS_DENIED;    /* the share root stays */
    }
    if(h->is_dir && (opts & SMB2_FILE_DELETE_ON_CLOSE) && !dir_is_empty(h->path)) {
      return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
    }
    if(!h->is_dir) {
      h->can_write = writing || truncate;
      h->fd = open(h->path, (h->can_write ? O_RDWR : O_RDONLY) |
                            (truncate ? O_TRUNC : 0));
      /* MAXIMUM_ALLOWED alone asks for whatever is possible: a file that
       * cannot be written is still opened for reading. */
      if(h->fd < 0 && !truncate && (errno == EACCES || errno == EPERM || errno == EROFS) &&
         !(req->desired_access & (DATA_WRITE_ACCESS & ~MAXIMUM_ALLOWED))) {
        h->can_write = 0;
        h->fd = open(h->path, O_RDONLY);
      }
      if(h->fd < 0) {
        return errno_status(errno);
      }
      if(truncate) {
        g.truncates++;
      }
    }
    *action = disp == FILE_SUPERSEDE ? FILE_SUPERSEDED :
              truncate ? FILE_OVERWRITTEN : FILE_OPENED;
  } else {
    if(disp == FILE_OPEN || disp == FILE_OVERWRITE) {
      return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if(opts & SMB2_FILE_DIRECTORY_FILE) {
      if(mkdir(h->path, 0777) != 0) {
        return errno_status(errno);
      }
      h->is_dir = 1;
      g.dirs_created++;
    } else {
      if((h->fd=open(h->path, O_RDWR | O_CREAT | O_EXCL, 0666)) < 0) {
        return errno_status(errno);
      }
      h->can_write = 1;
      g.files_created++;
    }
    *action = FILE_CREATED;
    nc_note(h->path, 1);
    if(stat(h->path, &g.last_created_st) == 0) {
      snprintf(g.last_created, sizeof g.last_created, "%s", h->rel);
    }
  }
  h->write_through = (opts & SMB2_FILE_WRITE_THROUGH) != 0;
  h->delete_pending = (opts & SMB2_FILE_DELETE_ON_CLOSE) != 0;
  if(stat(h->path, st) != 0) {
    return errno_status(errno);
  }
  return 0;
}


static uint32_t
do_create(conn_t *c, struct smb2_create_request *req,
          struct smb2_create_reply *rep) {
  uint32_t action = FILE_OPENED;
  uint32_t tree_id = 0;
  uint32_t status;
  struct stat st;
  handle_t *h;
  int kind = tree_kind(c, &tree_id);

  memset(&st, 0, sizeof st);
  if(kind == TREE_IPC) {
    note("CREATE on IPC$ %.30s", req->name ? req->name : "");
    return SMB2_STATUS_OBJECT_NAME_NOT_FOUND;
  }
  if(kind != TREE_DISK) {
    return STATUS_NETWORK_NAME_DELETED;
  }
  if(!(h=calloc(1, sizeof *h))) {
    return SMB2_STATUS_NO_MEMORY;
  }
  h->fd = -1;
  status = g.writable ? open_rw(h, req, &st, &action) : open_ro(h, req, &st);
  if(status) {
    free_handle(h);
    return status;
  }

  h->tree_id = tree_id;
  c->next_fid++;
  put64(h->fid, c->next_fid);
  put64(h->fid + 8, c->next_fid ^ 0x5053355350533553ULL);
  h->next = c->handles;
  c->handles = h;

  memset(rep, 0, sizeof *rep);
  rep->create_action = action;
  rep->creation_time = filetime(birth(&st));
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
  write_finish();
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
  finish_delete(h);
  if((req->flags & SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB) && stat(h->path, &st) == 0) {
    rep->flags = SMB2_CLOSE_FLAG_POSTQUERY_ATTRIB;
    rep->creation_time = filetime(birth(&st));
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

/* A pattern without wildcards names one entry. */
static int
is_literal(const char *pattern) {
  return !strpbrk(pattern, "*?<>\"");
}


/* The snapshot holds names only; each entry is stat()ed when a reply
 * carries it (encode_listed). A folder of 100,000 files then costs one
 * readdir() pass, not 100,000 stat() calls in a single request, which
 * stalled the whole server loop (live, 25.10.2026). A name without
 * wildcards, as the Explorer sends before it deletes a file, is looked
 * up directly instead of by reading the whole folder. */
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
  if(is_literal(pattern) && strcmp(pattern, ".") && strcmp(pattern, "..")) {
    char actual[256];
    char path[PATH_SIZE];
    dir_entry_t *ent = &h->entries[0];

    if(!valid_component(pattern) || strlen(pattern) >= sizeof actual) {
      return 0;
    }
    snprintf(actual, sizeof actual, "%s", pattern);
    if(snprintf(path, sizeof path, "%s%s%s", h->path, sep(h->path), actual) >= (int)sizeof path) {
      return 0;
    }
    if(stat(path, &ent->st) != 0) {
      if(errno != ENOENT || !find_ci(h->path, pattern, actual, sizeof actual) ||
         snprintf(path, sizeof path, "%s%s%s", h->path, sep(h->path), actual) >= (int)sizeof path ||
         stat(path, &ent->st) != 0) {
        return 0;
      }
    }
    if((ent->name=strdup(actual))) {
      ent->have_st = 1;
      h->nentries = 1;
    }
    return 0;
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
      h->entries[h->nentries].have_st = 1;
      h->nentries++;
    }
  }
  while((e=readdir(d))) {
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
    if(strlen(h->path) + 1 + strlen(e->d_name) >= PATH_SIZE || !(ent->name=strdup(e->d_name))) {
      continue;
    }
    ent->have_st = 0;
    h->nentries++;
  }
  closedir(d);
  return 0;
}


/* Fills in st of a listed entry. Returns 0 if it is gone since the
 * snapshot, then the entry is skipped. */
static int
stat_listed(const handle_t *h, dir_entry_t *ent) {
  char path[PATH_SIZE];

  if(ent->have_st) {
    return 1;
  }
  if(snprintf(path, sizeof path, "%s%s%s", h->path, sep(h->path), ent->name) >= (int)sizeof path ||
     stat(path, &ent->st) != 0) {
    return 0;
  }
  ent->have_st = 1;
  return 1;
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
    if(!stat_listed(h, &h->entries[h->pos])) {
      h->pos++;
      continue;
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
    if(h->pos >= h->nentries) {
      /* Everything left was deleted since the snapshot. */
      return fresh ? defer_status(smb2, SMB2_QUERY_DIRECTORY, SMB2_STATUS_NO_SUCH_FILE) : 0;
    }
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
    buf[20] = (uint8_t)h->delete_pending;
    buf[21] = h->is_dir;
    return 24;
  case SMB2_FILE_INTERNAL_INFORMATION:
    put64(buf, (uint64_t)st->st_ino);
    return 8;
  case SMB2_FILE_EA_INFORMATION:
    return 4;
  case SMB2_FILE_ACCESS_INFORMATION:
    put32(buf, g.writable ? FULL_ACCESS : READ_ACCESS);
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
    buf[60] = (uint8_t)h->delete_pending;
    buf[61] = h->is_dir;
    put64(buf + 64, (uint64_t)st->st_ino);
    put32(buf + 76, g.writable ? FULL_ACCESS : READ_ACCESS);
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


/* Sizes come from the file system of the open handle, not of the share
 * root: on the console "/" is a small system partition while /data holds
 * the free space, and Windows checks the free space of the target folder
 * before it copies. The volume identity stays that of the root, one
 * volume per share as Windows expects. */
static size_t
fs_info(uint8_t cls, const char *path, uint8_t *buf, size_t size, uint32_t *status) {
  struct statvfs vfs;
  uint64_t unit;

  memset(buf, 0, size);
  if(statvfs(path, &vfs) != 0 && statvfs(g.root, &vfs) != 0) {
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
               (g.writable ? 0 : FILE_READ_ONLY_VOLUME));
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
    len = fs_info(req->file_info_class, h->path, buf, sizeof buf, &status);
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


/* ---------------------------------------------------------------- writing */

static uint32_t
sync_fd(int fd) {
  uint64_t t0 = now_ns();
  int rc = fsync(fd);

  g.fsync_ns += now_ns() - t0;
  g.fsyncs++;
  return rc == 0 ? 0 : errno_status(errno);
}


static int
write_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_write_request *req, struct smb2_write_reply *rep) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  uint64_t offset = req->offset;
  size_t done = 0;
  uint64_t t0;

  if(!g.writable) {
    g.denied_writes++;
    return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
  }
  if(!h) {
    return reply_status(smb2, SMB2_WRITE, status);
  }
  if(h->is_dir) {
    return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_DEVICE_REQUEST);
  }
  if(!h->can_write) {
    return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_ACCESS_DENIED);
  }
  if(req->length > srv->max_write_size) {
    return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
  }
  if(req->length && req->data_offset != WRITE_DATA_OFFSET) {
    note("WRITE data offset %u", req->data_offset);
    return reply_status(smb2, SMB2_WRITE, SMB2_STATUS_INVALID_PARAMETER);
  }
  if(offset == WRITE_TO_END_OF_FILE) {
    struct stat st;

    if(fstat(h->fd, &st) != 0) {
      return reply_status(smb2, SMB2_WRITE, errno_status(errno));
    }
    offset = (uint64_t)st.st_size;
  }

  if(g_write_thread_ok && g_wake[0] < 0 && pipe(g_wake) != 0) {
    g_wake[0] = g_wake[1] = -1;
    g_write_thread_ok = 0;
    note("write thread: no pipe");
  }
  if(g_write_thread_ok && req->length >= ASYNC_WRITE_MIN && !h->write_through &&
     !(req->flags & SMB2_WRITEFLAG_WRITE_THROUGH)) {
    /* The request buffer is reused for the next request, so a copy. */
    uint8_t *copy = malloc(req->length);

    if(copy) {
      memcpy(copy, req->buf, req->length);
      memset(&g_pw, 0, sizeof g_pw);
      g_pw.smb2 = smb2;
      g_pw.message_id = smb2_get_last_request_message_id(smb2);
      g_pw.fd = h->fd;
      g_pw.buf = copy;
      g_pw.len = req->length;
      g_pw.offset = offset;
      if(pthread_create(&g_pw.thread, NULL, write_thread, &g_pw) == 0) {
        g_pw.active = 1;
        return 1;         /* replied by write_finish() */
      }
      free(copy);
      g_pw.buf = NULL;
      g_write_thread_ok = 0;
      note("write thread not available");
    }
  }

  t0 = now_ns();
  while(done < req->length) {
    ssize_t n = pwrite(h->fd, req->buf + done, req->length - done,
                       (off_t)(offset + done));
    if(n < 0 && errno == EINTR) {
      continue;
    }
    if(n <= 0) {
      status = n < 0 ? errno_status(errno) : SMB2_STATUS_DISK_FULL;
      break;
    }
    done += (size_t)n;
  }
  g.write_ns += now_ns() - t0;
  if(!done && req->length) {
    return reply_status(smb2, SMB2_WRITE, status);
  }
  g.writes++;
  g.bytes_written += done;
  if((h->write_through || (req->flags & SMB2_WRITEFLAG_WRITE_THROUGH)) &&
     (status=sync_fd(h->fd))) {
    return reply_status(smb2, SMB2_WRITE, status);
  }
  memset(rep, 0, sizeof *rep);
  rep->count = (uint32_t)done;    /* a short count tells the client the rest failed */
  return 0;
}


static int
flush_cmd(struct smb2_server *srv, struct smb2_context *smb2,
          struct smb2_flush_request *req) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;

  (void)srv;
  if(!h) {
    return reply_status(smb2, SMB2_FLUSH, status);
  }
  if(h->fd >= 0 && h->can_write && (status=sync_fd(h->fd))) {
    return reply_status(smb2, SMB2_FLUSH, status);
  }
  return 0;
}


/* Keeps open handles pointing at the right place after a rename: the
 * entry itself and, for a directory, everything below it. */
static void
renamed(const char *from, const char *to, const char *to_rel) {
  size_t flen = strlen(from);

  for(conn_t *c=g.conns; c; c=c->next) {
    for(handle_t *h=c->handles; h; h=h->next) {
      char path[PATH_SIZE];
      char rel[PATH_SIZE];
      const char *tail;

      if(strncmp(h->path, from, flen) || (h->path[flen] && h->path[flen] != '/')) {
        continue;
      }
      tail = h->path + flen;
      if(snprintf(path, sizeof path, "%s%s", to, tail) >= (int)sizeof path ||
         snprintf(rel, sizeof rel, "%s", to_rel) >= (int)sizeof rel) {
        continue;
      }
      /* The share-relative tail is the host tail with '\' for '/'. */
      for(size_t i=strlen(rel), j=0; tail[j] && i + 1 < sizeof rel; i++, j++) {
        rel[i] = tail[j] == '/' ? '\\' : tail[j];
        rel[i + 1] = '\0';
      }
      memcpy(h->path, path, sizeof path);
      memcpy(h->rel, rel, sizeof rel);
    }
  }
}


/* FILE_RENAME_INFORMATION (MS-FSCC 2.4.42.2) and the _EX variant: the
 * first byte (ReplaceIfExists, or bit 0 of Flags) asks to replace; the new
 * name is a full share-relative path. */
static uint32_t
set_rename(handle_t *h, const uint8_t *buf, uint32_t len) {
  uint16_t name16[PATH_SIZE];
  uint32_t name_len;
  uint32_t status;
  struct stat src;
  const char *name;
  int replace;
  target_t t;

  if(len < 20) {
    return SMB2_STATUS_INFO_LENGTH_MISMATCH;
  }
  replace = buf[0] & 1;
  name_len = get32(buf + 16);
  if(name_len % 2 || name_len > sizeof name16 || 20 + name_len > len ||
     get64(buf + 8)) {             /* RootDirectory must be 0 over SMB2 */
    return SMB2_STATUS_INVALID_PARAMETER;
  }
  if(!h->rel[0]) {
    return SMB2_STATUS_ACCESS_DENIED;      /* the share root stays */
  }
  memcpy(name16, buf + 20, name_len);
  if(!(name=smb2_utf16_to_utf8(name16, name_len / 2))) {
    return SMB2_STATUS_NO_MEMORY;
  }
  status = resolve_target(name, &t);
  free((void *)name);
  if(status) {
    return status;
  }
  if(lstat(h->path, &src) != 0) {
    return errno_status(errno);
  }
  if(t.exists && !(t.st.st_ino == src.st_ino && t.st.st_dev == src.st_dev)) {
    if(!replace) {
      return STATUS_OBJECT_NAME_COLLISION;
    }
    if(S_ISDIR(t.st.st_mode)) {
      return SMB2_STATUS_ACCESS_DENIED;    /* a directory is never replaced */
    }
  }
  /* The client's spelling: for the same entry this is a change of case. */
  if(!strcmp(t.asked, h->path)) {
    return 0;
  }
  if(rename(h->path, t.asked) != 0) {
    return errno_status(errno);
  }
  g.renames++;
  nc_note(h->path, 0);
  nc_note(t.asked, 1);
  {
    char from[PATH_SIZE];

    memcpy(from, h->path, sizeof from);
    renamed(from, t.asked, t.asked_rel);
  }
  return 0;
}


/* FILE_BASIC_INFORMATION: access and write time are applied; creation time
 * and attributes cannot be stored and are only counted.
 * utimes(), not utimensat(): the console's libkernel has no utimensat, and
 * the SDK's libc.a stub for it is a bare FreeBSD syscall number, which on
 * the console may be a different call (its result came back in place of
 * errno in S3). So times are kept to the microsecond. */
static uint32_t
set_basic(handle_t *h, const uint8_t *buf, uint32_t len) {
  struct timeval tv[2];
  struct stat st;
  uint64_t created;
  uint32_t attrs;
  int given;

  if(len < 36) {
    return SMB2_STATUS_INFO_LENGTH_MISMATCH;
  }
  created = get64(buf);
  attrs = get32(buf + 32);
  if(created && created < UINT64_MAX - 1) {
    g.creation_times_ignored++;
  }
  if(attrs & ~(uint32_t)(SMB2_FILE_ATTRIBUTE_DIRECTORY | SMB2_FILE_ATTRIBUTE_ARCHIVE |
                         SMB2_FILE_ATTRIBUTE_NORMAL)) {
    g.attributes_ignored++;
  }
  if(stat(h->path, &st) != 0) {
    return errno_status(errno);
  }
  tv[0].tv_sec = st.st_atim.tv_sec;
  tv[0].tv_usec = (suseconds_t)(st.st_atim.tv_nsec / 1000);
  tv[1].tv_sec = st.st_mtim.tv_sec;
  tv[1].tv_usec = (suseconds_t)(st.st_mtim.tv_nsec / 1000);
  given = from_filetime(get64(buf + 8), &tv[0]);
  given |= from_filetime(get64(buf + 16), &tv[1]);
  if(!given) {
    return 0;
  }
  if(utimes(h->path, tv) != 0) {
    return errno_status(errno);
  }
  g.times_set++;
  return 0;
}


static uint32_t
set_size(handle_t *h, uint64_t size, int allocation) {
  struct stat st;

  if(h->is_dir) {
    return SMB2_STATUS_INVALID_PARAMETER;
  }
  if(!h->can_write) {
    return SMB2_STATUS_ACCESS_DENIED;
  }
  if(size > INT64_MAX) {
    return SMB2_STATUS_INVALID_PARAMETER;
  }
  /* An allocation size only shrinks the file when it is below its end;
   * reserving space is left to the file system. */
  if(allocation) {
    if(fstat(h->fd, &st) != 0) {
      return errno_status(errno);
    }
    if(size >= (uint64_t)st.st_size) {
      return 0;
    }
  }
  if(ftruncate(h->fd, (off_t)size) != 0) {
    return errno_status(errno);
  }
  g.truncates++;
  return 0;
}


static uint32_t
set_delete(handle_t *h, int pending) {
  if(pending && !h->rel[0]) {
    return SMB2_STATUS_ACCESS_DENIED;      /* the share root stays */
  }
  if(pending && h->is_dir && !dir_is_empty(h->path)) {
    return SMB2_STATUS_DIRECTORY_NOT_EMPTY;
  }
  h->delete_pending = pending;
  return 0;
}


static int
set_info_cmd(struct smb2_server *srv, struct smb2_context *smb2,
             struct smb2_set_info_request *req) {
  conn_t *c = smb2_get_opaque(smb2);
  uint32_t status = SMB2_STATUS_FILE_CLOSED;
  handle_t *h = c ? lookup(c, req->file_id, &status) : NULL;
  const uint8_t *buf = req->input_data;
  uint32_t len = req->buffer_length;

  (void)srv;
  if(!g.writable) {
    g.denied_writes++;
    return reply_status(smb2, SMB2_SET_INFO, SMB2_STATUS_ACCESS_DENIED);
  }
  if(!h) {
    return reply_status(smb2, SMB2_SET_INFO, status);
  }
  if(len && (!buf || req->buffer_offset != SET_INFO_DATA_OFFSET)) {
    note("SET_INFO buffer offset %u", req->buffer_offset);
    return reply_status(smb2, SMB2_SET_INFO, SMB2_STATUS_INVALID_PARAMETER);
  }
  if(req->info_type != SMB2_0_INFO_FILE) {
    note("SET_INFO type %u", req->info_type);
    return reply_status(smb2, SMB2_SET_INFO, SMB2_STATUS_NOT_SUPPORTED);
  }

  switch(req->file_info_class) {
  case SMB2_FILE_BASIC_INFORMATION:
    status = set_basic(h, buf, len);
    break;
  case SMB2_FILE_RENAME_INFORMATION:
  case FILE_RENAME_INFORMATION_EX:
    status = set_rename(h, buf, len);
    break;
  case SMB2_FILE_DISPOSITION_INFORMATION:
    status = len < 1 ? SMB2_STATUS_INFO_LENGTH_MISMATCH : set_delete(h, buf[0] != 0);
    break;
  case FILE_DISPOSITION_INFORMATION_EX:
    status = len < 4 ? SMB2_STATUS_INFO_LENGTH_MISMATCH
                     : set_delete(h, (get32(buf) & FILE_DISPOSITION_DELETE) != 0);
    break;
  case SMB2_FILE_END_OF_FILE_INFORMATION:
  case SMB2_FILE_ALLOCATION_INFORMATION:
    status = len < 8 ? SMB2_STATUS_INFO_LENGTH_MISMATCH
                     : set_size(h, get64(buf),
                                req->file_info_class == SMB2_FILE_ALLOCATION_INFORMATION);
    break;
  case SMB2_FILE_POSITION_INFORMATION:
  case SMB2_FILE_MODE_INFORMATION:
    status = 0;     /* SMB2 has no file position; modes change nothing here */
    break;
  default:
    note("SET_INFO file 0x%02x", req->file_info_class);
    status = SMB2_STATUS_NOT_SUPPORTED;
    break;
  }
  return status ? reply_status(smb2, SMB2_SET_INFO, status) : 0;
}


/* Byte-range locks are granted but not enforced (prototype). */
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
struct smb2_server_request_handlers backend_handlers = {
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


void
backend_set_clock_skew(int64_t seconds) {
  g_skew_s = seconds;
}


int
backend_init(const char *root, const char *share_name,
             const char *user, const char *password,
             int writable, char profile) {
  snprintf(g.root, sizeof g.root, "%s", root);
  snprintf(g.share, sizeof g.share, "%s", share_name);
  snprintf(g.user, sizeof g.user, "%s", user);
  snprintf(g.password, sizeof g.password, "%s", password);
  g.writable = writable;
  g.profile = &PROFILES[0];
  for(size_t i=0; i<sizeof PROFILES / sizeof PROFILES[0]; i++) {
    if(PROFILES[i].name == profile) {
      g.profile = &PROFILES[i];
    }
  }
  g.listener_fd = -1;
  backend_reset_stats();
  if(stat(g.root, &g.root_st) != 0) {
    return -errno;
  }
  return S_ISDIR(g.root_st.st_mode) ? 0 : -ENOTDIR;
}


int
backend_status(char *buf, size_t size) {
  struct rusage ru;
  uint64_t window = g.first_read_ns ? g.last_read_ns - g.first_read_ns : 0;
  int n;

  getrusage(RUSAGE_SELF, &ru);
  n = snprintf(buf, size,
               "profile=%c (%s)\ndialect=0x%04x\n"
               "auth_attempts=%u\nsessions=%u\ncreates=%u\ncreates_ok=%u\n"
               "denied_writes=%u\ndir_queries=%u\nlost_replies=%u\nspecial_refused=%u\n"
               "since_reset_ms=%llu\ncpu_user_ms=%llu\ncpu_sys_ms=%llu\n"
               "reads=%llu\nbytes_read=%llu\nread_window_ms=%llu\npread_ms=%llu\n",
               g.profile->name, g.profile->what, g.last_dialect,
               g.auth_attempts, g.sessions, g.creates, g.creates_ok,
               g.denied_writes, g.dir_queries, g.lost_replies, g.special_refused,
               (unsigned long long)((now_ns() - g.stats_start_ns) / 1000000),
               (unsigned long long)(tv_ms(&ru.ru_utime) - tv_ms(&g.ru_start.ru_utime)),
               (unsigned long long)(tv_ms(&ru.ru_stime) - tv_ms(&g.ru_start.ru_stime)),
               g.reads, g.bytes_read,
               (unsigned long long)(window / 1000000),
               (unsigned long long)(g.read_ns / 1000000));

  if(g.writable && n > 0 && (size_t)n < size) {
    const struct stat *st = &g.last_created_st;

    n += snprintf(buf + n, size - n,
                  "writes=%llu\nbytes_written=%llu\npwrite_ms=%llu\n"
                  "writes_async=%llu\nwrite_wait_ms=%llu\n"
                  "fsyncs=%llu\nfsync_ms=%llu\n"
                  "files_created=%u\ndirs_created=%u\nrenames=%u\ndeletes=%u\n"
                  "delete_failures=%u\ntruncates=%u\ntimes_set=%u\n"
                  "creation_times_ignored=%u\nattributes_ignored=%u\n"
                  "last_created=%s uid=%u gid=%u mode=%04o\n",
                  g.writes, g.bytes_written,
                  (unsigned long long)(g.write_ns / 1000000),
                  g.writes_async, (unsigned long long)(g.write_wait_ns / 1000000), g.fsyncs,
                  (unsigned long long)(g.fsync_ns / 1000000),
                  g.files_created, g.dirs_created, g.renames, g.deletes,
                  g.delete_failures, g.truncates, g.times_set,
                  g.creation_times_ignored, g.attributes_ignored,
                  g.last_created[0] ? g.last_created : "-",
                  (unsigned)st->st_uid, (unsigned)st->st_gid,
                  (unsigned)(st->st_mode & 07777));
  }
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
