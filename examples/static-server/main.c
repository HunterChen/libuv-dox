#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include "dgb.h"
#include "../libuv/include/uv.h"
#include "../http-parser/http_parser.h"

#define CHECK(r, msg) if (r) {                                                 \
  log_err("%s: [%s(%d): %s]\n", msg, uv_err_name((r)), r, uv_strerror((r)));   \
  exit(1);                                                                     \
}

#define UVERR(r, msg) log_err("%s: [%s(%d): %s]\n", msg, uv_err_name((r)), r, uv_strerror((r)));

#define PORT    3000
#define BACKLOG 128

#define DEFAULT_RESPONSE \
  "HTTP/1.1 200 OK\r\n" \
  "Content-Type: text/plain\r\n" \
  "Content-Length: 12\r\n" \
  "\r\n" \
  "hello world\n"

typedef struct {
  uv_tcp_t handle;
  http_parser parser;
  uv_write_t write_req;
  int request_id;
} client_t;

static uv_loop_t *loop;
static uv_tcp_t server;
static int request_id;
static http_parser_settings parser_settings;
static uv_buf_t default_response;

static void on_connect(uv_stream_t *server, int status);
static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf);
static void on_client_read(uv_stream_t *client, ssize_t nread, const uv_buf_t *buf);

static int on_message_begin(http_parser* parser);
static int on_url(http_parser* parser, const char* hdr, size_t length);
static int on_status(http_parser* parser, const char* hdr, size_t length);
static int on_header_field(http_parser* parser, const char* hdr, size_t length);
static int on_header_value(http_parser* parser, const char* hdr, size_t length);
static int on_headers_complete(http_parser* parser);

static void on_res_end(uv_handle_t *handle);

static void alloc_cb(uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  buf->base = malloc(size);
  buf->len = size;
}

static void on_connect(uv_stream_t *server, int status) {
  int r;

  CHECK(status, "connecting");
  debug("connecting client");

  client_t *client = malloc(sizeof(client_t));
  client->request_id = request_id++;

  // we could use client pointer as shorthand interchangeably with &client->handle
  // for clarity, we assigned exactly what
  assert(client == (void*)&client->handle);

  uv_tcp_init(loop, &client->handle);
  http_parser_init(&client->parser, HTTP_REQUEST);

  // parser parses all data piped into the tcp socket (client->handle) -- that's all it needs from client struct
  // however we'll cast this back to the full client struct to get a hold of the client_req
  // which is kept around as long as the client itself is
  // see: on_headers_complete
  client->parser.data = client;

  // see: on_client_read
  client->handle.data = client;

  r = uv_accept(server, (uv_stream_t*) &client->handle);
  if (r) {
    log_err("error accepting connection %d", r);
    uv_close((uv_handle_t*) client, NULL);
  } else {
    // read the request into the tcp socket to cause it to get parsed
    // once the headers are in we'll get called back the first time (see on_headers_complete)
    // for now we assume no body since this is just a static webserver
    uv_read_start((uv_stream_t*) client, alloc_cb, on_client_read);
  }
}

static void on_client_read(uv_stream_t *tcp, ssize_t nread, const uv_buf_t *buf) {
  size_t parsed;

  if (nread == UV_EOF) {
    uv_close((uv_handle_t*) tcp, NULL);

    debug("closed client tcp connection due to unexpected EOF");
  } else if (nread > 0) {
    client_t *client = (client_t*) tcp->data;

    log_info("[ %3d ] request (len %ld)\n%s", client->request_id, nread, buf->base);

    parsed = http_parser_execute(&client->parser, &parser_settings, buf->base, nread);
    if (parsed < nread) {
      log_err("parsing http request");
      uv_close((uv_handle_t*) &client->handle, on_res_end);
    }
  } else {
    UVERR((int) nread, "reading client request");
  }
  if (buf->base) free(buf->base);
}

static void on_res_write(uv_write_t* req, int status) {
  CHECK(status, "on res write");
  uv_close((uv_handle_t*) req->handle, on_res_end);
}

static void on_res_end(uv_handle_t *handle) {
  client_t* client = (client_t*) handle->data;
  log_info("[ %3d ] connection closed", client->request_id);
}

static char* strslice(const char* s, size_t len) {
  char *slice = (char*) malloc(sizeof(char) * (len + 1));
  strncpy(slice, s, len);
  slice[len] = '\0';
  return slice;
}

static int on_message_begin(http_parser* parser) {
  client_t *client = (client_t*) parser->data;
  dbg("[ %3d ] message begin", client->request_id);
  return 0;
}

static int on_url(http_parser* parser, const char* hdr, size_t length) {
  client_t *client = (client_t*) parser->data;
  char *url = strslice(hdr, length);
  dbg("[ %3d ] h_url: %s", client->request_id, url);
  free(url);
  return 0;
}

static int on_status(http_parser* parser, const char* hdr, size_t length) {
  client_t *client = (client_t*) parser->data;
  char *status = strslice(hdr, length);
  dbg("[ %3d ] h_status: %s", client->request_id, status);
  free(status);
  return 0;
}

static int on_header_field(http_parser* parser, const char* hdr, size_t length) {
  client_t *client = (client_t*) parser->data;
  char *field = strslice(hdr, length);
  dbg("[ %3d ] h_field: %s", client->request_id, field);
  free(field);
  return 0;
}

static int on_header_value(http_parser* parser, const char* hdr, size_t length) {
  client_t *client = (client_t*) parser->data;
  char *value = strslice(hdr, length);
  dbg("[ %3d ] h_value: %s", client->request_id, value);
  free(value);
  return 0;
}

static int on_headers_complete(http_parser* parser) {
  // parser->data was pointed to the client struct in on_connect
  client_t *client = (client_t*) parser->data;
  dbg("[ %3d ] headers complete", client->request_id);

  // signal that there won't be a body by returning 1
  // we don't support anything but HEAD and GET since we are just a static webserver
  return 1;
}

static int on_message_complete(http_parser* parser) {
  client_t *client = (client_t*) parser->data;
  dbg("[ %3d ] message complete", client->request_id);

  // respond
  uv_write(&client->write_req, (uv_stream_t*) &client->handle, &default_response, 1, on_res_write);

  return 0;
}


int main() {
  int r;
  default_response.base = DEFAULT_RESPONSE;
  default_response.len = strlen(default_response.base);

  // parser settings shared for each request
  parser_settings.on_message_begin    =  on_message_begin;
  parser_settings.on_status           =  on_status;
  parser_settings.on_url              =  on_url;
  parser_settings.on_header_field     =  on_header_field;
  parser_settings.on_header_value     =  on_header_value;
  parser_settings.on_headers_complete =  on_headers_complete;
  parser_settings.on_message_complete =  on_message_complete;

  request_id = 0;
  loop = uv_default_loop();

  struct sockaddr_in bind_addr;
  r = uv_ip4_addr("0.0.0.0", PORT, &bind_addr);
  CHECK(r, "get bind addr");

  r = uv_tcp_init(loop, &server);
  CHECK(r, "init server");

  r = uv_tcp_bind(&server, (const struct sockaddr*) &bind_addr);
  CHECK(r, "bind");

  r = uv_listen((uv_stream_t*) &server, BACKLOG, on_connect);
  log_info("listening on http://localhost:%d", PORT);

  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}
