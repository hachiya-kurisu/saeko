char *root = "/var/spartan"; // root path

char *user = "spartan"; // setuid
char *group = "spartan"; // setgid

// domains to serve
struct host hosts[] = {
  { "localhost", "." },
  { 0 }
};

