// see us after school for copyright and license details

#define _PR_HAVE_LARGE_OFF_T

#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <syslog.h>
#include <grp.h>
#include <pwd.h>
#include <err.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <magic.h>

#define HEADER 1027
#define BUFFER 65536

struct host {
  char *domain, *root;
};

#include "../config.h"

magic_t cookie;
const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz0123456789"
                    " -._~:/?#[]@!$&'()*+,;=%\r\n";

struct request {
  int client;
  time_t time;
  char url[HEADER];
  int ongoing;
  long length;
  char *cwd, *path, *query, *strlength;
  char *ip;
};

int cgi(struct request *, char *);
int file(struct request *, char *);

int setdomain(char *domain) {
  struct host *host = hosts;
  while(host->domain) {
    if(!strcmp(domain, host->domain)) {
      return chdir(host->root) ? 0 : 1;
    }
    host++;
  }
  return 0;
}

int dig(char *path, char *dst, char *needle) {
  FILE *fp = fopen(path, "r");
  if(!fp) return -1;
  char buf[LINE_MAX];
  int retval = 0;
  while(fgets(buf, LINE_MAX, fp)) {
    buf[strcspn(buf, "\r\n")] = 0;
    if(needle && !strcmp(buf, needle)) {
      retval = 1;
      break;
    } else if(dst) {
      strlcpy(dst, buf, LINE_MAX);
      break;
    }
  }
  fclose(fp);
  return retval;
}

char *mime(char *path) {
  char *type = (char *) magic_file(cookie, path);
  if(!strncmp(type, "text/", 5)) return text;
  return type;
}

void attr(const char *subject, char *key, char *dst) {
  char needle[128] = { 0 };
  snprintf(needle, 128, "/%s=", key);
  char *found = strstr(subject, needle);
  if(found) {
    found += strlen(needle);
    char *end = strchr(found, '/');
    snprintf(dst, 128, "%.*s", (int) (end - found), found);
  }
}

void encode(char *src, char *dst) {
  unsigned char *s = (unsigned char *) src;
  if(!strlen((char *) s)) {
    dst[0] = '\0';
    return;
  }
  static char skip[256] = { 0 };
  if(!skip[(int) '-']) {
    unsigned int i;
    for(i = 0; i < 256; i++)
      skip[i] = strchr(valid, i) ? i : 0;
  }
  for(; *s; s++) {
    if(skip[(int) *s]) snprintf(dst, 1, "%c", skip[(int) *s]), ++dst;
    else {
      snprintf(dst, 3, "%%%02x", *s);
      while (*++dst);
    }
  }
}

int decode(char *src, char *dst) {
  int pos = 0;
  char buf[3] = { 0 };
  unsigned int decoded;
  while(src && *src) {
    buf[pos] = *src;
    if(pos == 2) {
      if(buf[0] == '%' && isxdigit(buf[1]) && isxdigit(buf[2])) {
        sscanf(buf, "%%%2x", &decoded);
        *dst++ = decoded;
        memset(buf, 0, 3);
        pos = 0;
      } else {
        *dst++ = buf[0];
        memmove(buf, &buf[1], 2);
        buf[2] = 0;
      }
    } else {
      pos++;
    }
    src++;
  }
  char *rest = buf;
  while(pos--) *dst++ = *rest++;
  *dst++ = '\0';
  return 0;
}

void deliver(int server, char *buf, int len) {
  while(len > 0) {
    ssize_t ret = write(server, buf, len);
    if(ret == -1) errx(1, "write failed");
    buf += ret; len -= ret;
  }
}

int header(struct request *req, int status, char *meta) {
  if(req->ongoing) return 1;
  if(strlen(meta) > 1024) return 1;
  char buf[HEADER];
  int len = snprintf(buf, HEADER, "%d %s\r\n", status, *meta ? meta : "");
  deliver(req->client, buf, len);
  req->ongoing = 1;
  return 0;
}

void include(struct request *req, char *buf) {
  buf += strspn(buf, " \t");
  buf[strcspn(buf, "\r\n")] = 0;
  struct stat sb = { 0 };
  stat(buf, &sb);
  if(S_ISREG(sb.st_mode) && sb.st_mode & S_IXOTH) {
    cgi(req, buf);
  } else if(S_ISREG(sb.st_mode)) {
    file(req, buf);
  }
}

