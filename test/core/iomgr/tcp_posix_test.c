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

#include "src/core/iomgr/tcp_posix.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>

#include <grpc/grpc.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "test/core/util/test_config.h"
#include "test/core/iomgr/endpoint_tests.h"

static grpc_pollset g_pollset;

/*
   General test notes:

   All tests which write data into a socket write i%256 into byte i, which is
   verified by readers.

   In general there are a few interesting things to vary which may lead to
   exercising different codepaths in an implementation:
   1. Total amount of data written to the socket
   2. Size of slice allocations
   3. Amount of data we read from or write to the socket at once

   The tests here tend to parameterize these where applicable.

 */

static void create_sockets(int sv[2]) {
  int flags;
  GPR_ASSERT(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  flags = fcntl(sv[0], F_GETFL, 0);
  GPR_ASSERT(fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) == 0);
  flags = fcntl(sv[1], F_GETFL, 0);
  GPR_ASSERT(fcntl(sv[1], F_SETFL, flags | O_NONBLOCK) == 0);
}

static ssize_t fill_socket(int fd) {
  ssize_t write_bytes;
  ssize_t total_bytes = 0;
  int i;
  unsigned char buf[256];
  for (i = 0; i < 256; ++i) {
    buf[i] = (gpr_uint8)i;
  }
  do {
    write_bytes = write(fd, buf, 256);
    if (write_bytes > 0) {
      total_bytes += write_bytes;
    }
  } while (write_bytes >= 0 || errno == EINTR);
  GPR_ASSERT(errno == EAGAIN);
  return total_bytes;
}

static size_t fill_socket_partial(int fd, size_t bytes) {
  ssize_t write_bytes;
  size_t total_bytes = 0;
  unsigned char *buf = malloc(bytes);
  unsigned i;
  for (i = 0; i < bytes; ++i) {
    buf[i] = (gpr_uint8)(i % 256);
  }

  do {
    write_bytes = write(fd, buf, bytes - total_bytes);
    if (write_bytes > 0) {
      total_bytes += (size_t)write_bytes;
    }
  } while ((write_bytes >= 0 || errno == EINTR) && bytes > total_bytes);

  gpr_free(buf);

  return total_bytes;
}

struct read_socket_state {
  grpc_endpoint *ep;
  size_t read_bytes;
  size_t target_read_bytes;
  gpr_slice_buffer incoming;
  grpc_iomgr_closure read_cb;
};

static size_t count_slices(gpr_slice *slices, size_t nslices,
                           int *current_data) {
  size_t num_bytes = 0;
  unsigned i, j;
  unsigned char *buf;
  for (i = 0; i < nslices; ++i) {
    buf = GPR_SLICE_START_PTR(slices[i]);
    for (j = 0; j < GPR_SLICE_LENGTH(slices[i]); ++j) {
      GPR_ASSERT(buf[j] == *current_data);
      *current_data = (*current_data + 1) % 256;
    }
    num_bytes += GPR_SLICE_LENGTH(slices[i]);
  }
  return num_bytes;
}

static void read_cb(void *user_data, int success) {
  struct read_socket_state *state = (struct read_socket_state *)user_data;
  size_t read_bytes;
  int current_data;

  GPR_ASSERT(success);

  gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
  current_data = state->read_bytes % 256;
  read_bytes = count_slices(state->incoming.slices, state->incoming.count,
                            &current_data);
  state->read_bytes += read_bytes;
  gpr_log(GPR_INFO, "Read %d bytes of %d", read_bytes,
          state->target_read_bytes);
  if (state->read_bytes >= state->target_read_bytes) {
    gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
  } else {
    switch (grpc_endpoint_read(state->ep, &state->incoming, &state->read_cb)) {
      case GRPC_ENDPOINT_DONE:
        gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
        read_cb(user_data, 1);
        break;
      case GRPC_ENDPOINT_ERROR:
        gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
        read_cb(user_data, 0);
        break;
      case GRPC_ENDPOINT_PENDING:
        gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
        break;
    }
  }
}

/* Write to a socket, then read from it using the grpc_tcp API. */
static void read_test(size_t num_bytes, size_t slice_size) {
  int sv[2];
  grpc_endpoint *ep;
  struct read_socket_state state;
  size_t written_bytes;
  gpr_timespec deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(20);

  gpr_log(GPR_INFO, "Read test of size %d, slice size %d", num_bytes,
          slice_size);

  create_sockets(sv);

  ep = grpc_tcp_create(grpc_fd_create(sv[1], "read_test"), slice_size, "test");
  grpc_endpoint_add_to_pollset(ep, &g_pollset);

  written_bytes = fill_socket_partial(sv[0], num_bytes);
  gpr_log(GPR_INFO, "Wrote %d bytes", written_bytes);

  state.ep = ep;
  state.read_bytes = 0;
  state.target_read_bytes = written_bytes;
  gpr_slice_buffer_init(&state.incoming);
  grpc_iomgr_closure_init(&state.read_cb, read_cb, &state);

  switch (grpc_endpoint_read(ep, &state.incoming, &state.read_cb)) {
    case GRPC_ENDPOINT_DONE:
      read_cb(&state, 1);
      break;
    case GRPC_ENDPOINT_ERROR:
      read_cb(&state, 0);
      break;
    case GRPC_ENDPOINT_PENDING:
      break;
  }

  gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
  while (state.read_bytes < state.target_read_bytes) {
    grpc_pollset_worker worker;
    grpc_pollset_work(&g_pollset, &worker, gpr_now(GPR_CLOCK_MONOTONIC),
                      deadline);
  }
  GPR_ASSERT(state.read_bytes == state.target_read_bytes);
  gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));

  gpr_slice_buffer_destroy(&state.incoming);
  grpc_endpoint_destroy(ep);
}

/* Write to a socket until it fills up, then read from it using the grpc_tcp
   API. */
static void large_read_test(size_t slice_size) {
  int sv[2];
  grpc_endpoint *ep;
  struct read_socket_state state;
  ssize_t written_bytes;
  gpr_timespec deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(20);

  gpr_log(GPR_INFO, "Start large read test, slice size %d", slice_size);

  create_sockets(sv);

  ep = grpc_tcp_create(grpc_fd_create(sv[1], "large_read_test"), slice_size,
                       "test");
  grpc_endpoint_add_to_pollset(ep, &g_pollset);

  written_bytes = fill_socket(sv[0]);
  gpr_log(GPR_INFO, "Wrote %d bytes", written_bytes);

  state.ep = ep;
  state.read_bytes = 0;
  state.target_read_bytes = (size_t)written_bytes;
  gpr_slice_buffer_init(&state.incoming);
  grpc_iomgr_closure_init(&state.read_cb, read_cb, &state);

  switch (grpc_endpoint_read(ep, &state.incoming, &state.read_cb)) {
    case GRPC_ENDPOINT_DONE:
      read_cb(&state, 1);
      break;
    case GRPC_ENDPOINT_ERROR:
      read_cb(&state, 0);
      break;
    case GRPC_ENDPOINT_PENDING:
      break;
  }

  gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
  while (state.read_bytes < state.target_read_bytes) {
    grpc_pollset_worker worker;
    grpc_pollset_work(&g_pollset, &worker, gpr_now(GPR_CLOCK_MONOTONIC),
                      deadline);
  }
  GPR_ASSERT(state.read_bytes == state.target_read_bytes);
  gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));

  gpr_slice_buffer_destroy(&state.incoming);
  grpc_endpoint_destroy(ep);
}

struct write_socket_state {
  grpc_endpoint *ep;
  int write_done;
};

static gpr_slice *allocate_blocks(size_t num_bytes, size_t slice_size,
                                  size_t *num_blocks, gpr_uint8 *current_data) {
  size_t nslices = num_bytes / slice_size + (num_bytes % slice_size ? 1u : 0u);
  gpr_slice *slices = gpr_malloc(sizeof(gpr_slice) * nslices);
  size_t num_bytes_left = num_bytes;
  unsigned i, j;
  unsigned char *buf;
  *num_blocks = nslices;

  for (i = 0; i < nslices; ++i) {
    slices[i] = gpr_slice_malloc(slice_size > num_bytes_left ? num_bytes_left
                                                             : slice_size);
    num_bytes_left -= GPR_SLICE_LENGTH(slices[i]);
    buf = GPR_SLICE_START_PTR(slices[i]);
    for (j = 0; j < GPR_SLICE_LENGTH(slices[i]); ++j) {
      buf[j] = *current_data;
      (*current_data)++;
    }
  }
  GPR_ASSERT(num_bytes_left == 0);
  return slices;
}

static void write_done(void *user_data /* write_socket_state */, int success) {
  struct write_socket_state *state = (struct write_socket_state *)user_data;
  gpr_log(GPR_INFO, "Write done callback called");
  gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
  gpr_log(GPR_INFO, "Signalling write done");
  state->write_done = 1;
  grpc_pollset_kick(&g_pollset, NULL);
  gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
}

void drain_socket_blocking(int fd, size_t num_bytes, size_t read_size) {
  unsigned char *buf = malloc(read_size);
  ssize_t bytes_read;
  size_t bytes_left = num_bytes;
  int flags;
  int current = 0;
  int i;

  flags = fcntl(fd, F_GETFL, 0);
  GPR_ASSERT(fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) == 0);

  for (;;) {
    grpc_pollset_worker worker;
    gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
    grpc_pollset_work(&g_pollset, &worker, gpr_now(GPR_CLOCK_MONOTONIC),
                      GRPC_TIMEOUT_MILLIS_TO_DEADLINE(10));
    gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
    do {
      bytes_read =
          read(fd, buf, bytes_left > read_size ? read_size : bytes_left);
    } while (bytes_read < 0 && errno == EINTR);
    GPR_ASSERT(bytes_read >= 0);
    for (i = 0; i < bytes_read; ++i) {
      GPR_ASSERT(buf[i] == current);
      current = (current + 1) % 256;
    }
    bytes_left -= (size_t)bytes_read;
    if (bytes_left == 0) break;
  }
  flags = fcntl(fd, F_GETFL, 0);
  GPR_ASSERT(fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);

  gpr_free(buf);
}

