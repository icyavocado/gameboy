#include "gb.h"
#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
  int socket;
  int failed;
} link_t;

static uint8_t *read_file(const char *path, size_t *size) {
  FILE *file = fopen(path, "rb");
  long length;
  uint8_t *data;
  if (!file || fseek(file, 0, SEEK_END) || (length = ftell(file)) < 0 ||
      fseek(file, 0, SEEK_SET)) {
    if (file) fclose(file);
    return NULL;
  }
  data = malloc((size_t)length);
  if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) {
    free(data);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return data;
}

static void transfer(void *user, uint8_t out, uint8_t *in) {
  link_t *link = user;
  ssize_t n;
  do n = send(link->socket, &out, 1, 0); while (n < 0 && errno == EINTR);
  if (n != 1) {
    link->failed = 1;
    *in = 0xff;
    return;
  }
  do n = recv(link->socket, in, 1, MSG_WAITALL); while (n < 0 && errno == EINTR);
  if (n != 1) {
    link->failed = 1;
    *in = 0xff;
  }
}

static int connect_peer(const char *host, const char *port) {
  struct sockaddr_in address = {0};
  char *end;
  unsigned long value = strtoul(port, &end, 10);
  int socket_fd;
  if (*port == '\0' || *end || value > 65535) return -1;
  socket_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (socket_fd < 0) return -1;
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)value);
  if (inet_pton(AF_INET, host, &address.sin_addr) != 1 ||
      connect(socket_fd, (struct sockaddr *)&address, sizeof address) < 0) {
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

static int listen_peer(const char *port) {
  struct sockaddr_in address = {0};
  char *end;
  unsigned long value = strtoul(port, &end, 10);
  int server, socket_fd, enabled = 1;
  if (*port == '\0' || *end || value > 65535) return -1;
  server = socket(AF_INET, SOCK_STREAM, 0);
  if (server < 0) return -1;
  setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof enabled);
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_ANY);
  address.sin_port = htons((uint16_t)value);
  if (bind(server, (struct sockaddr *)&address, sizeof address) < 0 ||
      listen(server, 1) < 0) {
    close(server);
    return -1;
  }
  socket_fd = accept(server, NULL, NULL);
  close(server);
  return socket_fd;
}

int main(int argc, char **argv) {
  const char *rom_path, *mode, *host, *port;
  unsigned frames = 0;
  char *end;
  size_t rom_size;
  uint8_t *rom;
  gb_t *gb;
  link_t link;
  int socket_fd;

  if ((argc != 4 && argc != 5 && argc != 6) ||
      (strcmp(argv[2], "listen") == 0 && argc != 4 && argc != 5) ||
      (strcmp(argv[2], "connect") == 0 && argc != 5 && argc != 6) ||
      (strcmp(argv[2], "listen") != 0 && strcmp(argv[2], "connect") != 0)) {
    fprintf(stderr, "usage: %s ROM listen PORT [frames]\n"
                    "       %s ROM connect HOST PORT [frames]\n",
            argv[0], argv[0]);
    return 2;
  }
  rom_path = argv[1];
  mode = argv[2];
  host = strcmp(mode, "listen") == 0 ? NULL : argv[3];
  port = strcmp(mode, "listen") == 0 ? argv[3] : argv[4];
  if ((strcmp(mode, "listen") == 0 && argc == 5) ||
      (strcmp(mode, "connect") == 0 && argc == 6)) {
    unsigned long value = strtoul(argv[argc - 1], &end, 10);
    if (*argv[argc - 1] == '-' || *argv[argc - 1] == '\0' || *end ||
        value > UINT_MAX)
      return 2;
    frames = (unsigned)value;
  }
  rom = read_file(rom_path, &rom_size);
  if (!rom) return 1;
  socket_fd = strcmp(mode, "listen") == 0 ? listen_peer(port)
                                            : connect_peer(host, port);
  if (socket_fd < 0) {
    free(rom);
    return 1;
  }
  gb = gb_create();
  if (!gb || gb_load_rom(gb, rom, rom_size)) {
    close(socket_fd);
    free(rom);
    gb_destroy(gb);
    return 1;
  }
  free(rom);
  link.socket = socket_fd;
  link.failed = 0;
  gb_set_serial_callback(gb, transfer, &link);
  while (frames--) gb_run_frame(gb);
  gb_destroy(gb);
  close(socket_fd);
  return link.failed ? 1 : 0;
}
