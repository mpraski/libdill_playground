#include <assert.h>
#include <libdill.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MESSAGE_BUF_SZ 1024u
#define SOCKET_TIMEOUT 1000
#define TIMEOUT -1

volatile sig_atomic_t done;

void sig_handler(int sig) {
  if (sig == SIGINT) {
    done = 1;

    fprintf(stdout, "\nClosing connections...\n");
    fflush(stdout);
  }
}

coroutine void worker(int s) {
  s = http_attach(s);
  if (s < 0)
    goto cleanup;

  int rc;
  unsigned long content_length;
  char command[64];
  char resource[256];

  http_recvrequest(s, command, sizeof(command), resource, sizeof(resource),
                   TIMEOUT);

  printf("=====\n");
  printf("%s %s\n=====\n", command, resource);

  while (1) {
    char name[256];
    char value[256];

    int rc =
        http_recvfield(s, name, sizeof(name), value, sizeof(value), TIMEOUT);
    if (rc == -1) {
      if (errno == EPIPE)
        break;
      else if (errno == ECANCELED)
        goto cleanup;
    }

    printf("%s: %s\n", name, value);

    if (strncmp(name, "Content-Length", 14) == 0) {
      content_length = strtoul(value, NULL, 0);
    }
  }

  printf("=====\n");

  http_sendstatus(s, 200, "OK", TIMEOUT);

  s = http_detach(s, TIMEOUT);
  if (s < 0)
    goto cleanup;

  if (content_length && !strncmp(command, "POST", 4)) {
    size_t data_sz = content_length * sizeof(char);
    size_t n = data_sz / MESSAGE_BUF_SZ;
    size_t l = data_sz % MESSAGE_BUF_SZ;

    char *data = malloc((content_length + 1) * sizeof(char));

    for (size_t i = 0; i < n; ++i) {
      rc = brecv(s, data + i * MESSAGE_BUF_SZ, MESSAGE_BUF_SZ, TIMEOUT);
      if (rc == -1) {
        if (errno == EPIPE)
          break;
        else if (errno == ECANCELED)
          goto cleanup;
      }
    }

    if (l) {
      rc = brecv(s, data + MESSAGE_BUF_SZ * n, l, TIMEOUT);
      if (rc == -1 && errno == ECANCELED)
        goto cleanup;
    }

    data[data_sz] = '\0';

    fprintf(stdout, "%s\n", data);
  }

  rc = tcp_close(s, TIMEOUT);
  if (rc < 0)
    goto cleanup;
  else
    return;

cleanup:
  rc = hclose(s);
  assert(rc == 0);
}

int main() {
  int port = 1234;
  struct ipaddr addr;

  if (signal(SIGINT, sig_handler) == SIG_ERR) {
    perror("Can't register signal handlerx");
    return 1;
  }

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

  int b = bundle();
  if (b < 0) {
    perror("Can't create libdill bundle");
    return 1;
  }

  while (!done) {
    int s = tcp_accept(ls, NULL, now() + SOCKET_TIMEOUT);
    if (s < 0)
      continue;

    int cr = bundle_go(b, worker(s));
    if (cr < 0) {
      perror("Can't start a coroutine");
      return 1;
    }
  }

  rc = bundle_wait(b, now() + SOCKET_TIMEOUT);
  if (!(rc == 0 || (rc < 0 && errno == ETIMEDOUT))) {
    perror("Failed to wait for libdill bundle");
    return 1;
  }

  rc = hclose(b);
  if (rc < 0) {
    perror("Can't close libdill bundle");
    return 1;
  }

  rc = tcp_close(ls, TIMEOUT);
  if (rc < 0) {
    perror("Can't close TCP socket");
    return 1;
  }

  printf("Closed connections\n");

  return 0;
}
