#include "rf_powermon_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <netinet/in.h>
#include <netdb.h>
#include <termios.h>

#include "rf_powermon_cmds.h"

static struct sockaddr_in server = {
  .sin_family = AF_INET,
  .sin_port = 0,
  .sin_addr = { 0 },
  .sin_zero = { 0 },
};

static char com_path[32] = { 0 };
struct termios options;

typedef int (*cb_setup_t)(void);
typedef int (*cb_read_t)(int, char*);
typedef void (*cb_write_t)(int, const char*);

static cb_setup_t cb_setup = NULL;
static cb_read_t cb_read = NULL;
static cb_write_t cb_write = NULL;

static int socket_setup(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if(fd < 0) {
    return(-1);
  }

  int ret = connect(fd, (struct sockaddr*)&server, sizeof(struct sockaddr_in));
  if(ret) {
    return(-1);
  }

  return(fd);
}

static int socket_read(int socket, char* cmd_buff) {
  int len = 0;
  do {
    ioctl(socket, FIONREAD, &len);
  } while(!len);

  len = recv(socket, cmd_buff, len, 0);
  if(len < 0) {
    // something went wrong
    return(0);
  }
  cmd_buff[len] = '\0';

  // strip trailing newline if present
  if((strlen(cmd_buff) > 1) && (cmd_buff[strlen(cmd_buff) - 1] == '\n')) {
    cmd_buff[strlen(cmd_buff) - 1] = '\0';
  }

  return(len);
}

static void socket_write(int socket, const char* data) {
  (void)send(socket, data, strlen(data), 0);
}

static int serial_setup(void) {
  int fd = open(com_path, O_RDWR | O_NOCTTY | O_SYNC);
  tcflush(fd, TCIFLUSH);
  tcsetattr(fd, TCSANOW, &options);
  return(fd);
}

static int serial_read(int fd, char* cmd_buff) {
  char* ptr = cmd_buff;
  int n = 0;
  do {
    n = read(fd, ptr++, 1);
  } while(n > 0);
  int len = ptr - cmd_buff;
  cmd_buff[len] = '\0';
  return(len);
}

static void serial_write(int fd, const char* data) {
  (void)write(fd, data, strlen(data));
}

static int scpi_exec(const char* cmd, char* rpl_buff) {
  if(!cb_read || !cb_write || !cb_setup) {
    return(EXIT_FAILURE);
  }

  int fd = cb_setup();
  if(fd < 0) {
    return(EXIT_FAILURE);
  }
  
  cb_write(fd, cmd);
  if(rpl_buff) {
    (void)cb_read(fd, rpl_buff);
  }
  (void)close(fd);
  return(EXIT_SUCCESS);
}

int rf_powermon_init_socket(const char* hostname, int port) {
  struct hostent* hostnm = gethostbyname(hostname);
  if(!hostnm) {
    return(EXIT_FAILURE);
  }

  memcpy(&server.sin_addr, hostnm->h_addr_list[0], hostnm->h_length);
  server.sin_port = htons(port);
  cb_read = socket_read;
  cb_write = socket_write;
  cb_setup = socket_setup;
  return(EXIT_SUCCESS);
}

int rf_powermon_init_serial(const char* port, int speed) {
  strncpy(com_path, port, sizeof(com_path) - 1);

  options.c_cflag |= (CLOCAL | CREAD);
  options.c_cflag &= ~CSIZE;
  options.c_cflag |= CS8;
  options.c_cflag &= ~PARENB;
  options.c_cflag &= ~CSTOPB;
  options.c_cflag &= ~CRTSCTS;
  options.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
  options.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
  options.c_oflag &= ~OPOST;
  options.c_cc[VMIN] = 0;
  options.c_cc[VTIME] = 5; // 0.5 seconds read timeout

  // TODO support for other speed rates
  (void)
  cfsetispeed(&options, B115200);
  cfsetospeed(&options, B115200);

  cb_read = serial_read;
  cb_write = serial_write;
  cb_setup = serial_setup;
  return(EXIT_SUCCESS);
}

int rf_powermon_read(float* val) {
  char rpl_buff[256];
  int ret = scpi_exec(RF_POWERMON_CMD_READ_POWER, rpl_buff);
  if(val) { *val = strtof(rpl_buff, NULL); }
  return(ret);
}

int rf_powermon_exit() {
  return(scpi_exec(RF_POWERMON_CMD_SYSTEM_EXIT, NULL));
}

int rf_powermon_reset() {
  return(scpi_exec(RF_POWERMON_CMD_RESET, NULL));
}
