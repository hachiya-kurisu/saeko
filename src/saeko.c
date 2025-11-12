// see us after school for copyright and license details

#include <err.h>
#include <errno.h>
#include <grp.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>

#include "spartan.h"

const int backlog = 32;

char *root = "/var/spartan";
char *user = "www";
char *group = "www";

char *addr = "::1";
char *port = "300";

int shared = 0;

char *flags = "[-ds] [-u user] [-g group] [-a address] [-p port] [-r root]";

static void usage(const char *name) {
  fprintf(stderr, "usage: %s %s\n", name, flags);
}

int main(int argc, char *argv[]) {
  int debug = 0;
  int c;
  while((c = getopt(argc, argv, "dhsu:g:a:p:r:")) != -1) {
    switch(c) {
      case 'd': debug = 1; break;
      case 's': shared = 1; break;
      case 'u': user = optarg; break;
      case 'g': group = optarg; break;
      case 'a': addr = optarg; break;
      case 'p': port = optarg; break;
      case 'r': root = optarg; break;
      default: usage(argv[0]); exit(0);
    }
  }

  if(!strtonum(port, 1, 65535, 0)) errx(1, "invalid port");

  tzset();

  struct addrinfo hints, *res;
  bzero(&hints, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;

  int err = getaddrinfo(addr, port, &hints, &res);
  if(err)
    errx(1, "getaddrinfo: %s", gai_strerror(err));

  int server = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
  if(server == -1)
    errx(1, "socket failed");

  struct timeval tv = {.tv_sec = 10};

  int opt = 1;
  if(setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1)
    errx(1, "setsockopt TCP_NODELAY failed");
  if(setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    errx(1, "setsockopt SO_REUSEADDR failed");
  if(setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
    errx(1, "setsockopt SO_RCVTIMEO failed");
  if(setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1)
    errx(1, "setsockopt SO_SNDTIMEO failed");

  if(bind(server, res->ai_addr, res->ai_addrlen))
    errx(1, "bind failed: %s", strerror(errno));

  freeaddrinfo(res);

  struct group *grp = {0};
  struct passwd *pwd = {0};

  if(group && !(grp = getgrnam(group)))
    errx(1, "group %s not found", group);

  if(user && !(pwd = getpwnam(user)))
    errx(1, "user %s not found", user);

  if(chdir(root)) errx(1, "chdir failed");

  openlog("saeko", LOG_NDELAY, LOG_DAEMON);

  if(group && setgid(grp->gr_gid)) errx(1, "setgid failed");
  if(user && setuid(pwd->pw_uid)) errx(1, "setuid failed");

#ifdef __OpenBSD__
  if(!debug) daemon(0, 0);
  if(unveil(root, "rwxc")) errx(1, "unveil failed");
  if(pledge("stdio inet proc exec rpath wpath cpath unix flock", 0))
    errx(1, "pledge failed");
#endif

  if(listen(server, backlog)) errx(1, "listen failed");
  if(debug) fprintf(stderr, "listening on %s:%s\n", addr, port);

  signal(SIGCHLD, SIG_IGN);

  struct sockaddr_storage client;
  socklen_t len = sizeof(client);

  int sock;
  while((sock = accept(server, (struct sockaddr *) &client, &len)) > -1) {
    pid_t pid = fork();
    if(pid == -1) errx(1, "fork failed");
    if(!pid) {
      close(server);
      setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

      struct request req = {0};
      char url[HEADER] = {0};
      ssize_t bytes = read(sock, url, HEADER - 1);
      if(bytes <= 0) {
        close(sock);
        if(bytes == 0)
          errx(1, "disconnected");
        else
          errx(1, "read failed");
      }
      url[bytes] = '\0';

      char ip[INET6_ADDRSTRLEN];
      void *ptr;
      if(client.ss_family == AF_INET) {
        ptr = &((struct sockaddr_in *)&client)->sin_addr;
      } else {
        ptr = &((struct sockaddr_in6 *)&client)->sin6_addr;
      }
      if(!inet_ntop(client.ss_family, ptr, ip, INET6_ADDRSTRLEN))
        errx(1, "inet_ntop failed");

      req.socket = sock;
      req.ip = ip;
      if(debug) fprintf(stderr, "request to %s", url);
      spartan(&req, url, shared);
      close(sock);
      _exit(0);
    } else {
      close(sock);
    }
  }
  close(server);
  closelog();
  return 0;
}
