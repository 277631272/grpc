/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/support/port_platform.h>

#ifdef GPR_POSIX_SOCKET

#include "src/core/iomgr/tcp_client.h"

#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>

#include "src/core/iomgr/alarm.h"
#include "src/core/iomgr/iomgr_posix.h"
#include "src/core/iomgr/pollset_posix.h"
#include "src/core/iomgr/sockaddr_utils.h"
#include "src/core/iomgr/socket_utils_posix.h"
#include "src/core/iomgr/tcp_posix.h"
#include "src/core/support/string.h"
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>
#include <grpc/support/time.h>

typedef struct {
  void (*cb)(void *arg, grpc_endpoint *tcp);
  void *cb_arg;
  gpr_mu mu;
  grpc_fd *fd;
  gpr_timespec deadline;
  grpc_alarm alarm;
  int refs;
  grpc_iomgr_closure write_closure;
  grpc_pollset_set *interested_parties;
  char *addr_str;
} async_connect;

static int prepare_socket(const struct sockaddr *addr, int fd) {
  if (fd < 0) {
    goto error;
  }

  if (!grpc_set_socket_nonblocking(fd, 1) || !grpc_set_socket_cloexec(fd, 1) ||
      (addr->sa_family != AF_UNIX && !grpc_set_socket_low_latency(fd, 1)) ||
      !grpc_set_socket_no_sigpipe_if_possible(fd)) {
    gpr_log(GPR_ERROR, "Unable to configure socket %d: %s", fd,
            strerror(errno));
    goto error;
  }

  return 1;

error:
  if (fd >= 0) {
    close(fd);
  }
  return 0;
}

static void tc_on_alarm(void *acp, int success) {
  int done;
  async_connect *ac = acp;
  gpr_mu_lock(&ac->mu);
  if (ac->fd != NULL) {
    grpc_fd_shutdown(ac->fd);
  }
  done = (--ac->refs == 0);
  gpr_mu_unlock(&ac->mu);
  if (done) {
    gpr_mu_destroy(&ac->mu);
    gpr_free(ac->addr_str);
    gpr_free(ac);
  }
}

static void on_writable(void *acp, int success) {
  async_connect *ac = acp;
  int so_error = 0;
  socklen_t so_error_size;
  int err;
  int done;
  grpc_endpoint *ep = NULL;
  void (*cb)(void *arg, grpc_endpoint *tcp) = ac->cb;
  void *cb_arg = ac->cb_arg;
  grpc_fd *fd;

  gpr_mu_lock(&ac->mu);
  GPR_ASSERT(ac->fd);
  fd = ac->fd;
  ac->fd = NULL;
  gpr_mu_unlock(&ac->mu);

  grpc_alarm_cancel(&ac->alarm);

  gpr_mu_lock(&ac->mu);
  if (success) {
    do {
      so_error_size = sizeof(so_error);
      err = getsockopt(fd->fd, SOL_SOCKET, SO_ERROR, &so_error, &so_error_size);
    } while (err < 0 && errno == EINTR);
    if (err < 0) {
      gpr_log(GPR_ERROR, "getsockopt(ERROR): %s", strerror(errno));
      goto finish;
    } else if (so_error != 0) {
      if (so_error == ENOBUFS) {
        /* We will get one of these errors if we have run out of
           memory in the kernel for the data structures allocated
           when you connect a socket.  If this happens it is very
           likely that if we wait a little bit then try again the
           connection will work (since other programs or this
           program will close their network connections and free up
           memory).  This does _not_ indicate that there is anything
           wrong with the server we are connecting to, this is a
           local problem.

           If you are looking at this code, then chances are that
           your program or another program on the same computer
           opened too many network connections.  The "easy" fix:
           don't do that! */
        gpr_log(GPR_ERROR, "kernel out of buffers");
        gpr_mu_unlock(&ac->mu);
        grpc_fd_notify_on_write(fd, &ac->write_closure);
        return;
      } else {
        switch (so_error) {
          case ECONNREFUSED:
            gpr_log(GPR_ERROR, "socket error: connection refused");
            break;
          default:
            gpr_log(GPR_ERROR, "socket error: %d", so_error);
            break;
        }
        goto finish;
      }
    } else {
      grpc_pollset_set_del_fd(ac->interested_parties, fd);
      ep = grpc_tcp_create(fd, GRPC_TCP_DEFAULT_READ_SLICE_SIZE, ac->addr_str);
      fd = NULL;
      goto finish;
    }
  } else {
    gpr_log(GPR_ERROR, "on_writable failed during connect");
    goto finish;
  }

  abort();

finish:
  if (fd != NULL) {
    grpc_pollset_set_del_fd(ac->interested_parties, fd);
    grpc_fd_orphan(fd, NULL, "tcp_client_orphan");
    fd = NULL;
  }
  done = (--ac->refs == 0);
  gpr_mu_unlock(&ac->mu);
  if (done) {
    gpr_mu_destroy(&ac->mu);
    gpr_free(ac->addr_str);
    gpr_free(ac);
  }
  cb(cb_arg, ep);
}