static ssize_t drain_socket(int fd) {
  ssize_t read_bytes;
  ssize_t total_bytes = 0;
  unsigned char buf[256];
  int current = 0;
  int i;
  do {
    read_bytes = read(fd, buf, 256);
    if (read_bytes > 0) {
      total_bytes += read_bytes;
      for (i = 0; i < read_bytes; ++i) {
        GPR_ASSERT(buf[i] == current);
        current = (current + 1) % 256;
      }
    }
  } while (read_bytes >= 0 || errno == EINTR);
  GPR_ASSERT(errno == EAGAIN);
  return total_bytes;
}

/* Write to a socket using the grpc_tcp API, then drain it directly.
   Note that if the write does not complete immediately we need to drain the
   socket in parallel with the read. */
static void write_test(size_t num_bytes, size_t slice_size) {
  int sv[2];
  grpc_endpoint *ep;
  struct write_socket_state state;
  ssize_t read_bytes;
  size_t num_blocks;
  gpr_slice *slices;
  gpr_uint8 current_data = 0;
  gpr_slice_buffer outgoing;
  grpc_iomgr_closure write_done_closure;
  gpr_timespec deadline = GRPC_TIMEOUT_SECONDS_TO_DEADLINE(20);

  gpr_log(GPR_INFO, "Start write test with %d bytes, slice size %d", num_bytes,
          slice_size);

  create_sockets(sv);

  ep = grpc_tcp_create(grpc_fd_create(sv[1], "write_test"),
                       GRPC_TCP_DEFAULT_READ_SLICE_SIZE, "test");
  grpc_endpoint_add_to_pollset(ep, &g_pollset);

  state.ep = ep;
  state.write_done = 0;

  slices = allocate_blocks(num_bytes, slice_size, &num_blocks, &current_data);

  gpr_slice_buffer_init(&outgoing);
  gpr_slice_buffer_addn(&outgoing, slices, num_blocks);
  grpc_iomgr_closure_init(&write_done_closure, write_done, &state);

  switch (grpc_endpoint_write(ep, &outgoing, &write_done_closure)) {
    case GRPC_ENDPOINT_DONE:
      /* Write completed immediately */
      read_bytes = drain_socket(sv[0]);
      GPR_ASSERT((size_t)read_bytes == num_bytes);
      break;
    case GRPC_ENDPOINT_PENDING:
      drain_socket_blocking(sv[0], num_bytes, num_bytes);
      gpr_mu_lock(GRPC_POLLSET_MU(&g_pollset));
      for (;;) {
        grpc_pollset_worker worker;
        if (state.write_done) {
          break;
        }
        grpc_pollset_work(&g_pollset, &worker, gpr_now(GPR_CLOCK_MONOTONIC),
                          deadline);
      }
      gpr_mu_unlock(GRPC_POLLSET_MU(&g_pollset));
      break;
    case GRPC_ENDPOINT_ERROR:
      gpr_log(GPR_ERROR, "endpoint got error");
      abort();
  }

  gpr_slice_buffer_destroy(&outgoing);
  grpc_endpoint_destroy(ep);
  gpr_free(slices);
}

void run_tests(void) {
  size_t i = 0;

  read_test(100, 8192);
  read_test(10000, 8192);
  read_test(10000, 137);
  read_test(10000, 1);
  large_read_test(8192);
  large_read_test(1);

  write_test(100, 8192);
  write_test(100, 1);
  write_test(100000, 8192);
  write_test(100000, 1);
  write_test(100000, 137);

  for (i = 1; i < 1000; i = GPR_MAX(i + 1, i * 5 / 4)) {
    write_test(40320, i);
  }
}

static void clean_up(void) {}

static grpc_endpoint_test_fixture create_fixture_tcp_socketpair(
    size_t slice_size) {
  int sv[2];
  grpc_endpoint_test_fixture f;

  create_sockets(sv);
  f.client_ep = grpc_tcp_create(grpc_fd_create(sv[0], "fixture:client"),
                                slice_size, "test");
  f.server_ep = grpc_tcp_create(grpc_fd_create(sv[1], "fixture:server"),
                                slice_size, "test");
  grpc_endpoint_add_to_pollset(f.client_ep, &g_pollset);
  grpc_endpoint_add_to_pollset(f.server_ep, &g_pollset);

  return f;
}

static grpc_endpoint_test_config configs[] = {
    {"tcp/tcp_socketpair", create_fixture_tcp_socketpair, clean_up},
};

static void destroy_pollset(void *p) { grpc_pollset_destroy(p); }

int main(int argc, char **argv) {
  grpc_test_init(argc, argv);
  grpc_init();
  grpc_pollset_init(&g_pollset);
  run_tests();
  grpc_endpoint_tests(configs[0], &g_pollset);
  grpc_pollset_shutdown(&g_pollset, destroy_pollset, &g_pollset);
  grpc_shutdown();

  return 0;
}
