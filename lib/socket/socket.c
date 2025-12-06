#include "socket.h"

#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <sys/ioctl.h>

int socket_setup(int port) {
  // set up the ingest socket
  int control_socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in srv_addr = {
    .sin_family = AF_INET,
    .sin_addr = { .s_addr = htonl(INADDR_ANY), },
    .sin_port = htons(port),
    .sin_zero = { 0 },
  };

  if(bind(control_socket_fd, (struct sockaddr*)&srv_addr, sizeof(srv_addr)) != 0) {
    fprintf(stderr, "Failed to bind command ingest socket, errno %d.\n", errno);
    return(-1);
  }
  
  // make the socket non-blocking
  fcntl(control_socket_fd, F_SETFL, fcntl(control_socket_fd, F_GETFL, 0) | O_NONBLOCK);
  
  int yes = 1;
  setsockopt(control_socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

  // start listening
  if(listen(control_socket_fd, 10) != 0) {
    fprintf(stderr, "Failed to start listening for commands, errno %d.\n", errno);
    return(-1);
  }

  return(control_socket_fd);
}

int socket_read(int listen_fd, char* cmd_buff) {
  int cmd_conn_fd = accept(listen_fd, (struct sockaddr*)NULL, NULL);
  if(cmd_conn_fd < 0) {
    // nothing to do
    return(0);
  }

  int len = 0;
  do {
    ioctl(cmd_conn_fd, FIONREAD, &len);
  } while(!len);
  len = recv(cmd_conn_fd, cmd_buff, len, 0);
  if(len < 0) {
    // something went wrong
    return(0);
  }
  cmd_buff[len] = '\0';

  return(cmd_conn_fd);
}

void socket_write(int socket, char* data) {
  send(socket, data, strlen(data), 0);
}
