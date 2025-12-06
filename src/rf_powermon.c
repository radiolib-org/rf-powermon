#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include <lgpio.h>
#include "argtable3/argtable3.h"
#include "socket/socket.h"

#include "rf-powermon-client/rf_powermon_cmds.h"

#ifndef GITREV
#define GITREV "unknown"
#endif

#define STR_HELPER(s) #s    
#define STR(s) STR_HELPER(s)     

// some default configuration values
#define PORT_DEFAULT          "/dev/ttyACM0"
#define SPEED_DEFAULT         115200
#define FREQ_DEFAULT          434
#define ATTEN_DEFAULT         0.0
#define WINDOW_DEFAULT        512
#define CONTROL_DEFAULT       41122

// buffer to save incoming data into
#define BUFF_SIZE             4096
static char rx_buff[BUFF_SIZE] = { 0 };

// buffer for commands from socket
static char socket_buff[256] = { 0 };

// app configuration structure
static struct conf_t {
  char port[64];
  int speed;
  int freq;
  float atten;
  int window;
  int serial_fd;
  int socket_fd;
} conf = {
  .port = PORT_DEFAULT,
  .speed = SPEED_DEFAULT,
  .freq = FREQ_DEFAULT,
  .atten = ATTEN_DEFAULT,
  .window = WINDOW_DEFAULT,
  .serial_fd = -1,
  .socket_fd = -1,
};

// units returned by the meter
enum sample_abs_level_unit {
  ABS_LEVEL_MICROWATT = 'u',
  ABS_LEVEL_MILLIWATT = 'm',
  ABS_LEVEL_WATT = 'w',
};

// structure to save data about a single sample
// TODO add timestamp
struct sample_t {
  float dbm;
  float abs_level;
  enum sample_abs_level_unit abs_unit;
};

// structure holding information about the minimum and maximum
static struct stats_t {
  struct sample_t min;
  struct sample_t max;
  struct sample_t avg;
} stats = {
  .min = { .dbm = 99,   .abs_level = 1000,  .abs_unit = ABS_LEVEL_WATT },
  .max = { .dbm = -99,  .abs_level = 0,     .abs_unit = ABS_LEVEL_MICROWATT },
  .avg = { .dbm = 0,    .abs_level = 0,     .abs_unit = ABS_LEVEL_MICROWATT },
};

// averaging window
static float avg_window[BUFF_SIZE] = { 0 };
static float* avg_ptr = avg_window;

// argtable arguments
static struct args_t {
  struct arg_str* port;
  struct arg_int* speed;
  struct arg_int* freq;
  struct arg_dbl* atten;
  struct arg_int* window;
  struct arg_int* control;
  struct arg_lit* help;
  struct arg_end* end;
} args;

static void sighandler(int signal) {
  (void)signal;
  exit(EXIT_SUCCESS);
}

static void exithandler(void) {
  fprintf(stdout, "\n");
  fflush(stdout);
  int ret = lgSerialClose(conf.serial_fd);
  if(ret < 0) {
    fprintf(stderr, "ERROR: Failed to close COM port (%s)\n", lguErrorText(ret));
  }
  close(conf.socket_fd);
}

static void stats_reset() {
  stats.min.dbm = 99; stats.min.abs_level = 1000;
  stats.max.dbm = -99; stats.max.abs_level = 0;
  stats.avg.dbm = 0; stats.avg.abs_level = 0;
}

static void stats_update(struct sample_t* sample) {
  // update statistics
  if(sample->dbm < stats.min.dbm) {
    stats.min.dbm = sample->dbm;
  } else if(sample->dbm > stats.max.dbm) {
    stats.max.dbm = sample->dbm;
  }

  // calculate the average
  *avg_ptr = sample->dbm;
  stats.avg.dbm = 0;
  for(int i = 0; i < conf.window; i++) {
    stats.avg.dbm += avg_window[i];
  }
  stats.avg.dbm /= conf.window;
  avg_ptr++;
  if((avg_ptr - avg_window) > conf.window) {
    avg_ptr = avg_window;
  }
}

