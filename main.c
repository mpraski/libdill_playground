#include <assert.h>
#include <libdill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

coroutine void worker(const char *text) {
  while (1) {
    printf("%s\n", text);
    msleep(now() + random() % 500);
  }
}

int main() {
  // go(worker("Hello!"));
  // go(worker("World!"));
  // msleep(now() + 5000);

  int port = 1234;
  struct ipaddr addr;

  int rc = ipaddr_local(&addr, NULL, port, 0);
  if (rc < 0) {
    perror("Can't open socket on local address");
    return 1;
  }

  printf("Opened socket on local port %d\n", port);

  int ls = tcp_listen(&addr, 10);
  if (ls < 0) {
    perror("Can't listen on local socket");
    return 1;
  }

  printf("Listening for TCP connections\n");

  int s = tcp_accept(ls, NULL, -1);
  if (s < 0) {
    perror("Can't accept new connection on local socket");
    return 1;
  }

  printf("Accepted TCP connections\n");

  /*s = tls_attach_client(s, -1);
      if(s < 0) {
              perror("Can't add TLS layer");
              return 1;
      }*/

  s = http_attach(s);
  if (s < 0) {
    perror("Can't add HTTP layer");
    return 1;
  }

  printf("Attached HTTP layer\n");

  int content_length;
  char command[64];
  char resource[256];

  http_recvrequest(s, command, sizeof(command), resource, sizeof(resource), -1);

  printf("===\n");
  printf("%s %s\n===\n", command, resource);

  while (1) {
    char name[256];
    char value[256];
    int rc = http_recvfield(s, name, sizeof(name), value, sizeof(value), -1);
    if (rc == -1 && errno == EPIPE) break;
    assert(rc == 0);

    printf("%s: %s\n", name, value);

    if (strncmp(name, "Content-Length", 14) == 0) {
      content_length = atoi(value);
    }
  }

  printf("===\n");

  http_sendstatus(s, 200, "OK", -1);

  printf("Sent status OK\n");

  /* Perform HTTP terminal handshake. */
  s = http_detach(s, -1);
  assert(s >= 0);

  printf("Detached HTTP layer\n");

  if (strncmp(command, "POST", 4) == 0) {
    size_t data_sz = ((size_t)content_length * sizeof(char)) + 1;
    char *data = (char *)malloc(data_sz);

    rc = brecv(s, data, data_sz - 1, -1);
    assert(rc >= 0);

    data[data_sz - 1] = '\0';

    fprintf(stdout, "%s\n", data);
  }

  /* Close the underlying TCP co22222222222nnection. */
  rc = tcp_close(s, -1);
  assert(rc == 0);

  printf("Closed underlying connection\n");

  return 0;
}
