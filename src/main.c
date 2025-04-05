#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#define PORT 8080
#define BACKLOG 10
#define BUFFER_SIZE 2048
#define MAX_PATH_LEN 256
#define MAX_METHOD_LEN 8
#define MAX_VERSION_LEN 10

typedef enum {
    OK,
    ERR
} Result;

typedef struct {
    char method[MAX_METHOD_LEN];
    char path[MAX_PATH_LEN];
    char version[MAX_VERSION_LEN];
} HttpRequest;

typedef struct {
    int status_code;
    const char *status_message;
    const char *content_type;
    char body[BUFFER_SIZE / 2];
    size_t body_len;
} HttpResponse;

Result setup_server_socket(int *server_fd) {
    struct sockaddr_in address;
    int opt = 1;

    *server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*server_fd == 0) {
        perror("socket failed");
        return ERR;
    }

    if (setsockopt(*server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        close(*server_fd);
        return ERR;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(*server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(*server_fd);
        return ERR;
    }

    if (listen(*server_fd, BACKLOG) < 0) {
        perror("listen failed");
        close(*server_fd);
        return ERR;
    }

    printf("Server listening on port %d\n", PORT);
    return OK;
}

Result parse_http_request(const char *raw_request, HttpRequest *request) {
    int res = sscanf(raw_request, "%7s %255s %9s",
                     request->method, request->path, request->version);
    if (res != 3) {
        fprintf(stderr, "Failed to parse request line: %s\n", raw_request);
        return ERR;
    }
    request->method[MAX_METHOD_LEN - 1] = '\0';
    request->path[MAX_PATH_LEN - 1] = '\0';
    request->version[MAX_VERSION_LEN - 1] = '\0';
    return OK;
}

Result prepare_http_response(const HttpRequest *request, HttpResponse *response) {
    if (strcmp(request->method, "GET") != 0) {
        response->status_code = 501;
        response->status_message = "Not Implemented";
        response->content_type = "text/plain";
        snprintf(response->body, sizeof(response->body), "Method Not Implemented");
    } else if (strcmp(request->path, "/") == 0) {
        response->status_code = 200;
        response->status_message = "OK";
        response->content_type = "text/html";
        snprintf(response->body, sizeof(response->body),
                 "<html><body><h1>Hello World!</h1></body></html>");
    } else {
        response->status_code = 404;
        response->status_message = "Not Found";
        response->content_type = "text/plain";
        snprintf(response->body, sizeof(response->body), "Resource Not Found");
    }

    response->body_len = strlen(response->body);
    return OK;
}


Result send_http_response(int client_fd, const HttpResponse *response) {
    char response_buffer[BUFFER_SIZE];
    int header_len = snprintf(response_buffer, sizeof(response_buffer),
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n"
                              "%s",
                              response->status_code,
                              response->status_message,
                              response->content_type,
                              response->body_len,
                              response->body);

    if (header_len < 0 || (size_t)header_len >= sizeof(response_buffer)) {
         fprintf(stderr, "Failed to format response or buffer too small\n");
         return ERR;
    }

    ssize_t bytes_sent;
    size_t total_sent = 0;
    size_t total_to_send = (size_t)header_len;

    while (total_sent < total_to_send) {
        bytes_sent = write(client_fd, response_buffer + total_sent, total_to_send - total_sent);
        if (bytes_sent < 0) {
            if (errno == EINTR) continue;
            perror("write failed");
            return ERR;
        }
        if (bytes_sent == 0) {
             fprintf(stderr, "Client closed connection unexpectedly\n");
             return ERR;
        }
        total_sent += (size_t)bytes_sent;
    }

    return OK;
}


Result handle_client_connection(int client_fd) {
    char request_buffer[BUFFER_SIZE];
    HttpRequest request = {0};
    HttpResponse response = {0};
    Result res = ERR;

    ssize_t bytes_read = read(client_fd, request_buffer, sizeof(request_buffer) - 1);

    if (bytes_read < 0) {
        perror("read failed");
    } else if (bytes_read == 0) {
        fprintf(stderr, "Client disconnected before sending data\n");
    } else {
        request_buffer[bytes_read] = '\0';

        if (parse_http_request(request_buffer, &request) == OK) {
            if (prepare_http_response(&request, &response) == OK) {
                res = send_http_response(client_fd, &response);
            }
        } else {
             response.status_code = 400;
             response.status_message = "Bad Request";
             response.content_type = "text/plain";
             snprintf(response.body, sizeof(response.body), "Bad Request");
             response.body_len = strlen(response.body);
             res = send_http_response(client_fd, &response);
        }
    }

    close(client_fd);
    return res;
}

int main() {
    int server_fd;
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    if (setup_server_socket(&server_fd) == ERR) {
        return 1;
    }

    while (1) {
        client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept failed");
            continue;
        }

        printf("Connection accepted\n");

        if (handle_client_connection(client_fd) == ERR) {
            fprintf(stderr, "Failed to handle client connection\n");
        }

        printf("Connection closed\n");
    }

    close(server_fd);
    return 0;
}