#include <winsock2.h>
#include <ws2tcpip.h> // for struct sockaddr_storage
#define _SS_MAXSIZE 128                 /* Maximum size. */
#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL
