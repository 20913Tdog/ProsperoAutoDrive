/* Prospero AutoDrive - prospero-smbd: network interfaces and link type
 *
 * Copyright (C) 2026 Prospero AutoDrive contributors
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version. See ps5/LICENSE.
 *
 * Three hints, each printed, because which of them the console gives is
 * not known yet (spike of 24.10.2026, only libc and libkernel calls):
 *   media  SIOCGIFMEDIA: IFM_ETHER or IFM_IEEE80211 (FreeBSD)
 *   type   sdl_type of the AF_LINK address: IFT_ETHER or IFT_IEEE80211;
 *          FreeBSD reports wlan interfaces as IFT_ETHER too
 *   name   "wlan..." is Wi-Fi
 * The first one that says Wi-Fi or wired decides, in that order.
 */

#include "netinfo.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#ifdef __FreeBSD__
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#endif

#define MAX_IFS 16

typedef struct {
  char name[IFNAMSIZ];
  const char *media;            /* "ether", "wifi", "other" or "?" */
  int media_active;             /* 1, 0, or -1 if unknown */
  const char *type;             /* from AF_LINK: "ether", "wifi", "other", "?" */
} ifinfo_t;


static void
media_of(int s, ifinfo_t *i) {
  i->media = "?";
  i->media_active = -1;
#ifdef SIOCGIFMEDIA
  struct ifmediareq m;

  memset(&m, 0, sizeof m);
  snprintf(m.ifm_name, sizeof m.ifm_name, "%s", i->name);
  if(s >= 0 && ioctl(s, SIOCGIFMEDIA, &m) == 0) {
    int t = m.ifm_active & IFM_NMASK;

    i->media = t == IFM_ETHER ? "ether" : t == IFM_IEEE80211 ? "wifi" : "other";
    if(m.ifm_status & IFM_AVALID) {
      i->media_active = (m.ifm_status & IFM_ACTIVE) != 0;
    }
  }
#else
  (void)s;
#endif
}


static ifinfo_t *
find(ifinfo_t *ifs, int *n, const char *name) {
  for(int k=0; k<*n; k++) {
    if(!strcmp(ifs[k].name, name)) {
      return &ifs[k];
    }
  }
  if(*n >= MAX_IFS) {
    return NULL;
  }
  ifinfo_t *i = &ifs[(*n)++];
  snprintf(i->name, sizeof i->name, "%s", name);
  i->media = "?";
  i->media_active = -1;
  i->type = "?";
  return i;
}


static int
add(char *buf, size_t size, int n, const char *fmt, ...) {
  va_list ap;
  int k;

  if(n < 0 || (size_t)n >= size) {
    return n;
  }
  va_start(ap, fmt);
  k = vsnprintf(buf + n, size - (size_t)n, fmt, ap);
  va_end(ap);
  return k < 0 ? n : n + k;
}


int
netinfo_text(int fd, char *buf, size_t size) {
  struct sockaddr_in local;
  socklen_t len = sizeof local;
  struct ifaddrs *list = NULL;
  ifinfo_t ifs[MAX_IFS];
  int nifs = 0;
  char here[INET_ADDRSTRLEN] = "?";
  const char *link = "unknown";
  const char *basis = "none";
  const char *link_if = "?";
  int n = 0;
  int s;

  if(size == 0) {
    return 0;
  }
  buf[0] = '\0';
  memset(&local, 0, sizeof local);
  if(getsockname(fd, (struct sockaddr *)&local, &len) == 0 && local.sin_family == AF_INET) {
    inet_ntop(AF_INET, &local.sin_addr, here, sizeof here);
  }
  n = add(buf, size, n, "PROSPERO-NET OK\nlocal=%s\n", here);
  if(getifaddrs(&list) != 0) {
    return add(buf, size, n, "getifaddrs=failed\nlink=unknown basis=none\n");
  }
  for(struct ifaddrs *a=list; a; a=a->ifa_next) {
    if(!a->ifa_name || !a->ifa_addr) {
      continue;
    }
    ifinfo_t *i = find(ifs, &nifs, a->ifa_name);
#ifdef __FreeBSD__
    if(i && a->ifa_addr->sa_family == AF_LINK) {
      const struct sockaddr_dl *dl = (const struct sockaddr_dl *)a->ifa_addr;

      i->type = dl->sdl_type == IFT_ETHER ? "ether" :
                dl->sdl_type == IFT_IEEE80211 ? "wifi" : "other";
    }
#else
    (void)i;      /* Linux: no AF_LINK, the name is still listed */
#endif
  }
  s = socket(AF_INET, SOCK_DGRAM, 0);
  for(int k=0; k<nifs; k++) {
    media_of(s, &ifs[k]);
  }
  if(s >= 0) {
    close(s);
  }
  for(struct ifaddrs *a=list; a; a=a->ifa_next) {
    char addr[INET_ADDRSTRLEN];
    char flags[16];

    if(!a->ifa_name || !a->ifa_addr || a->ifa_addr->sa_family != AF_INET) {
      continue;
    }
    ifinfo_t *i = find(ifs, &nifs, a->ifa_name);
    if(!i) {
      continue;
    }
    const struct sockaddr_in *in = (const struct sockaddr_in *)a->ifa_addr;
    inet_ntop(AF_INET, &in->sin_addr, addr, sizeof addr);
    snprintf(flags, sizeof flags, "0x%x", (unsigned)a->ifa_flags);
    n = add(buf, size, n, "if=%s inet=%s flags=%s media=%s type=%s", i->name, addr, flags,
            i->media, i->type);
    n = add(buf, size, n, " media_active=%s\n",
            i->media_active < 0 ? "?" : i->media_active ? "1" : "0");
    if(in->sin_addr.s_addr == local.sin_addr.s_addr && local.sin_family == AF_INET) {
      link_if = i->name;
      if(!strcmp(i->media, "wifi") || !strcmp(i->media, "ether")) {
        link = !strcmp(i->media, "wifi") ? "wifi" : "wired";
        basis = "media";
      } else if(!strcmp(i->type, "wifi")) {
        link = "wifi";
        basis = "type";
      } else if(!strncmp(i->name, "wlan", 4)) {
        link = "wifi";
        basis = "name";
      }
    }
  }
  n = add(buf, size, n, "link=%s basis=%s if=%s\n", link, basis, link_if);
  freeifaddrs(list);
  return n < 0 ? 0 : ((size_t)n >= size ? (int)size - 1 : n);
}
