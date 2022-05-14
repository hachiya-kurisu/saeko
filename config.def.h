int secure = 1; // try to chroot?
int wild = 1; // wild mode

char *root = "/var/spartan"; // root path

char text[LINE_MAX] = "text/gemini"; // plaintext mime type

char *user = "spartan"; // setuid
char *group = "spartan"; // setgid

// domains to serve
struct host hosts[] = {
  { "localhost", "gmidocs" },
  { 0 }
};

