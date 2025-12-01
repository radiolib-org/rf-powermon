#ifndef POWERMON_SOCKET_H
#define POWERMON_SOCKET_H

int socket_setup(int port);
int socket_read(int listen_fd, char* cmd_buff);
void socket_write(int socket, char* data);

#endif
