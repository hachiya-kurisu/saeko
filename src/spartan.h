#ifndef SPARTAN_H
#define SPARTAN_H

#define HEADER 1028
#define BUFFER 65536

// handles a spartan request and sends a response
// returns 0 on success, 1 on error
int spartan(int sock, char *url, int shared);

#endif
