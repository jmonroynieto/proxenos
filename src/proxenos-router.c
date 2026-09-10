/* Proxenos Router: the two adapters a *.proxenos hostname needs.  It answers
   DNS queries for the zone with 127.0.0.1, and forwards an HTTP request to the
   port its Host name is registered on.  Both listeners are on loopback.

   Everything runs in one process on one poll loop.  A browser holds a
   keep-alive connection open for a minute or more, so the connections cannot
   be served one at a time: a single blocking relay would leave DNS unanswered
   for the whole of that minute. */
#define _GNU_SOURCE
#include "proxenos-common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PROXENOS_ZONE_SUFFIX ".proxenos"
#define MAX_CONNECTIONS 64
#define RELAY_BUFFER_SIZE 32768
#define IDLE_TIMEOUT_SECONDS 120
#define POLL_INTERVAL_MS 1000
#define DNS_MESSAGE_MAX 512
#define DNS_ANSWER_TTL_SECONDS 30

typedef struct {
  gchar *hostname;
  guint16 port;
} Route;

/* Pending bytes are data[start, end).  One buffer per direction, so a slow
   reader on one side stops Proxenos reading from the other rather than
   growing without limit. */
typedef struct {
  gsize start;
  gsize end;
  gchar data[RELAY_BUFFER_SIZE];
} Buffer;

typedef struct {
  gboolean active;
  int client;
  int upstream;           /* -1 until a route is chosen and connected */
  gboolean connecting;    /* the upstream connect() has not completed */
  gboolean header_parsed;
  gboolean draining;      /* an error response is queued; close once it is out */
  gboolean client_eof;
  gboolean upstream_eof;
  time_t last_activity;
  Buffer to_upstream;
  Buffer to_client;
} Connection;

typedef struct {
  GPtrArray *routes;
  int dns_fd;
  int http_fd;
  Connection connections[MAX_CONNECTIONS];
} Router;

static volatile sig_atomic_t keep_running = 1;

static void request_stop(int signal_number) {
  (void)signal_number;
  keep_running = 0;
}

static void free_route(gpointer data) {
  Route *route = data;
  g_free(route->hostname);
  g_free(route);
}

/* ---- configuration ---------------------------------------------------- */

static gboolean is_proxenos_name(const gchar *name) {
  gsize length = strlen(name);
  gsize suffix = strlen(PROXENOS_ZONE_SUFFIX);
  return length > suffix && g_ascii_strcasecmp(name + length - suffix, PROXENOS_ZONE_SUFFIX) == 0;
}

static GPtrArray *load_routes(const gchar *path, GError **error) {
  GKeyFile *config = g_key_file_new();
  if (!g_key_file_load_from_file(config, path, G_KEY_FILE_NONE, error)) {
    g_key_file_unref(config);
    return NULL;
  }

  GPtrArray *routes = g_ptr_array_new_with_free_func(free_route);
  gsize count = 0;
  gchar **names = g_key_file_get_groups(config, &count);

  for (gsize i = 0; i < count; i++) {
    gchar *hostname = g_key_file_get_string(config, names[i], "hostname", NULL);
    gint port = g_key_file_get_integer(config, names[i], "port", NULL);

    if (hostname && is_proxenos_name(hostname) && port > 0 && port <= 65535) {
      Route *route = g_new0(Route, 1);
      route->hostname = g_ascii_strdown(hostname, -1);
      route->port = (guint16)port;
      g_ptr_array_add(routes, route);
    }
    g_free(hostname);
  }

  g_strfreev(names);
  g_key_file_unref(config);
  return routes;
}

static const Route *find_route(const Router *router, const gchar *hostname) {
  for (guint i = 0; i < router->routes->len; i++) {
    const Route *candidate = g_ptr_array_index(router->routes, i);
    if (g_ascii_strcasecmp(candidate->hostname, hostname) == 0)
      return candidate;
  }
  return NULL;
}

/* ---- DNS -------------------------------------------------------------- */

