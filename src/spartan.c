#include <ctype.h>
#include <err.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "spartan.h"

struct mime {
  char *ext;
  char *type;
};

static const struct mime types[] = {
  {".gmi", "text/gemini"},
  {".txt", "text/plain"},
  {".jpg", "image/jpeg"},
  {".jpeg", "image/jpeg"},
  {".png", "image/png"},
  {".gif", "image/gif"},
  {".jxl", "image/jxl"},
  {".webp", "image/webp"},
  {".mp3", "audio/mpeg"},
  {".m4a", "audio/mp4"},
  {".mp4", "video/mp4"},
  {".wav", "audio/wav"},
  { 0, 0 },
};

const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                    "abcdefghijklmnopqrstuvwxyz0123456789"
                    " -._~:/?#[]@!$&'()*+,;=%\r\n";

char *fallback = "application/octet-stream";

static char *mime(const char *path) {
  char *ext = strrchr(path, '.');
  if(!ext)
    return fallback;
  for (int i = 0; types[i].ext != 0; i++) {
    if(!strcasecmp(ext, types[i].ext)) {
      return types[i].type;
    }
  }
  return fallback;
}

static void encode(char *src, char *dst) {
  unsigned char *s = (unsigned char *) src;
  if(!strlen((char *) s)) {
    dst[0] = '\0';
    return;
  }
  static char skip[256] = {0};
  if(!skip[(int) '-']) {
    unsigned int i;
    for(i = 0; i < 256; i++)
      skip[i] = strchr(valid, i) ? i : 0;
  }
  for(; *s; s++) {
    if(skip[(int) *s]) snprintf(dst, 2, "%c", skip[(int) *s]), ++dst;
    else {
      snprintf(dst, 4, "%%%02x", *s);
      dst += 3;
    }
  }
  *dst = '\0';
}

static int decode(const char *src, char *dst) {
  int pos = 0;
  char buf[3] = {0};
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
  const char *rest = buf;
  while(pos--) *dst++ = *rest++;
  *dst++ = '\0';
  return 0;
}

static void deliver(int server, char *buf, int len) {
  while(len > 0) {
    ssize_t ret = write(server, buf, len);
    if(ret == -1) errx(1, "write failed");
    buf += ret; len -= ret;
  }
}

static int header(struct request *req, int status, char *meta) {
  if(req->ongoing) return 1;
  if(strlen(meta) > 1024) return 1;
  char buf[HEADER];
  int len = snprintf(buf, HEADER, "%d %s\r\n", status, *meta ? meta : "");
  deliver(req->socket, buf, len);
  req->ongoing = 1;
  return 0;
}

static void transfer(const struct request *req, int fd) {
  char buf[BUFFER] = {0};
  ssize_t len;
  while((len = read(fd, buf, BUFFER)) > 0)
    deliver(req->socket, buf, len);
}

static int file(struct request *req, char *path) {
  int fd = open(path, O_RDONLY);
  if(fd == -1) return header(req, 4, "not found");
  char *type = mime(path);
  header(req, 2, type);
  transfer(req, fd);
  close(fd);
  return 0;
}

static void entry(const struct request *req, char *path) {
  struct stat sb = {0};
  if(stat(path, &sb) == -1) return;
  double size = sb.st_size / 1000.0;
  char buf[PATH_MAX * 2];
  char safe[strlen(path) * 3 + 1];
  encode(path, safe);
  const char *type = mime(path);
  int len = snprintf(buf, PATH_MAX * 2, "=> %s %s [%s %.2f KB]\n",
      safe, path, type, size);
  deliver(req->socket, buf, len);
}

static int ls(struct request *req) {
  struct stat sb = {0};
  int ok = stat("index.gmi", &sb);
  if(!ok && S_ISREG(sb.st_mode))
    return file(req, "index.gmi");
  header(req, 2, "text/gemini");
  glob_t res;
  if(glob("*", GLOB_MARK, 0, &res)) {
    char *empty = "(*^o^*)\r\n";
    deliver(req->socket, empty, strlen(empty));
    return 0;
  }
  for(size_t i = 0; i < res.gl_pathc; i++) {
    entry(req, res.gl_pathv[i]);
  }
  return 0;
}

static int cgi(struct request *req, char *path) {
  setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
  setenv("PATH_INFO", req->path ? req->path : "", 1);
  setenv("SCRIPT_NAME", path ? path : "", 1);
  setenv("REMOTE_ADDR", req->ip, 1);
  setenv("REMOTE_HOST", req->ip, 1);
  setenv("SERVER_PROTOCOL", "spartan", 1);

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

  char buf[BUFFER] = {0};
  ssize_t len;
  while((len = read(fd[0], buf, BUFFER)) > 0) {
    deliver(req->socket, buf, len);
  }
  kill(pid, SIGTERM);
  int status;
  if(waitpid(pid, &status, WNOHANG) == 0) {
    sleep(1);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
  return 0;
}

static int route(struct request *req) {
  if(!req->path)  {
    char url[HEADER];
    snprintf(url, HEADER, "%s/", req->cwd);
    if(!strlen(url)) return header(req, 4, "invalid request");
    char safe[strlen(url) * 3 + 1];
    encode(url, safe);
    return header(req, 3, safe);
  }
  if(!strcspn(req->path, "/")) return ls(req);

  char *path = strsep(&req->path, "/");
  struct stat sb = {0};
  if(stat(path, &sb) == -1)
    return header(req, 4, "not found");
  if(S_ISREG(sb.st_mode) && sb.st_mode & S_IXOTH) 
    return cgi(req, path);
  if(S_ISDIR(sb.st_mode)) {
    size_t current = strlen(req->cwd);
    int bytes = snprintf(req->cwd + current, HEADER - current, "/%s", path);
    if(bytes >= (int)(HEADER - current))
      return header(req, 4, "path too long");
    if(chdir(path)) return header(req, 4, "not found");
    return route(req);
  }
  return S_ISREG(sb.st_mode) ? file(req, path) : header(req, 4, "not found");
}

int spartan(struct request *req, char *url, int shared) {
  static char cwd[HEADER] = "";
  static char path[HEADER] = {0};

  cwd[0] = '\0';
  memset(path, 0, HEADER);

  size_t eof = strspn(url, valid);
  if(url[eof]) return header(req, 4, "invalid request");

  if(strlen(url) >= HEADER) return header(req, 4, "not found");
  if(strlen(url) <= 2) return header(req, 4, "not found");
  if(url[strlen(url) - 2] != '\r' || url[strlen(url) - 1] != '\n')
    return 1;
  url[strcspn(url, "\r\n")] = 0;

  req->time = time(0);

  char *domain = strsep(&url, " ");
  char *rawpath = strsep(&url, " ");
  if(!domain || !rawpath) return header(req, 4, "not found");
  if(!shared && chdir(domain)) return header(req, 4, "not found");

  const char *strlength = url;
  long length = strtol(strlength, 0, 10);

  decode(rawpath, path);

  if(*path != '/' || strstr(path, "..") || strstr(path, "//"))
    return header(req, 4, "invalid request");

  req->cwd = cwd;
  req->path = path + 1;
  req->length = length;

  syslog(LOG_INFO, "spartan://%s%s %s", domain, rawpath, req->ip);

  return route(req);
}
