#include <assert.h>
#include <libdill.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rpa_queue.h"

#define MESSAGE_BUF_SZ 1024u
#define SOCKET_TIMEOUT 1000
#define TIMEOUT -1
#define QUEUE_CAPACITY 100u

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
  char name[256];
  char value[256];

  http_recvrequest(s, name, sizeof(name), value, sizeof(value), TIMEOUT);

  printf("=====\n");
  printf("%s %s\n=====\n", name, value);

  int is_post = !strncmp(name, "POST", 4);

  while (1) {
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

  if (is_post && content_length) {
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

void *slave(void *q) {
  rpa_queue_t *queue = (rpa_queue_t *)q;

  int b = bundle();
  if (b < 0) {
    perror("Can't create libdill bundle");
    return NULL;
  }

  while (1) {
    int s;
    if (!rpa_queue_pop(queue, (void **)&s)) {
      fprintf(stderr, "Can't pop item off a queue\n");
    }

    if (s == -1)
      break;

    int cr = bundle_go(b, worker(s));
    if (cr < 0) {
      perror("Can't start a coroutine");
      return NULL;
    }
  }

  int rc = bundle_wait(b, now() + SOCKET_TIMEOUT);
  if (!(rc == 0 || (rc < 0 && errno == ETIMEDOUT))) {
    perror("Failed to wait for libdill bundle");
    return NULL;
  }

  return NULL;
}

int main(int argc, char *argv[]) {
  // TO-DO handler signals in a multi-thread environment
  if (signal(SIGINT, sig_handler) == SIG_ERR) {
    perror("Can't register signal handler");
    return 1;
  }

  int port = 1234;
  if (argc > 1)
    port = atoi(argv[1]);

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

  int b = bundle();
  if (b < 0) {
    perror("Can't create libdill bundle");
    return 1;
  }

  // prepare the threads
  unsigned c_proc = 0;
  unsigned n_proc = 4;

  rpa_queue_t *queues = (rpa_queue_t *)malloc(n_proc * sizeof(rpa_queue_t));
  pthread_t *threads = (pthread_t *)malloc(n_proc * sizeof(pthread_t));

  for (unsigned i = 0; i < n_proc; ++i) {
    rpa_queue_t *q = &queues[i];
    if (!rpa_queue_create(&q, QUEUE_CAPACITY)) {
      perror("Can't initialize a queue");
      return 1;
    }

    int rc = pthread_create(&threads[i], NULL, slave, (void*) q);
    if(rc < 0) {
      perror("Can't create a thread");
      return 1;
    }
  }

  while (!done) {
    int s = tcp_accept(ls, NULL, now() + SOCKET_TIMEOUT);
    if (s < 0)
      continue;

    if (!rpa_queue_push(&queues[c_proc], (void*)(uintptr_t)(s))) {
      perror("Can't push to a queue");
      return 1;
    }

    c_proc = (c_proc + 1) % n_proc;
  }

  // signal an end to the threads
  for (unsigned i = 0; i < n_proc; ++i) {
    if (!rpa_queue_push(&queues[c_proc], (void *)-1)) {
      perror("Can't push to a queue");
      return 1;
    }
  }

  // join the threads
  for (unsigned i = 0; i < n_proc; ++i) {
    int rc = pthread_join(threads[i], NULL);
    if(rc < 0) {
      perror("Can't join a thread");
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
