#include <assert.h>
#include <libdill.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_BUF_SZ 1024u
#define TIMEOUT 10000u

coroutine void worker(int s) {
  s = http_attach(s);
  if (s < 0) {
    perror("Can't add HTTP layer");
    return;
  }

  int rc;
  unsigned long content_length;
  char command[64];
  char resource[256];

  http_recvrequest(s, command, sizeof(command), resource, sizeof(resource), TIMEOUT);

  printf("=====\n");
  printf("%s %s\n=====\n", command, resource);

  while (1) {
    char name[256];
    char value[256];
    int rc = http_recvfield(s, name, sizeof(name), value, sizeof(value), TIMEOUT);
    if (rc == -1 && errno == EPIPE) break;
    assert(rc == 0);

    printf("%s: %s\n", name, value);

    if (strncmp(name, "Content-Length", 14) == 0) {
      content_length = strtoul(value, NULL, 0);
    }
  }

  printf("=====\n");

  http_sendstatus(s, 200, "OK", TIMEOUT);

  s = http_detach(s, TIMEOUT);
  if (s < 0) {
    perror("Can't detach HTTP layer");
    return;
  }

  if (content_length && !strncmp(command, "POST", 4)) {
    size_t data_sz = content_length * sizeof(char);
    size_t n = data_sz / MESSAGE_BUF_SZ;
    size_t l = data_sz % MESSAGE_BUF_SZ;

    char *data = malloc((content_length + 1) * sizeof(char));

    for(size_t i = 0; i < n; ++i) {
    	rc = brecv(s, data + i * MESSAGE_BUF_SZ, MESSAGE_BUF_SZ, TIMEOUT);
    	assert(rc >= 0);
    }

    if(l) {
    	rc = brecv(s, data + MESSAGE_BUF_SZ * n, l, TIMEOUT);
    	assert(rc >= 0);
    }

    data[data_sz] = '\0';

    fprintf(stdout, "%s\n", data);
  }

  /* Close the underlying TCP connection. */
  rc = tcp_close(s, TIMEOUT);
  if (rc < 0) {
    perror("Can't close TCP socket");
    return;
  }
}

int main() {
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

  while(1) {
    int s = tcp_accept(ls, NULL, TIMEOUT);
    if (s < 0) {
      perror("Can't accept new connection on local socket");
      return 1;
    }

    int cr = go(worker(s));
    if (s < 0) {
      perror("Can't start a coroutine");
      return 1;
    }
  }

  return 0;
}