enum {
  DNS_RCODE_OK = 0,
  DNS_RCODE_FORMAT_ERROR = 1,
  DNS_RCODE_NOT_IMPLEMENTED = 4,
  DNS_RCODE_REFUSED = 5,
};

enum { DNS_TYPE_A = 1 };

typedef struct {
  guint8 data[DNS_MESSAGE_MAX];
  gsize length;
} DnsMessage;

/* Reads a name in wire format into dotted form and returns the offset just
   past it, or 0 if the encoding is unusable.  Compression pointers are
   rejected: a question section has nothing to point back at. */
static gsize dns_read_name(const guint8 *message, gsize size, gsize offset,
                           gchar *name, gsize name_size) {
  gsize written = 0;

  while (offset < size && message[offset] != 0) {
    guint8 label = message[offset++];
    if (label > 63 || offset + label > size || written + label + 2 >= name_size)
      return 0;
    if (written)
      name[written++] = '.';
    memcpy(name + written, message + offset, label);
    written += label;
    offset += label;
  }

  if (offset >= size)
    return 0;
  name[written] = '\0';
  return offset + 1;
}

/* Builds the reply to one query.  Returns FALSE when there is nothing sensible
   to send back, which is the right answer to a malformed packet.

   A *.proxenos name always resolves, whether or not a service is registered
   for it; an unregistered name then meets the router's own 403 over HTTP,
   which says more to the reader than a name that fails to resolve. */
static gboolean dns_build_response(const guint8 *query, gsize query_length, DnsMessage *response) {
  if (query_length < 12 || query_length > DNS_MESSAGE_MAX)
    return FALSE;
  if (query[2] & 0x80)          /* already a response: not ours to answer */
    return FALSE;

  guint8 opcode = (query[2] >> 3) & 0x0f;
  guint16 question_count = (guint16)(query[4] << 8 | query[5]);

  gchar name[256] = "";
  gsize question_end = 0;
  guint16 type = 0;
  int rcode = DNS_RCODE_OK;
  gboolean answer = FALSE;

  if (opcode != 0) {
    rcode = DNS_RCODE_NOT_IMPLEMENTED;
  } else if (question_count != 1
             || !(question_end = dns_read_name(query, query_length, 12, name, sizeof name))
             || question_end + 4 > query_length) {
    rcode = DNS_RCODE_FORMAT_ERROR;
    question_end = 0;
  } else {
    type = (guint16)(query[question_end] << 8 | query[question_end + 1]);
    if (!is_proxenos_name(name))
      rcode = DNS_RCODE_REFUSED;         /* the router is not a general resolver */
    else
      answer = type == DNS_TYPE_A;       /* other types: the name exists, with no such record */
  }

  gsize length = question_end ? question_end + 4 : 12;
  memcpy(response->data, query, length);

  /* QR, the request's opcode and RD bit, and AA for the zone Proxenos owns.
     RA stays clear: the router recurses for nobody. */
  response->data[2] = (guint8)(0x80 | (opcode << 3) | (query[2] & 0x01));
  if (rcode == DNS_RCODE_OK)
    response->data[2] |= 0x04;
  response->data[3] = (guint8)rcode;
  response->data[4] = 0;
  response->data[5] = question_end ? 1 : 0;
  response->data[6] = 0;
  response->data[7] = answer ? 1 : 0;
  memset(response->data + 8, 0, 4);      /* no authority or additional records */

  if (answer) {
    const guint8 record[] = {
      0xc0, 0x0c,                        /* the question's name, by reference */
      0x00, DNS_TYPE_A,
      0x00, 0x01,                        /* class IN */
      0x00, 0x00, 0x00, DNS_ANSWER_TTL_SECONDS,
      0x00, 0x04,
      127, 0, 0, 1,
    };
    memcpy(response->data + length, record, sizeof record);
    length += sizeof record;
  }

  response->length = length;
  return TRUE;
}

