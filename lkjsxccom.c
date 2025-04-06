#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define MAX_CONNECTIONS 12
#define REQUEST_BUFFER_SIZE 2048
#define RESPONSE_BUFFER_SIZE 1024 * 1024 * 16
#define ERRORRESPONSE_BUFFER_SIZE 2048
#define FILE_BUFFER_SIZE 1024
#define METHOD_MAX_LEN 16
#define URI_MAX_LEN 256
#define VERSION_MAX_LEN 16
#define FILE_PATH_MAX_LEN 512
#define BASE_ROUTE_PATH "./routes"

enum result {
    RESULT_OK,
    RESULT_ERR
};

enum method {
    METHOD_GET,
    METHOD_POST,
};

enum status_code {
    STATUS_OK = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_NOT_FOUND = 404,
    STATUS_INTERNAL_SERVER_ERROR = 500
};

struct connection {
    struct connection* next;
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t addr_len;
    char request_buffer[REQUEST_BUFFER_SIZE];
    char response_buffer[RESPONSE_BUFFER_SIZE];
    char file_buffer[FILE_BUFFER_SIZE];
    char method[METHOD_MAX_LEN];
    char uri[URI_MAX_LEN];
    char version[VERSION_MAX_LEN];
    char file_path[FILE_PATH_MAX_LEN];
    int status_code;
};

static struct connection connection[MAX_CONNECTIONS];
static struct connection* connection_active;
static struct connection* connection_free;