void process(struct request *req, int fd) {
  FILE *fp = fdopen(fd, "r");
  if(!fp) return;
  char buf[LINE_MAX];
  while(fgets(buf, LINE_MAX, fp)) {
    if(!strncmp(buf, "$>", 2) && strlen(buf) > 2) {
      include(req, &buf[2]);
    } else {
      deliver(req->client, buf, strlen(buf));
    }
  }
}

void transfer(struct request *req, int fd) {
  char buf[BUFFER] = { 0 };
  ssize_t len;
  while((len = read(fd, buf, BUFFER)) != 0)
    deliver(req->client, buf, len);
}

int file(struct request *req, char *path) {
  int fd = open(path, O_RDONLY);
  if(fd == -1) return header(req, 4, "not found");
  char *type = mime(path);
  header(req, 2, type);
  (wild && !strncmp(type, "text/", 5)) ? process(req, fd) : transfer(req, fd);
  transfer(req, fd);
  close(fd);
  return 0;
}

void entry(struct request *req, char *path) {
  struct stat sb = { 0 };
  stat(path, &sb);
  double size = sb.st_size / 1000.0;
  char full[PATH_MAX];
  snprintf(full, PATH_MAX, "%s/%s", req->cwd, path);
  char buf[PATH_MAX * 2];
  char safe[strlen(path) * 3 + 1];
  encode(path, safe);
  char *type = mime(path);
  int len = snprintf(buf, PATH_MAX * 2, "=> %s %s [%s %.2f KB]\n",
      safe, path, type, size);
  deliver(req->client, buf, len);
}

int ls(struct request *req) {
  struct stat sb = { 0 };
  stat("index.gmi", &sb);
  if(S_ISREG(sb.st_mode))
    return file(req, "index.gmi");
  header(req, 2, text);
  glob_t res;
  if(glob("*", GLOB_MARK, 0, &res)) {
    char *empty = "(*^o^*)\r\n";
    deliver(req->client, empty, strlen(empty));
    return 0;
  }
  for(size_t i = 0; i < res.gl_pathc; i++) {
    entry(req, res.gl_pathv[i]);
  }
  return 0;
}

int cgi(struct request *req, char *path) {
  setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
  setenv("QUERY_STRING", req->query ? req->query : "", 1);
  setenv("PATH_INFO", req->path ? req->path : "", 1);
  setenv("SCRIPT_NAME", path ? path : "", 1);
  setenv("CONTENT_LENGTH", req->strlength, 1);
  setenv("REMOTE_ADDR", req->ip, 1);
  setenv("REMOTE_HOST", req->ip, 1);
  setenv("SERVER_PROTOCOL", "spartan", 1);
  setenv("SERVER_PORT", "300", 1);
  setenv("SERVER_SOFTWARE", "冴子/202205", 1);

  int fd[2];
  pipe(fd);

  pid_t pid = fork();
  if(pid == -1) errx(1, "fork failed");

  if(!pid) {
    dup2(fd[1], 1);
    close(fd[0]);
    char *argv[] = { path, 0 };
    execv(path, argv);
  }
  close(fd[1]);

  char buf[BUFFER] = { 0 };
  ssize_t len;
  while((len = read(fd[0], buf, BUFFER)) != 0) {
    deliver(req->client, buf, len);
  }
  kill(pid, SIGKILL);
  wait(0);
  return 0;
}

int fallback(struct request *req, char *notfound) {
  char path[LINE_MAX];
  snprintf(path, LINE_MAX, "%s.gmi", notfound);
  struct stat sb = { 0 };
  stat(path, &sb);
  return S_ISREG(sb.st_mode) ? file(req, path) : header(req, 4, "not found");
}

