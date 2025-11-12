#ifndef SPARTAN_H
#define SPARTAN_H

#include <time.h>

#define HEADER 1028
#define BUFFER 65536

// a spartan request
struct request {
  int socket;
  time_t time;
  int ongoing;
  long length;

  char *cwd;
  char *path;

  char *ip;
};

// handles a spartan request and sends a response
// returns 0 on success, 1 on error
int spartan(struct request *req, char *url, int shared);

#endif