void grpc_tcp_client_connect(void (*cb)(void *arg, grpc_endpoint *ep),
                             void *arg, grpc_pollset_set *interested_parties,
                             const struct sockaddr *addr, size_t addr_len,
                             gpr_timespec deadline) {
  int fd;
  grpc_dualstack_mode dsmode;
  int err;
  async_connect *ac;
  struct sockaddr_in6 addr6_v4mapped;
  struct sockaddr_in addr4_copy;
  grpc_fd *fdobj;
  char *name;
  char *addr_str;

  /* Use dualstack sockets where available. */
  if (grpc_sockaddr_to_v4mapped(addr, &addr6_v4mapped)) {
    addr = (const struct sockaddr *)&addr6_v4mapped;
    addr_len = sizeof(addr6_v4mapped);
  }

  fd = grpc_create_dualstack_socket(addr, SOCK_STREAM, 0, &dsmode);
  if (fd < 0) {
    gpr_log(GPR_ERROR, "Unable to create socket: %s", strerror(errno));
  }
  if (dsmode == GRPC_DSMODE_IPV4) {
    /* If we got an AF_INET socket, map the address back to IPv4. */
    GPR_ASSERT(grpc_sockaddr_is_v4mapped(addr, &addr4_copy));
    addr = (struct sockaddr *)&addr4_copy;
    addr_len = sizeof(addr4_copy);
  }
  if (!prepare_socket(addr, fd)) {
    cb(arg, NULL);
    return;
  }

  do {
    GPR_ASSERT(addr_len < ~(socklen_t)0);
    err = connect(fd, addr, (socklen_t)addr_len);
  } while (err < 0 && errno == EINTR);

  addr_str = grpc_sockaddr_to_uri(addr);
  gpr_asprintf(&name, "tcp-client:%s", addr_str);

  fdobj = grpc_fd_create(fd, name);

  if (err >= 0) {
    cb(arg, grpc_tcp_create(fdobj, GRPC_TCP_DEFAULT_READ_SLICE_SIZE, addr_str));
    goto done;
  }

  if (errno != EWOULDBLOCK && errno != EINPROGRESS) {
    gpr_log(GPR_ERROR, "connect error to '%s': %s", addr_str, strerror(errno));
    grpc_fd_orphan(fdobj, NULL, "tcp_client_connect_error");
    cb(arg, NULL);
    goto done;
  }

  grpc_pollset_set_add_fd(interested_parties, fdobj);

  ac = gpr_malloc(sizeof(async_connect));
  ac->cb = cb;
  ac->cb_arg = arg;
  ac->fd = fdobj;
  ac->interested_parties = interested_parties;
  ac->addr_str = addr_str;
  addr_str = NULL;
  gpr_mu_init(&ac->mu);
  ac->refs = 2;
  ac->write_closure.cb = on_writable;
  ac->write_closure.cb_arg = ac;

  gpr_mu_lock(&ac->mu);
  grpc_alarm_init(&ac->alarm,
                  gpr_convert_clock_type(deadline, GPR_CLOCK_MONOTONIC),
                  tc_on_alarm, ac, gpr_now(GPR_CLOCK_MONOTONIC));
  grpc_fd_notify_on_write(ac->fd, &ac->write_closure);
  gpr_mu_unlock(&ac->mu);

done:
  gpr_free(name);
  gpr_free(addr_str);
}

#endif