static void process_socket_cmd(int fd, char* cmd) {
  char buff[64] = { 0 };
  if(strstr(cmd, RF_POWERMON_CMD_READ_POWER) == cmd) {
    sprintf(buff, "%.2fdBm" RF_POWERMON_RSP_LINEFEED, (double)stats.avg.dbm);

  } else if(strstr(cmd, RF_POWERMON_CMD_RESET) == cmd) {
    stats_reset();
    sprintf(buff, RF_POWERMON_RSP_LINEFEED);
  
  } else if(strstr(cmd, RF_POWERMON_CMD_ID) == cmd) {
    sprintf(buff, "radiolib-org,RFpowerMon," GITREV RF_POWERMON_RSP_LINEFEED);

  } else if(strstr(cmd, RF_POWERMON_CMD_SYSTEM_EXIT) == cmd) {
    raise(SIGINT);

  } else {
    // TODO handle invalid commands?
    fprintf(stderr, "invalid socket cmd: %s\n", cmd);

  }


  socket_write(fd, buff);
}

static char* strchr_first(char* s, const char* delims) {
  // by default, set the result to point at the final NULL in the input string
  // we know that this will never match any of our delimiters
  int len_str = strlen(s);
  char* ptr_first = s + len_str + 1;

  // grab the first delimiter and start iterating
  char* ptr;
  char delim = delims[0];
  while(delim) {
    ptr = strchr(s, delim);

    // if it was a hit, and is better than our current best, then save it
    if(ptr && (ptr < ptr_first)) {
      ptr_first = ptr;
    }

    delim = *delims++;
  }

  // if this was never changed, we did not find any of the delimiters
  if(ptr_first == s + len_str + 1) {
    return(NULL);
  }

  return(ptr_first);
}

static void parse_sample(char* sample_str, struct sample_t* sample) {
  if(!sample_str || (strlen(sample_str) != 10)) {
    //fprintf(stderr, "Invalid sample: %s\n", sample_str);
    return;
  }

  // format is fixed-width
  char buff[8] = { 0 };

  // <1 char dbm sign><3 chars dbm level * 10>
  memcpy(buff, &sample_str[1], 3);
  int dbm_i = atoi(buff);
  if(sample_str[0] == '-') { dbm_i *= -1; }
  sample->dbm = (float)dbm_i / 10.0f;

  // <5 chars absolute level * 100><1 char absolute level unit>
  memcpy(buff, &sample_str[4], 5);
  sample->abs_level = (float)atoi(buff) / 100.0f;
  sample->abs_unit = sample_str[9];
}

static void process_rx(char* data, int len) {
  if(!data) {
    return;
  }

  // discard everything up to the first delimiter
  const char delims[] = "-+";
  char* ptr = data;
  char* ptr_prev = strchr_first(data, delims);
  if(!ptr_prev) {
    return;
  }
  
  char buff[16] = { 0 };
  while(ptr - data < len) {
    // find the next delimiter occurence
    ptr = strchr_first(ptr_prev + 1, delims);
    if(!ptr) { break; }

    // copy string between those two pointers
    memcpy(buff, ptr_prev, ptr - ptr_prev);

    // parse the sample
    struct sample_t sample = { 0 };
    parse_sample(buff, &sample);

    // update statistics
    stats_update(&sample);

    // print the result
    fprintf(stdout, " %5.1f dBm  %6.2f %c    %5.1f dBm  %5.1f dBm  %5.1f dBm\r", 
      (double)sample.dbm,
      (double)sample.abs_level,
      sample.abs_unit,
      (double)stats.min.dbm,
      (double)stats.max.dbm,
      (double)stats.avg.dbm);
    fflush(stdout);

    // clear the buffer and advance pointer
    memset(buff, 0, sizeof(buff));
    ptr_prev = ptr;
  }
}

static int set_config(int handle, int freq, float atten) {
  char buff[32];
  sprintf(buff, "A%04d+%.2f\r\n", freq, (double)atten);
  return(lgSerialWrite(handle, buff, strlen(buff)));
}

