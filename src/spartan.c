#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
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

static struct request {
  time_t time;
  int socket;
  int ongoing;
  long length;
  char *cwd;
  char *path;
} req;

static const char *types[][2] = {
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
  {0, 0},
};

const char *valid = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
"abcdefghijklmnopqrstuvwxyz0123456789"
" -._~:/?#[]@!$&'()*+,;=%\r\n";

const char *fallback = "application/octet-stream";

static const char *mime(const char *path) {
  const char *ext = strrchr(path, '.');
  if(!ext)
    return fallback;
  for (int i = 0; types[i][0] != 0; i++) {
    if(!strcasecmp(ext, types[i][0])) {
      return types[i][1];
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
    for(int i = 0; i < 256; i++)
      skip[i] = strchr(valid, i) ? (char)i : 0;
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
        *dst++ = (char)decoded;
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

static void deliver(int server, const char *buf, ssize_t len) {
  while(len > 0) {
    ssize_t ret = write(server, buf, (size_t)len);
    if(ret == -1) errx(1, "write failed");
    buf += ret; len -= ret;
  }
}

static int header(int status, const char *meta) {
  if(req.ongoing) return 1;
  if(strlen(meta) > 1024) return 1;
  char buf[HEADER];
  int len = snprintf(buf, HEADER, "%d %s\r\n", status, *meta ? meta : "");
  deliver(req.socket, buf, len);
  req.ongoing = 1;
  if(status != 2)
    syslog(LOG_WARNING, "%s - %d: %s", req.path, status, meta);
  return 0;
}

static int file(const char *path) {
  int fd = open(path, O_RDONLY);
  if(fd == -1) return header(4, "not found");
  const char *type = mime(path);
  header(2, type);

  char buf[BUFFER] = {0};
  ssize_t ret;
  while((ret = read(fd, buf, BUFFER)) > 0)
    deliver(req.socket, buf, (ssize_t)ret);
  if(ret == -1) errx(1, "read error: %s", strerror(errno));
  close(fd);
  return 0;
}

static void entry(char *path) {
  struct stat sb = {0};
  if(stat(path, &sb) == -1) return;
  double size = (double)sb.st_size / 1000.0;
  char buf[PATH_MAX * 2];
  char safe[strlen(path) * 3 + 1];
  encode(path, safe);
  const char *type = mime(path);
  int len = snprintf(buf, PATH_MAX * 2, "=> %s %s [%s %.2f KB]\n",
      safe, path, type, size);
  deliver(req.socket, buf, len);
}

static int ls(void) {
  struct stat sb = {0};
  int ok = stat("index.gmi", &sb);
  if(!ok && S_ISREG(sb.st_mode))
    return file("index.gmi");
  header(2, "text/gemini");
  glob_t res;
  if(glob("*", GLOB_MARK, 0, &res)) {
    const char *empty = "(*^o^*)\r\n";
    deliver(req.socket, empty, (ssize_t)strlen(empty));
    return 0;
  }
  for(size_t i = 0; i < res.gl_pathc; i++) {
    entry(res.gl_pathv[i]);
  }
  globfree(&res);
  return 0;
}

static int cgi(char *path) {
  setenv("GATEWAY_INTERFACE", "CGI/1.1", 1);
  setenv("PATH_INFO", req.path ? req.path : "", 1);
  setenv("SCRIPT_NAME", path ? path : "", 1);
  setenv("SERVER_PROTOCOL", "spartan", 1);

  char len[32];
  snprintf(len, sizeof(len), "%ld", req.length);
  setenv("CONTENT_LENGTH", len, 1);

  pid_t pid = fork();
  if(pid == -1) errx(1, "fork failed");

  if(!pid) {
    dup2(req.socket, 0);
    dup2(req.socket, 1);
    close(req.socket);
    char *argv[] = { path, 0 };
    alarm(30);
    execv(path, argv);
    _exit(1);
  }

  int status;
  waitpid(pid, &status, 0);
  return 0;
}

static int route(void) {
  if(!req.path)  {
    char url[HEADER];
    snprintf(url, HEADER, "%s/", req.cwd);
    if(!strlen(url)) return header(4, "invalid request");
    char safe[strlen(url) * 3 + 1];
    encode(url, safe);
    return header(3, safe);
  }
  if(*req.path == '\0') return ls();

  char *path = strsep(&req.path, "/");
  struct stat sb = {0};
  if(stat(path, &sb) == -1)
    return header(4, "not found");
  if(S_ISREG(sb.st_mode) && sb.st_mode & 0111) 
    return cgi(path);
  if(S_ISDIR(sb.st_mode)) {
    size_t current = strlen(req.cwd);
    int bytes = snprintf(req.cwd + current, HEADER - current, "/%s", path);
    if(bytes >= (int)(HEADER - current))
      return header(4, "path too long");
    if(chdir(path)) return header(4, "not found");
    return route();
  }
  return file(path);
}

int spartan(int sock, char *url, int shared) {
  char cwd[HEADER] = "";
  char path[HEADER] = {0};

  req.socket = sock;

  cwd[0] = '\0';
  memset(path, 0, HEADER);

  size_t eof = strspn(url, valid);
  if(url[eof]) return header(4, "invalid request");

  if(strlen(url) >= HEADER) return header(4, "not found");
  if(strlen(url) <= 2) return header(4, "not found");
  if(url[strlen(url) - 2] != '\r' || url[strlen(url) - 1] != '\n')
    return 1;
  url[strcspn(url, "\r\n")] = 0;

  req.time = time(0);

  const char *domain = strsep(&url, " ");
  const char *rawpath = strsep(&url, " ");
  if(!domain || !rawpath) return header(4, "invalid request");
  if(!url) return header(4, "invalid request");

  if(!shared && chdir(domain)) return header(4, "not found");

  long length = strtol(url, 0, 10);
  if (length < 0)
    return header(4, "invalid request");

  decode(rawpath, path);
  if(*path != '/' || strstr(path, "..") || strstr(path, "//"))
    return header(4, "invalid request");

  req.cwd = cwd;
  req.path = path + 1;
  req.length = length;

  return route();
}
