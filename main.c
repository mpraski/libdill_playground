#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libdill.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if defined __APPLE__ || __OpenBSD__ || __FreeBSD__ || __DragonFly__
#include <sys/sysctl.h>
#include <sys/types.h>
#elif defined __linux__
#include <sys/sysinfo.h>
#endif

#include "rpa_queue.h"

#define TIMEOUT -1
#define MESSAGE_BUF_SZ 1024u
#define QUEUE_CAPACITY 64u

volatile sig_atomic_t done;

static void sig_handler(int sig, siginfo_t *siginfo, void *context) {
  if (sig == SIGINT) {
    done = 1;
  }
}

static int set_sig_handler() {
  struct sigaction act;

  memset(&act, 0, sizeof(act));

  act.sa_sigaction = &sig_handler;
  act.sa_flags = SA_SIGINFO | SA_RESTART;

  return sigaction(SIGINT, &act, NULL);
}

static int block_signal(int sig) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, sig);

  return pthread_sigmask(SIG_BLOCK, &set, NULL);
}

static void unblock(int s) {
  int opt = fcntl(s, F_GETFL, 0);
  if (opt == -1) opt = 0;

  int rc = fcntl(s, F_SETFL, opt | O_NONBLOCK);
  assert(rc == 0);

  opt = 1;
  rc = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  assert(rc == 0);
}

static int cpu_num() {
#if defined __APPLE__ || __OpenBSD__ || __FreeBSD__ || __DragonFly__
  int mib[4];
  int num_proc;
  size_t num_proc_len = sizeof(num_proc);

  mib[0] = CTL_HW;
  mib[1] = HW_AVAILCPU;

  sysctl(mib, 2, &num_proc, &num_proc_len, NULL, 0);

  if (num_proc < 1) {
    mib[1] = HW_NCPU;
    sysctl(mib, 2, &num_proc, &num_proc_len, NULL, 0);
    if (num_proc < 1) num_proc = 1;
  }

  return num_proc;
#elif defined __linux__
  return get_nprocs();
#endif
}

static coroutine void worker(int s) {
  s = http_attach(s);
  if (s < 0) goto cleanup;

  int rc;
  unsigned long content_length;
  char name[256];
  char value[256];

  http_recvrequest(s, name, sizeof(name), value, sizeof(value), -1);

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
  if (s < 0) goto cleanup;

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
      if (rc == -1 && errno == ECANCELED) goto cleanup;
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

static void *slave(void *q) {
  int rc = block_signal(SIGINT);
  if (rc < 0) {
    perror("Can't block signals");
    return NULL;
  }

  rpa_queue_t *queue = (rpa_queue_t *)q;

  while (1) {
    int s;
    if (!rpa_queue_pop(queue, (void **)&s)) {
      fprintf(stderr, "Can't pop item off a queue\n");
      return NULL;
    }

    if (s == -1) break;

    int rc = fdin(s, -1);
    if (rc < 0) {
      perror("OS socket not readable");
      return NULL;
    }

    s = tcp_fromfd(s);
    if (s < 0) {
      perror("Can't wrap an OS connection");
      return NULL;
    }

    int cr = go(worker(s));
    if (cr < 0) {
      perror("Can't start a coroutine");
      return NULL;
    }
  }

  return NULL;
}

int main(int argc, char *argv[]) {
  // set signal handler
  int rc = set_sig_handler();
  if (rc < 0) {
    perror("Can't register signal handler");
    return 1;
  }

  int port = 1234;
  if (argc > 1) port = atoi(argv[1]);

  struct sockaddr_in serv_addr, cli_addr;
  socklen_t cli_len = sizeof(cli_addr);

  // create the socket
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    perror("Failed to open socket");
    return 1;
  }

  unblock(fd);
  bzero((char *)&serv_addr, sizeof(serv_addr));

  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);

  // bind the socket
  rc = bind(fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr));
  if (rc < 0) {
    perror("Failed to bind socket");
    return 1;
  }

  // listen for connections
  rc = listen(fd, 64);
  if (rc < 0) {
    perror("Failed to listen on socket");
    return 1;
  }

  // prepare the threads
  int c_proc = 0;
  int n_proc = cpu_num() - 1;

  if (!n_proc) {
    fprintf(stderr, "only one cpu, aborting...\n");
    return 1;
  }

  rpa_queue_t **queues = (rpa_queue_t **)malloc(n_proc * sizeof(rpa_queue_t *));
  pthread_t *threads = (pthread_t *)malloc(n_proc * sizeof(pthread_t));

  // start the threads
  for (int i = 0; i < n_proc; ++i) {
    if (!rpa_queue_create(&queues[i], QUEUE_CAPACITY)) {
      perror("Can't initialize a queue");
      return 1;
    }

    int rc = pthread_create(&threads[i], NULL, slave, (void *)queues[i]);
    if (rc < 0) {
      perror("Can't create a thread");
      return 1;
    }
  }

  // main accept loop
  while (!done) {
    int s = accept(fd, (struct sockaddr *)&cli_addr, &cli_len);
    if (s < 0) continue;

    if (!rpa_queue_push(queues[c_proc], (void *)(uintptr_t)(s))) {
      perror("Can't push to a queue");
      return 1;
    }

    printf(">> New connection %d on thread %d\n", s, c_proc);

    c_proc = (c_proc + 1 == n_proc ? 0 : c_proc + 1);
  }

  printf("\nClosing connections...\n");

  // signal an end to the threads
  for (int i = 0; i < n_proc; ++i) {
    if (!rpa_queue_push(queues[i], (void *)-1)) {
      perror("Can't push to a queue");
      return 1;
    }
  }

  // join the threads
  for (int i = 0; i < n_proc; ++i) {
    int rc = pthread_join(threads[i], NULL);
    if (rc < 0) {
      perror("Can't join a thread");
      return 1;
    }
    printf("Thread %d finished\n", i);
  }

  // close the socket
  rc = close(fd);
  if (rc < 0) {
    perror("Can't close the socket");
    return 1;
  }

  printf("Closed connections\n");

  return 0;
}