static void answer_dns_query(Router *router) {
  guint8 query[DNS_MESSAGE_MAX];
  struct sockaddr_in peer;
  socklen_t peer_size = sizeof peer;

  ssize_t received = recvfrom(router->dns_fd, query, sizeof query, 0,
                              (struct sockaddr *)&peer, &peer_size);
  if (received <= 0)
    return;

  DnsMessage response;
  if (dns_build_response(query, (gsize)received, &response))
    sendto(router->dns_fd, response.data, response.length, 0,
           (struct sockaddr *)&peer, peer_size);
}

/* ---- HTTP ------------------------------------------------------------- */

static gsize buffer_pending(const Buffer *buffer) {
  return buffer->end - buffer->start;
}

static gsize buffer_space(const Buffer *buffer) {
  return sizeof buffer->data - buffer->end;
}

static void buffer_compact(Buffer *buffer) {
  if (buffer->start == 0)
    return;
  if (buffer->start == buffer->end) {
    buffer->start = buffer->end = 0;
    return;
  }
  memmove(buffer->data, buffer->data + buffer->start, buffer->end - buffer->start);
  buffer->end -= buffer->start;
  buffer->start = 0;
}

static void buffer_append(Buffer *buffer, const gchar *text) {
  gsize length = strlen(text);
  if (length > buffer_space(buffer))
    length = buffer_space(buffer);
  memcpy(buffer->data + buffer->end, text, length);
  buffer->end += length;
}

/* Header names are case insensitive, so a client that writes `host:` must be
   understood exactly as one that writes `Host:`.  The value may carry a port,
   which is not part of the name being routed. */
static gboolean read_host_header(const gchar *header, gsize length, gchar *host, gsize host_size) {
  const gchar *end = header + length;
  const gchar *line = memmem(header, length, "\r\n", 2);
  if (!line)
    return FALSE;
  line += 2;

  while (line < end) {
    const gchar *line_end = memmem(line, (gsize)(end - line), "\r\n", 2);
    if (!line_end || line_end == line)
      return FALSE;                                  /* end of the header block */

    if ((gsize)(line_end - line) > 5 && g_ascii_strncasecmp(line, "Host:", 5) == 0) {
      const gchar *value = line + 5;
      while (value < line_end && (*value == ' ' || *value == '\t'))
        value++;

      const gchar *value_end = value;
      while (value_end < line_end && *value_end != ':' && *value_end != ' ' && *value_end != '\t')
        value_end++;

      gsize size = (gsize)(value_end - value);
      if (size == 0 || size >= host_size)
        return FALSE;
      memcpy(host, value, size);
      host[size] = '\0';
      return TRUE;
    }
    line = line_end + 2;
  }
  return FALSE;
}

static int connect_to_service(guint16 port, gboolean *in_progress) {
  int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_in target = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
  };

  *in_progress = FALSE;
  if (connect(fd, (struct sockaddr *)&target, sizeof target) == 0)
    return fd;
  if (errno == EINPROGRESS) {
    *in_progress = TRUE;
    return fd;
  }

  close(fd);
  return -1;
}

static void connection_close(Connection *connection) {
  if (connection->client >= 0)
    close(connection->client);
  if (connection->upstream >= 0)
    close(connection->upstream);
  memset(connection, 0, sizeof *connection);
  connection->client = -1;
  connection->upstream = -1;
}

/* Answers the client itself instead of forwarding.  A hostname with no route
   is refused; a route whose service is not listening is unavailable. */
