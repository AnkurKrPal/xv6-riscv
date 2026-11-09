#include "kernel/types.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/stat.h"
#include "user/user.h"

#define MAX_REQUEST 256
#define MAX_RESPONSE 4096
// xv6 has a one-page user stack; keep response storage in the data segment.
static char data[MAX_RESPONSE];

static int
allowed_path(char *path)
{
  int n = strlen(path);
  if (strcmp(path, "/demoFile.txt") == 0)
    return 1;
  if (n < 6 || (strcmp(path, "/agent") != 0 && path[6] != '/') ||
      path[0] != '/' || path[1] != 'a' || path[2] != 'g' ||
      path[3] != 'e' || path[4] != 'n' || path[5] != 't')
    return 0;
  for (int i = 0; i < n; i++)
    if ((uchar)path[i] <= ' ' || (uchar)path[i] >= 127 ||
        (path[i] == '.' && (i == 0 || path[i - 1] == '/') &&
         (path[i + 1] == 0 || path[i + 1] == '/' ||
          (path[i + 1] == '.' && (path[i + 2] == 0 || path[i + 2] == '/')))))
      return 0;
  return 1;
}

static int
parse_request(char *request, char *command, char *path)
{
  int i = 0, j = 0;
  while (request[i] != ' ' && request[i] != 0 && j < 15)
    command[j++] = request[i++];
  command[j] = 0;
  if (request[i++] != ' ')
    return 0;
  j = 0;
  while (request[i] != 0 && j < 127)
    path[j++] = request[i++];
  path[j] = 0;
  return request[i] == 0 && j > 0;
}

static void
respond(int id, int ok, char *data, int n)
{
  printf("RESULT %d %s %d\n", id, ok ? "OK" : "ERROR", n);
  if (n > 0)
    write(1, data, n);
  write(1, "\n", 1);
}

static void
list_files(int id, char *path)
{
  struct dirent de;
  struct stat st;
  int fd, n = 0, got;

  fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) < 0 || st.type != T_DIR) {
    if (fd >= 0)
      close(fd);
    respond(id, 0, 0, 0);
    return;
  }
  while ((got = read(fd, &de, sizeof(de))) == sizeof(de)) {
    int name_len = 0;
    if (de.inum == 0 ||
        (de.name[0] == '.' && (de.name[1] == 0 ||
         (de.name[1] == '.' && de.name[2] == 0))))
      continue;
    while (name_len < DIRSIZ && de.name[name_len] != 0)
      name_len++;
    if (n + name_len + 1 > MAX_RESPONSE) {
      close(fd);
      respond(id, 0, "directory exceeds bridge limit", 30);
      return;
    }
    memmove(data + n, de.name, name_len);
    n += name_len;
    data[n++] = '\n';
  }
  close(fd);
  respond(id, got == 0, got == 0 ? data : 0, got == 0 ? n : 0);
}

static void
read_file(int id, char *path)
{
  struct stat st;
  int fd, n = 0, got = 0;

  fd = open(path, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) < 0 || st.type != T_FILE) {
    if (fd >= 0)
      close(fd);
    respond(id, 0, 0, 0);
    return;
  }
  if (st.size > MAX_RESPONSE) {
    close(fd);
    respond(id, 0, "file exceeds bridge limit", 25);
    return;
  }
  while (n < MAX_RESPONSE &&
         (got = read(fd, data + n, MAX_RESPONSE - n)) > 0)
    n += got;
  close(fd);
  respond(id, got >= 0, got < 0 ? 0 : data, got < 0 ? 0 : n);
}

int
main(void)
{
  char request[MAX_REQUEST];
  char command[16];
  char path[128];
  int id = 0;

  printf("READY\n");
  while (1) {
    gets(request, sizeof(request));
    int n = strlen(request);
    if (n == 0)
      break;
    while (n > 0 && (request[n - 1] == '\n' || request[n - 1] == '\r'))
      request[--n] = 0;
    if (!parse_request(request, command, path) ||
        !allowed_path(path)) {
      respond(++id, 0, 0, 0);
      continue;
    }
    if (strcmp(command, "list_files") == 0)
      list_files(++id, path);
    else if (strcmp(command, "read_file") == 0)
      read_file(++id, path);
    else
      respond(++id, 0, 0, 0);
  }
  exit(0);
}
