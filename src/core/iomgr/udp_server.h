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

#ifndef GRPC_INTERNAL_CORE_IOMGR_UDP_SERVER_H
#define GRPC_INTERNAL_CORE_IOMGR_UDP_SERVER_H

#include "src/core/iomgr/endpoint.h"

/* Forward decl of grpc_udp_server */
typedef struct grpc_udp_server grpc_udp_server;

/* New server callback: ep is the newly connected connection */
typedef void (*grpc_udp_server_cb)(void *arg, grpc_endpoint *ep);

/* Called when data is available to read from the socket. */
typedef void (*grpc_udp_server_read_cb)(int fd,
                                        grpc_udp_server_cb new_transport_cb,
                                        void *cb_arg);

/* Create a server, initially not bound to any ports */
grpc_udp_server *grpc_udp_server_create(void);

/* Start listening to bound ports */
void grpc_udp_server_start(grpc_udp_server *server, grpc_pollset **pollsets,
                           size_t pollset_count, grpc_udp_server_cb cb,
                           void *cb_arg);

int grpc_udp_server_get_fd(grpc_udp_server *s, unsigned index);

/* Add a port to the server, returning port number on success, or negative
   on failure.

   The :: and 0.0.0.0 wildcard addresses are treated identically, accepting
   both IPv4 and IPv6 connections, but :: is the preferred style.  This usually
   creates one socket, but possibly two on systems which support IPv6,
   but not dualstack sockets. */

/* TODO(ctiller): deprecate this, and make grpc_udp_server_add_ports to handle
                  all of the multiple socket port matching logic in one place */
int grpc_udp_server_add_port(grpc_udp_server *s, const void *addr,
                             size_t addr_len, grpc_udp_server_read_cb read_cb);

void grpc_udp_server_destroy(grpc_udp_server *server,
                             void (*shutdown_done)(void *shutdown_done_arg),
                             void *shutdown_done_arg);

/* Write the contents of buffer to the underlying UDP socket. */
/*
void grpc_udp_server_write(grpc_udp_server *s,
                           const char *buffer,
                           int buf_len,
                           const struct sockaddr* to);
                           */

#endif /* GRPC_INTERNAL_CORE_IOMGR_UDP_SERVER_H */