static void connection_refuse(Connection *connection, const gchar *status) {
  gchar *response = g_strdup_printf("HTTP/1.1 %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status);
  buffer_append(&connection->to_client, response);
  g_free(response);

  if (connection->upstream >= 0) {
    close(connection->upstream);
    connection->upstream = -1;
  }
  /* The request is not going anywhere now, so nothing is left to forward. */
  connection->to_upstream.start = connection->to_upstream.end = 0;
  connection->connecting = FALSE;
  connection->draining = TRUE;
  connection->client_eof = TRUE;
}

/* Runs once the request header is complete: chooses the route and starts the
   connection to the service.  The header itself stays in the buffer and is
   forwarded as the first thing the service receives. */
static void connection_route(Router *router, Connection *connection) {
  const gchar *header = connection->to_upstream.data + connection->to_upstream.start;
  gsize length = buffer_pending(&connection->to_upstream);

  gchar host[256];
  if (!read_host_header(header, length, host, sizeof host)) {
    proxenos_debug_log("request without a usable Host header");
    connection_refuse(connection, "400 Bad Request");
    return;
  }

  const Route *route = find_route(router, host);
  if (!route) {
    proxenos_debug_log("no route configured for %s", host);
    connection_refuse(connection, "403 Forbidden");
    return;
  }

  gboolean in_progress = FALSE;
  connection->upstream = connect_to_service(route->port, &in_progress);
  if (connection->upstream < 0) {
    proxenos_debug_log("%s is routed to port %u but is not listening", host, route->port);
    connection_refuse(connection, "503 Service Unavailable");
    return;
  }

  connection->connecting = in_progress;
  connection->header_parsed = TRUE;
}

/* The header must arrive before anything can be routed.  A client that fills
   the buffer without ever ending its header is not going to. */
static void connection_read_header(Router *router, Connection *connection) {
  const gchar *data = connection->to_upstream.data + connection->to_upstream.start;
  gsize length = buffer_pending(&connection->to_upstream);

  if (memmem(data, length, "\r\n\r\n", 4))
    connection_route(router, connection);
  else if (buffer_space(&connection->to_upstream) == 0)
    connection_refuse(connection, "431 Request Header Fields Too Large");
  else if (connection->client_eof)
    connection_refuse(connection, "400 Bad Request");
}

/* Moves bytes from a socket into a buffer, or out of a buffer to a socket.
   Both report FALSE only for a real failure; an empty read is end of stream
   and a blocked socket is simply nothing to do this time round. */
static gboolean receive_into(int fd, Buffer *buffer, gboolean *eof) {
  if (buffer_space(buffer) == 0)
    return TRUE;                 /* a zero-length recv would read as end of stream */

  ssize_t received = recv(fd, buffer->data + buffer->end, buffer_space(buffer), 0);
  if (received > 0) {
    buffer->end += (gsize)received;
    return TRUE;
  }
  if (received == 0) {
    *eof = TRUE;
    return TRUE;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static gboolean send_from(int fd, Buffer *buffer) {
  ssize_t sent = send(fd, buffer->data + buffer->start, buffer_pending(buffer), MSG_NOSIGNAL);
  if (sent > 0) {
    buffer->start += (gsize)sent;
    return TRUE;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
}

static Connection *free_connection_slot(Router *router) {
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (!router->connections[i].active)
      return &router->connections[i];
  return NULL;
}

static void accept_client(Router *router) {
  int client = accept4(router->http_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  if (client < 0)
    return;

  Connection *connection = free_connection_slot(router);
  if (!connection) {
    close(client);
    return;
  }

  memset(connection, 0, sizeof *connection);
  connection->active = TRUE;
  connection->client = client;
  connection->upstream = -1;
  connection->last_activity = time(NULL);
}

/* One pass over a connection: what it can read, what it can write, and whether
   either side has finished.  Called with the poll results for its sockets. */
static void service_connection(Router *router, Connection *connection,
                               short client_events, short upstream_events) {
  gboolean progressed = FALSE;

  if (connection->connecting && (upstream_events & (POLLOUT | POLLERR | POLLHUP))) {
    int failure = 0;
    socklen_t size = sizeof failure;
    getsockopt(connection->upstream, SOL_SOCKET, SO_ERROR, &failure, &size);
    if (failure != 0) {
      proxenos_debug_log("upstream connection failed: %s", g_strerror(failure));
      connection_refuse(connection, "503 Service Unavailable");
    } else {
      connection->connecting = FALSE;
    }
    progressed = TRUE;
  }

  if (client_events & POLLIN) {
    if (!receive_into(connection->client, &connection->to_upstream, &connection->client_eof)) {
      connection_close(connection);
      return;
    }
    /* Only the first request on a connection is parsed.  After that the
       connection is a plain relay to the service the first Host named, which
       is what a browser does with keep-alive; a client that reused one
       connection for two different hostnames would reach only the first. */
    if (!connection->header_parsed && !connection->draining)
      connection_read_header(router, connection);
    progressed = TRUE;
  }

  if (connection->upstream >= 0 && (upstream_events & POLLIN) && !connection->connecting) {
    if (!receive_into(connection->upstream, &connection->to_client, &connection->upstream_eof)) {
      connection_close(connection);
      return;
    }
    progressed = TRUE;
  }

  if (connection->upstream >= 0 && (upstream_events & POLLOUT) && !connection->connecting
      && buffer_pending(&connection->to_upstream)) {
    if (!send_from(connection->upstream, &connection->to_upstream)) {
      connection_close(connection);
      return;
    }
    progressed = TRUE;
  }

  if ((client_events & POLLOUT) && buffer_pending(&connection->to_client)) {
    if (!send_from(connection->client, &connection->to_client)) {
      connection_close(connection);
      return;
    }
    progressed = TRUE;
  }

  /* poll reports a hang-up whether or not it was asked to.  A buffer with no
     room means POLLIN is not being requested, so without this the same POLLHUP
     would be re-reported on every pass and the loop would spin. */
  if ((client_events | upstream_events) & (POLLERR | POLLNVAL)) {
    connection_close(connection);
    return;
  }
  if (client_events & POLLHUP)
    connection->client_eof = TRUE;
  if (upstream_events & POLLHUP)
    connection->upstream_eof = TRUE;

  /* Pass each end-of-stream on once its direction has drained, so a client
     that half-closes still receives the rest of the service's response. */
  if (connection->client_eof && !connection->draining && connection->upstream >= 0
      && !connection->connecting && buffer_pending(&connection->to_upstream) == 0)
    shutdown(connection->upstream, SHUT_WR);

  gboolean finished =
      (connection->draining && buffer_pending(&connection->to_client) == 0)
      || (connection->upstream_eof && buffer_pending(&connection->to_client) == 0)
      || (connection->client_eof && connection->upstream < 0 && !connection->draining);

  if (finished) {
    connection_close(connection);
    return;
  }

  if (progressed)
    connection->last_activity = time(NULL);
}

static short client_interest(const Connection *connection) {
  short events = 0;
  if (!connection->client_eof && buffer_space(&connection->to_upstream) > 0)
    events |= POLLIN;
  if (buffer_pending(&connection->to_client) > 0)
    events |= POLLOUT;
  return events;
}

static short upstream_interest(const Connection *connection) {
  if (connection->upstream < 0)
    return 0;
  if (connection->connecting)
    return POLLOUT;

  short events = 0;
  if (!connection->upstream_eof && buffer_space(&connection->to_client) > 0)
    events |= POLLIN;
  if (buffer_pending(&connection->to_upstream) > 0)
    events |= POLLOUT;
  return events;
}

static void close_idle_connections(Router *router, time_t now) {
  for (int i = 0; i < MAX_CONNECTIONS; i++) {
    Connection *connection = &router->connections[i];
    if (connection->active && now - connection->last_activity > IDLE_TIMEOUT_SECONDS) {
      proxenos_debug_log("closing a connection idle for %d seconds", IDLE_TIMEOUT_SECONDS);
      connection_close(connection);
    }
  }
}

static void run_router(Router *router) {
  /* Two listeners plus both sockets of every connection. */
  struct pollfd watched[2 + 2 * MAX_CONNECTIONS];
  int connection_index[2 + 2 * MAX_CONNECTIONS];
  gboolean is_upstream[2 + 2 * MAX_CONNECTIONS];

  while (keep_running) {
    int watch_count = 0;
    int active = 0;

    watched[watch_count] = (struct pollfd){ .fd = router->dns_fd, .events = POLLIN };
    connection_index[watch_count++] = -1;
    int http_slot = watch_count;
    watched[watch_count] = (struct pollfd){ .fd = router->http_fd, .events = 0 };
    connection_index[watch_count++] = -1;

    for (int i = 0; i < MAX_CONNECTIONS; i++) {
      Connection *connection = &router->connections[i];
      if (!connection->active)
        continue;
      active++;
      buffer_compact(&connection->to_upstream);
      buffer_compact(&connection->to_client);

      short events = client_interest(connection);
      if (events) {
        watched[watch_count] = (struct pollfd){ .fd = connection->client, .events = events };
        is_upstream[watch_count] = FALSE;
        connection_index[watch_count++] = i;
      }
      events = upstream_interest(connection);
      if (events) {
        watched[watch_count] = (struct pollfd){ .fd = connection->upstream, .events = events };
        is_upstream[watch_count] = TRUE;
        connection_index[watch_count++] = i;
      }
    }

    /* With every slot in use, new connections wait in the listen backlog
       rather than being accepted and starved. */
    if (active < MAX_CONNECTIONS)
      watched[http_slot].events = POLLIN;

    /* With no connections open there is nothing to time out, so the router
       waits for a packet rather than waking once a second forever. */
    int timeout = active > 0 ? POLL_INTERVAL_MS : -1;
    if (poll(watched, (nfds_t)watch_count, timeout) < 0) {
      if (errno == EINTR)
        continue;
      proxenos_debug_log("poll failed: %s", g_strerror(errno));
      break;
    }

    if (watched[0].revents & POLLIN)
      answer_dns_query(router);
    if (watched[http_slot].revents & POLLIN)
      accept_client(router);

    /* Collect each connection's events before servicing it, so a connection
       watched on both sockets is handled once rather than twice. */
    short client_events[MAX_CONNECTIONS] = {0};
    short upstream_events[MAX_CONNECTIONS] = {0};
    gboolean touched[MAX_CONNECTIONS] = {0};

    for (int i = 2; i < watch_count; i++) {
      int index = connection_index[i];
      if (index < 0 || !watched[i].revents)
        continue;
      touched[index] = TRUE;
      if (is_upstream[i])
        upstream_events[index] |= watched[i].revents;
      else
        client_events[index] |= watched[i].revents;
    }

    for (int i = 0; i < MAX_CONNECTIONS; i++)
      if (touched[i] && router->connections[i].active)
        service_connection(router, &router->connections[i], client_events[i], upstream_events[i]);

    close_idle_connections(router, time(NULL));
  }
}

/* ---- listeners and lifetime ------------------------------------------- */

static int loopback_socket(int type, guint16 port) {
  int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  int reuse = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);

  struct sockaddr_in address = {
    .sin_family = AF_INET,
    .sin_port = htons(port),
    .sin_addr = { .s_addr = htonl(INADDR_LOOPBACK) },
  };

  if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0
      || (type == SOCK_STREAM && listen(fd, 64) != 0)) {
    int failure = errno;
    close(fd);
    errno = failure;
    return -1;
  }
  return fd;
}

/* The default pid file needs root, which is also what binding ports 53 and 80
   needs.  A development router on high ports keeps its pid somewhere it can
   actually write. */
static int stop_existing_router(const gchar *pid_file) {
  gboolean standard = g_strcmp0(pid_file, PROXENOS_ROUTER_PID_FILE) == 0;
  pid_t pid;
  if (!(standard ? proxenos_router_pid(&pid) : proxenos_pid_from_file(pid_file, &pid))) {
    proxenos_debug_log("stop requested; no running router found");
    g_unlink(pid_file);
    g_print("Proxenos Router is not running.\n");
    return 0;
  }

  if (kill(pid, SIGTERM) != 0) {
    proxenos_debug_log("stop of pid %d failed: %s", (int)pid, g_strerror(errno));
    g_printerr("Cannot stop router: %s\n", g_strerror(errno));
    return 1;
  }

  proxenos_debug_log("stop requested for pid %d", (int)pid);
  g_print("Stopping Proxenos Router (pid %d).\n", (int)pid);
  return 0;
}

static gboolean run_command(const gchar *const *argv) {
  gint status = 0;
  GError *error = NULL;
  gboolean ran = g_spawn_sync(NULL, (gchar **)argv, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, NULL, &status, &error);

  if (!ran || status != 0) {
    g_printerr("%s failed: %s\n", argv[0], error ? error->message : "non-zero status");
    proxenos_debug_log("%s failed: %s", argv[0], error ? error->message : "non-zero status");
    g_clear_error(&error);
    return FALSE;
  }
  return TRUE;
}

/* Points the system resolver at the router, keeping a copy of whatever
   /etc/resolv.conf held first.  Runs as root, under pkexec, and touches
   nothing outside the files named here. */
static gboolean setup_manjaro_resolver(void) {
  const gchar *resolved_conf = "/etc/systemd/resolved.conf.d/proxenos.conf";
  const gchar *networkmanager_conf = "/etc/NetworkManager/conf.d/proxenos.conf";
  const gchar *resolv_conf = "/etc/resolv.conf";
  const gchar *backup = "/etc/resolv.conf.proxenos-backup";
  const gchar *stub = "/run/systemd/resolve/stub-resolv.conf";
  GError *error = NULL;

  proxenos_debug_log("configuring NetworkManager and systemd-resolved");

  if (g_mkdir_with_parents("/etc/systemd/resolved.conf.d", 0755) != 0
      || !g_file_set_contents(resolved_conf, "[Resolve]\nDNS=127.0.0.1\nDomains=~proxenos\n", -1, &error)) {
    g_printerr("Cannot configure systemd-resolved: %s\n", error ? error->message : g_strerror(errno));
    g_clear_error(&error);
    return FALSE;
  }

  if (g_mkdir_with_parents("/etc/NetworkManager/conf.d", 0755) != 0
      || !g_file_set_contents(networkmanager_conf, "[main]\ndns=systemd-resolved\n", -1, &error)) {
    g_printerr("Cannot configure NetworkManager: %s\n", error ? error->message : g_strerror(errno));
    g_clear_error(&error);
    return FALSE;
  }

  if (!g_file_test(backup, G_FILE_TEST_EXISTS)) {
    gchar *previous = NULL;
    if (g_file_get_contents(resolv_conf, &previous, NULL, NULL)) {
      g_file_set_contents(backup, previous, -1, NULL);
      g_free(previous);
    }
  }

  if (g_unlink(resolv_conf) != 0 && errno != ENOENT) {
    g_printerr("Cannot replace /etc/resolv.conf: %s\n", g_strerror(errno));
    return FALSE;
  }
  if (symlink(stub, resolv_conf) != 0) {
    g_printerr("Cannot link /etc/resolv.conf: %s\n", g_strerror(errno));
    return FALSE;
  }

  const gchar *const enable[] = { "systemctl", "enable", "--now", "systemd-resolved.service", NULL };
  const gchar *const restart[] = { "systemctl", "restart", "systemd-resolved.service", NULL };
  const gchar *const reload[] = { "nmcli", "general", "reload", NULL };
  if (!run_command(enable) || !run_command(restart) || !run_command(reload))
    return FALSE;

  char resolved[PATH_MAX];
  if (!realpath(resolv_conf, resolved) || g_strcmp0(resolved, stub) != 0) {
    /* The UI recognises this wording and explains it in the user's terms. */
    g_printerr("/etc/resolv.conf is not using its stub resolver, so NetworkManager "
               "did not accept the systemd-resolved DNS configuration.\n");
    return FALSE;
  }
  return TRUE;
}

#ifndef PROXENOS_TESTING
int main(int argc, char **argv) {
  gchar *config_path = NULL;
  gchar *debug_path = NULL;
  gchar *pid_file = NULL;
  gint dns_port = 53;
  gint http_port = 80;
  gboolean stop = FALSE;
  gboolean setup_manjaro = FALSE;

  GOptionEntry entries[] = {
    { "config", 'c', 0, G_OPTION_ARG_FILENAME, &config_path, "Service configuration file", "PATH" },
    { "dns-port", 0, 0, G_OPTION_ARG_INT, &dns_port, "DNS listen port", "PORT" },
    { "http-port", 0, 0, G_OPTION_ARG_INT, &http_port, "HTTP listen port", "PORT" },
    { "debug-log", 0, 0, G_OPTION_ARG_FILENAME, &debug_path, "Diagnostic log file", "PATH" },
    { "pid-file", 0, 0, G_OPTION_ARG_FILENAME, &pid_file, "Where to record the router's pid", "PATH" },
    { "stop", 0, 0, G_OPTION_ARG_NONE, &stop, "Stop the running router", NULL },
    { "setup-manjaro", 0, 0, G_OPTION_ARG_NONE, &setup_manjaro, "Configure systemd-resolved for .proxenos", NULL },
    { NULL },
  };

  GOptionContext *context = g_option_context_new("- route *.proxenos names to local services");
  g_option_context_add_main_entries(context, entries, NULL);
  GError *error = NULL;
  gboolean parsed = g_option_context_parse(context, &argc, &argv, &error);
  g_option_context_free(context);

  if (!parsed) {
    g_printerr("%s\n", error->message);
    g_error_free(error);
    return 2;
  }

  proxenos_debug_log_configure("router", debug_path);
  if (!pid_file)
    pid_file = g_strdup(PROXENOS_ROUTER_PID_FILE);

  int result = 1;
  if (stop) {
    result = stop_existing_router(pid_file);
    goto finished;
  }

  if (!config_path)
    config_path = proxenos_config_path();

  proxenos_debug_log("start requested; config=%s dns_port=%d http_port=%d setup_manjaro=%d",
                     config_path, dns_port, http_port, setup_manjaro);

  if (setup_manjaro && !setup_manjaro_resolver())
    goto finished;

  Router router = { .dns_fd = -1, .http_fd = -1 };
  router.routes = load_routes(config_path, &error);
  if (!router.routes) {
    g_printerr("Cannot load %s: %s\n", config_path, error->message);
    g_error_free(error);
    goto finished;
  }

  router.dns_fd = loopback_socket(SOCK_DGRAM, (guint16)dns_port);
  router.http_fd = loopback_socket(SOCK_STREAM, (guint16)http_port);
  if (router.dns_fd < 0 || router.http_fd < 0) {
    proxenos_debug_log("cannot bind DNS %d or HTTP %d: %s", dns_port, http_port, g_strerror(errno));
    g_printerr("Cannot bind 127.0.0.1:%d (DNS) and :%d (HTTP): %s\n",
               dns_port, http_port, g_strerror(errno));
    goto cleanup;
  }

  signal(SIGINT, request_stop);
  signal(SIGTERM, request_stop);
  signal(SIGPIPE, SIG_IGN);

  if (!proxenos_write_pid_file(pid_file, getpid(), &error)) {
    g_printerr("Cannot write %s: %s\n", pid_file, error->message);
    g_error_free(error);
    goto cleanup;
  }

  proxenos_debug_log("router started as pid %d with %u route(s)", (int)getpid(), router.routes->len);
  g_print("Routing %u service(s): DNS 127.0.0.1:%d, HTTP 127.0.0.1:%d\n",
          router.routes->len, dns_port, http_port);

  run_router(&router);

  proxenos_debug_log("router stopping");
  g_unlink(pid_file);
  result = 0;

cleanup:
  for (int i = 0; i < MAX_CONNECTIONS; i++)
    if (router.connections[i].active)
      connection_close(&router.connections[i]);
  if (router.dns_fd >= 0)
    close(router.dns_fd);
  if (router.http_fd >= 0)
    close(router.http_fd);
  g_clear_pointer(&router.routes, g_ptr_array_unref);

finished:
  g_free(config_path);
  g_free(debug_path);
  g_free(pid_file);
  return result;
}
#endif
