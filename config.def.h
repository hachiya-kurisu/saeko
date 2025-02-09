char *root = "/var/spartan"; // root path

char *user = "spartan"; // setuid
char *group = "spartan"; // setgid

char *fallback = "application/octet-stream"; // fallback mime type

// domains to serve
struct host hosts[] = {
  { "localhost", "." },
  { 0 }
};

