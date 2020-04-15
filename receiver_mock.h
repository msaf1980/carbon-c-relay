#ifndef _RECEIVER_MOCK_H_
#define _RECEIVER_MOCK_H_

#include <sys/types.h>

#define MAX_CLIENTS 32

typedef struct _receiver receiver;

receiver *receiver_new(char *addr, unsigned short port, size_t qsize, char noread, char reset);
unsigned short receiver_get_port(receiver *r);
int receiver_start(receiver *r, char bind_only);
void receiver_stop(receiver *r);
void receiver_free(receiver *r);

#endif /* _RECEIVER_MOCK_H_ */
