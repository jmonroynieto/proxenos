/* Checks on the router's packet and header parsing.  The router source is
   included directly so these tests can reach its internal helpers; the
   PROXENOS_TESTING guard leaves main() out. */
#define PROXENOS_TESTING
#include "proxenos-router.c"

/* Encodes a question section: "ollama.proxenos" becomes 6ollama8proxenos0. */
static gsize build_query(guint8 *packet, guint16 id, const gchar *name, guint16 type, guint16 questions) {
  packet[0] = (guint8)(id >> 8);
  packet[1] = (guint8)id;
  packet[2] = 0x01;                 /* a query, recursion desired */
  packet[3] = 0x00;
  packet[4] = (guint8)(questions >> 8);
  packet[5] = (guint8)questions;
  memset(packet + 6, 0, 6);

  gsize offset = 12;
  gchar **labels = g_strsplit(name, ".", -1);
  for (gchar **label = labels; *label; label++) {
    gsize length = strlen(*label);
    packet[offset++] = (guint8)length;
    memcpy(packet + offset, *label, length);
    offset += length;
  }
  g_strfreev(labels);

  packet[offset++] = 0;
  packet[offset++] = (guint8)(type >> 8);
  packet[offset++] = (guint8)type;
  packet[offset++] = 0;
  packet[offset++] = 1;             /* class IN */
  return offset;
}

static guint16 answer_count(const DnsMessage *response) {
  return (guint16)(response->data[6] << 8 | response->data[7]);
}

static int rcode(const DnsMessage *response) {
  return response->data[3] & 0x0f;
}

static void check_a_query_is_answered_with_loopback(void) {
  guint8 query[DNS_MESSAGE_MAX];
  gsize length = build_query(query, 0xbeef, "ollama.proxenos", DNS_TYPE_A, 1);

  DnsMessage response;
  g_assert_true(dns_build_response(query, length, &response));
  g_assert_cmpint(response.data[0], ==, 0xbe);      /* the id is echoed back */
  g_assert_cmpint(response.data[1], ==, 0xef);
  g_assert_true(response.data[2] & 0x80);           /* QR: this is a response */
  g_assert_true(response.data[2] & 0x04);           /* AA: Proxenos owns the zone */
  g_assert_true(response.data[2] & 0x01);           /* RD is echoed from the query */
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_OK);
  g_assert_cmpint(answer_count(&response), ==, 1);

  const guint8 *address = response.data + response.length - 4;
  g_assert_cmpint(address[0], ==, 127);
  g_assert_cmpint(address[1], ==, 0);
  g_assert_cmpint(address[2], ==, 0);
  g_assert_cmpint(address[3], ==, 1);
}

/* A resolver asks for A and AAAA together.  Answering "no such name" to the
   AAAA half contradicts the A half; the name exists and has no AAAA record. */
static void check_aaaa_query_says_no_such_record(void) {
  guint8 query[DNS_MESSAGE_MAX];
  gsize length = build_query(query, 1, "ollama.proxenos", 28 /* AAAA */, 1);

  DnsMessage response;
  g_assert_true(dns_build_response(query, length, &response));
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_OK);
  g_assert_cmpint(answer_count(&response), ==, 0);
}

static void check_names_outside_the_zone_are_refused(void) {
  guint8 query[DNS_MESSAGE_MAX];
  gsize length = build_query(query, 2, "example.com", DNS_TYPE_A, 1);

  DnsMessage response;
  g_assert_true(dns_build_response(query, length, &response));
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_REFUSED);
  g_assert_cmpint(answer_count(&response), ==, 0);
}