int route(struct request *req) {
  if(!req->path)  {
    char url[HEADER];
    snprintf(url, HEADER, "%s/", req->cwd);
    if(!strlen(url)) return header(req, 4, "bad request");
    char safe[strlen(url) * 3 + 1];
    encode(url, safe);
    return header(req, 3, safe);
  }
  if(!strcspn(req->path, "/")) return ls(req);

  char *path = strsep(&req->path, "/");
  struct stat sb = { 0 };
  stat(path, &sb);
  if(S_ISREG(sb.st_mode) && sb.st_mode & S_IXOTH) 
    return cgi(req, path);
  if(S_ISDIR(sb.st_mode)) {
    snprintf(req->cwd + strlen(req->cwd), LINE_MAX, "/%s", path);
    if(chdir(path)) return header(req, 4, "not found");
    return route(req);
  }
  return S_ISREG(sb.st_mode) ? file(req, path) : fallback(req, path);
}

int saeko(struct request *req, char *url) {
  size_t eof = strspn(url, valid);
  if(url[eof]) return header(req, 4, "bad");

  if(strlen(url) >= HEADER) return header(req, 4, "something bad happened");
  if(strlen(url) <= 2) return header(req, 4, "bad request: no");
  if(url[strlen(url) - 2] != '\r' || url[strlen(url) - 1] != '\n')
    return 1;
  url[strcspn(url, "\r\n")] = 0;

  req->time = time(0);

  char *domain = strsep(&url, " ");
  char *rawpath = strsep(&url, " ");
  char *strlength = url;
  long length = strtol(strlength, 0, 10);

  if(!setdomain(domain)) return header(req, 4, "refused");

  char cwd[HEADER] = "";
  char path[HEADER] = { 0 };
  char query[HEADER] = { 0 };

  decode(rawpath, path);

  if(!*path || *path != '/' || strstr(path, "..") || strstr(path, "//"))
    return header(req, 4, "nah");

  req->cwd = cwd;
  req->path = path + 1;
  req->query = query;
  req->length = length;
  req->strlength = strlength;

  syslog(LOG_INFO, "spartan://%s%s %s", domain, rawpath, req->ip);

  return route(req);
}

int main() {
  cookie = magic_open(MAGIC_MIME_TYPE);
  magic_load(cookie, 0);
  tzset();

  struct sockaddr_in6 addr;
  int server = socket(AF_INET6, SOCK_STREAM, 0);

  bzero(&addr, sizeof(addr));
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  addr.sin6_port = htons(300);

  struct timeval timeout;
  timeout.tv_sec = 10;

  int opt = 1;
  setsockopt(server, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  if(bind(server, (struct sockaddr *) &addr, (socklen_t) sizeof(addr)))
    errx(1, "bind totally failed %d", errno);

  struct group *grp = { 0 };
  struct passwd *pwd = { 0 };

  if(group && !(grp = getgrnam(group)))
    errx(1, "group %s not found", group);

  if(user && !(pwd = getpwnam(user)))
    errx(1, "user %s not found", user);

  daemon(0, 0);

  if(secure && chroot(root)) errx(1, "chroot failed");
  if(chdir(secure ? "/" : root)) errx(1, "chdir failed");

  openlog("saeko", LOG_NDELAY, LOG_DAEMON);

  if(group && grp && setgid(grp->gr_gid)) errx(1, "setgid failed");
  if(user && pwd && setuid(pwd->pw_uid)) errx(1, "setuid failed");

  if(pledge("stdio inet proc dns exec rpath wpath cpath getpw unix flock", 0))
    errx(1, "pledge failed");

  listen(server, 32);

  int sock;
  socklen_t len = sizeof(addr);
  while((sock = accept(server, (struct sockaddr *) &addr, &len)) > -1) {
    pid_t pid = fork();
    if(pid == -1) errx(1, "fork failed");
    if(!pid) {
      close(server);
      struct request req = { 0 };
      char url[HEADER] = { 0 };
      if(read(sock, url, HEADER) == -1) {
        close(sock);
        errx(1, "read failed");
      }
      char ip[INET6_ADDRSTRLEN];
      inet_ntop(AF_INET6, &addr, ip, INET6_ADDRSTRLEN);
      req.client = sock;
      req.ip = ip;
      saeko(&req, url);
      close(sock);
    } else {
      close(sock);
      signal(SIGCHLD, SIG_IGN);
    }
  }
  close(server);
  closelog();
  return 0;
}