static int run() {
  // open the port
  conf.serial_fd = lgSerialOpen(conf.port, conf.speed, 0);
  if(conf.serial_fd < 0) {
    fprintf(stderr, "ERROR: Failed to open COM port (%s)\n", lguErrorText(conf.serial_fd));
    return(conf.serial_fd);
  }

  // send the configuration
  int ret = set_config(conf.serial_fd, conf.freq, conf.atten);
  if(ret != 0) {
    return(ret);
  }

  // counters
  int av_count = 0;
  int rx_count = 0;
  int read_socket_fd = 0;
  fprintf(stdout, " Relative   Absolute    Minimum    Maximum     Average\n");

  // run until killed by the user
  for(;;) {
    // check how many bytes are available
    av_count = lgSerialDataAvailable(conf.serial_fd);
    while(av_count) {
      // read up to the buffer size
      rx_count = lgSerialRead(conf.serial_fd, rx_buff, av_count > BUFF_SIZE ? BUFF_SIZE : av_count);
      //fprintf(stdout, "read %d: %s\n", rx_count, rx_buff);

      process_rx(rx_buff, rx_count);

      // clear the buffer
      memset(rx_buff, 0, BUFF_SIZE);
      av_count -= rx_count;
    }

    // check if there is something to read from the socket
    read_socket_fd = socket_read(conf.socket_fd, socket_buff);
    if(read_socket_fd > 0) {
      // got something, process it
      process_socket_cmd(read_socket_fd, socket_buff);

      // close the socket now, we're done with it
      close(read_socket_fd);
    }
    
    // some small sleep to stop the thread for a bit
    usleep(1000);
  }

  return(0);
}

int main(int argc, char** argv) {
  void *argtable[] = {
    args.port = arg_str0("p", "port", NULL, "Which serial port to use, defaults to" STR(PORT_DEFAULT)),
    args.speed = arg_int0("s", "speed", "baud", "Port speed, defaults to " STR(SPEED_DEFAULT)),
    args.freq = arg_int0("f", "frequency", "MHz", "Frequency to set, defaults to " STR(FREQ_DEFAULT)),
    args.atten = arg_dbl0("a", "attenuation", "dB", "Attenuation to set, defaults to " STR(ATTEN_DEFAULT)),
    args.window = arg_int0("w", "window", NULL, "Averaging window length, defaults to " STR(WINDOW_DEFAULT)),
    args.control = arg_int0("c", "control", "port", "Control port for socket connection, defaults to " STR(CONTROL_DEFAULT)),
    args.help = arg_lit0(NULL, "help", "Display this help and exit"),
    args.end = arg_end(2),
  };

  int exitcode = 0;
  if(arg_nullcheck(argtable) != 0) {
    fprintf(stderr, "%s: insufficient memory\n", argv[0]);
    exitcode = 1;
    goto exit;
  }

  int nerrors = arg_parse(argc, argv, argtable);
  if(args.help->count > 0) {
    fprintf(stdout, "RF power monitor, gitrev " GITREV "\n");
    fprintf(stdout, "Usage: %s", argv[0]);
    arg_print_syntax(stdout, argtable, "\n");
    fprintf(stdout, "After start, send SIGINT /Ctrl+C/ to stop\n");
    arg_print_glossary(stdout, argtable,"  %-25s %s\n");
    exitcode = 0;
    goto exit;
  }

  if(nerrors > 0) {
    arg_print_errors(stdout, args.end, argv[0]);
    fprintf(stderr, "Try '%s --help' for more information.\n", argv[0]);
    exitcode = 1;
    goto exit;
  }

  atexit(exithandler);
  signal(SIGINT, sighandler);

  if(args.port->count) { strncpy(conf.port, args.port->sval[0], sizeof(conf.port) - 1); }
  if(args.speed->count) { conf.speed = args.speed->ival[0]; }
  if(args.freq->count) { conf.freq = args.freq->ival[0]; }
  if(args.atten->count) { conf.atten = args.atten->dval[0]; }
  if(args.window->count) { conf.window = args.window->ival[0]; }

  // set up the socket
  int socket_port = CONTROL_DEFAULT;
  if(args.control->count) { socket_port = args.control->ival[0]; };
  conf.socket_fd = socket_setup(socket_port);

  exitcode = run();

exit:
  arg_freetable(argtable, sizeof(argtable)/sizeof(argtable[0]));

  return exitcode;
}