static void check_malformed_packets(void) {
  guint8 query[DNS_MESSAGE_MAX];
  DnsMessage response;

  gsize length = build_query(query, 3, "ollama.proxenos", DNS_TYPE_A, 1);

  g_assert_false(dns_build_response(query, 8, &response));       /* shorter than a header */

  /* A question cut short still arrives with an intact header, so the sender
     gets a format error naming its own request rather than silence. */
  g_assert_true(dns_build_response(query, length - 2, &response));
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_FORMAT_ERROR);

  query[2] |= 0x80;                                              /* a response, not a query */
  g_assert_false(dns_build_response(query, length, &response));
  query[2] &= (guint8)~0x80;

  length = build_query(query, 4, "ollama.proxenos", DNS_TYPE_A, 0);
  g_assert_true(dns_build_response(query, length, &response));
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_FORMAT_ERROR);

  length = build_query(query, 5, "ollama.proxenos", DNS_TYPE_A, 1);
  query[2] = (guint8)(query[2] | (4 << 3));                      /* an unimplemented opcode */
  g_assert_true(dns_build_response(query, length, &response));
  g_assert_cmpint(rcode(&response), ==, DNS_RCODE_NOT_IMPLEMENTED);
}

static gboolean host_of(const gchar *request, gchar *host, gsize size) {
  return read_host_header(request, strlen(request), host, size);
}

static void check_host_header_parsing(void) {
  gchar host[256];

  g_assert_true(host_of("GET / HTTP/1.1\r\nHost: ollama.proxenos\r\n\r\n", host, sizeof host));
  g_assert_cmpstr(host, ==, "ollama.proxenos");

  /* Header names are case insensitive, and the value may carry a port. */
  g_assert_true(host_of("GET / HTTP/1.1\r\nhost:forgejo.proxenos:80\r\n\r\n", host, sizeof host));
  g_assert_cmpstr(host, ==, "forgejo.proxenos");

  g_assert_true(host_of("GET / HTTP/1.1\r\nAccept: */*\r\nHOST:\tollama.proxenos \r\n\r\n", host, sizeof host));
  g_assert_cmpstr(host, ==, "ollama.proxenos");

  g_assert_false(host_of("GET / HTTP/1.1\r\nAccept: */*\r\n\r\n", host, sizeof host));
  g_assert_false(host_of("GET / HTTP/1.1\r\nHost: \r\n\r\n", host, sizeof host));
  g_assert_false(host_of("GET / HTTP/1.1\r\n", host, sizeof host));
  /* A header whose name merely starts with "host" is a different header. */
  g_assert_false(host_of("GET / HTTP/1.1\r\nHostage: no\r\n\r\n", host, sizeof host));
}

static void check_zone_membership(void) {
  g_assert_true(is_proxenos_name("ollama.proxenos"));
  g_assert_true(is_proxenos_name("OLLAMA.PROXENOS"));
  g_assert_false(is_proxenos_name("proxenos"));
  g_assert_false(is_proxenos_name(".proxenos"));
  g_assert_false(is_proxenos_name("ollama.proxenos.example.com"));
}

/* The relay buffers are the router's only memory of a connection, so their
   bookkeeping is worth pinning down. */
static void check_buffer_bookkeeping(void) {
  Buffer buffer = {0};
  g_assert_cmpuint(buffer_pending(&buffer), ==, 0);
  g_assert_cmpuint(buffer_space(&buffer), ==, sizeof buffer.data);

  buffer_append(&buffer, "hello");
  g_assert_cmpuint(buffer_pending(&buffer), ==, 5);

  buffer.start = 2;
  buffer_compact(&buffer);
  g_assert_cmpuint(buffer.start, ==, 0);
  g_assert_cmpuint(buffer_pending(&buffer), ==, 3);
  g_assert_cmpint(memcmp(buffer.data, "llo", 3), ==, 0);

  buffer.start = buffer.end;
  buffer_compact(&buffer);
  g_assert_cmpuint(buffer_pending(&buffer), ==, 0);
  g_assert_cmpuint(buffer_space(&buffer), ==, sizeof buffer.data);
}

int main(void) {
  check_a_query_is_answered_with_loopback();
  check_aaaa_query_says_no_such_record();
  check_names_outside_the_zone_are_refused();
  check_malformed_packets();
  check_host_header_parsing();
  check_zone_membership();
  check_buffer_bookkeeping();

  g_print("proxenos router test: all checks passed\n");
  return 0;
}
