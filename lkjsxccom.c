#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#define PORT 8080
#define MAX_CONNECTIONS 10
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

struct http_request {
    char method[METHOD_MAX_LEN];
    char uri[URI_MAX_LEN];
    char version[VERSION_MAX_LEN];
};

struct server_socket {
    int socket_fd;
};

struct client_connection {
    int client_fd;
};

static char response_buffer[RESPONSE_BUFFER_SIZE];

const char *http_status_message(int status_code) {
    switch (status_code) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 500: return "Internal Server Error";
        default: return "Unknown Status";
    }
}

enum result send_all(int sockfd, const char *buffer, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (sent < 0) {
            return RESULT_ERR;
        }
        if (sent == 0) {
             return RESULT_ERR;
        }
        total_sent += (size_t)sent;
    }
    return RESULT_OK;
}

enum result send_response_header(int client_fd, int status_code, const char *content_type, long content_length) {
    int header_len = snprintf(response_buffer, RESPONSE_BUFFER_SIZE,
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %ld\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status_code, http_status_message(status_code),
                              content_type, content_length);

    if (header_len < 0 || header_len >= RESPONSE_BUFFER_SIZE) {
        return RESULT_ERR;
    }

    return send_all(client_fd, response_buffer, (size_t)header_len);
}

enum result send_error_response(int client_fd, int status_code) {
    char body[ERRORRESPONSE_BUFFER_SIZE];
    const char *status_msg = http_status_message(status_code);
    int body_len = snprintf(body, ERRORRESPONSE_BUFFER_SIZE,
                           "<html><body><h1>%d %s</h1></body></html>",
                           status_code, status_msg);

    if (body_len < 0 || body_len >= ERRORRESPONSE_BUFFER_SIZE) {
         body_len = 0;
    }

    if (send_response_header(client_fd, status_code, "text/html", body_len) == RESULT_ERR) {
        return RESULT_ERR;
    }

    if (body_len > 0) {
        return send_all(client_fd, body, (size_t)body_len);
    }

    return RESULT_OK;
}


enum result send_file_contents(int client_fd, FILE *file) {
    char file_buffer[FILE_BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(file_buffer, 1, FILE_BUFFER_SIZE, file)) > 0) {
        if (send_all(client_fd, file_buffer, bytes_read) == RESULT_ERR) {
            return RESULT_ERR;
        }
    }

    if (ferror(file)) {
        return RESULT_ERR;
    }
    return RESULT_OK;
}

enum result send_file_response(int client_fd, const char *file_path) {
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        if (errno == ENOENT) {
            return send_error_response(client_fd, 404);
        } else {
            return send_error_response(client_fd, 500);
        }
    }

    enum result res = RESULT_ERR;
    if (fseek(file, 0, SEEK_END) != 0) {
        send_error_response(client_fd, 500);
        fclose(file);
        return res;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        send_error_response(client_fd, 500);
        fclose(file);
        return res;
    }

    if (fseek(file, 0, SEEK_SET) != 0) {
        send_error_response(client_fd, 500);
        fclose(file);
        return res;
    }

    if (send_response_header(client_fd, 200, "text/html", file_size) == RESULT_ERR) {
        fclose(file);
        return res;
    }

    res = send_file_contents(client_fd, file);
    fclose(file);
    return res;
}

enum result parse_request(const char *buffer, struct http_request *req) {
    int matched = sscanf(buffer, "%15s %255s %15s", req->method, req->uri, req->version);
    if (matched != 3) {
        return RESULT_ERR;
    }
    return RESULT_OK;
}


enum result handle_get_request(int client_fd, const struct http_request *req) {
    char file_path[FILE_PATH_MAX_LEN];
    int path_len = snprintf(file_path, FILE_PATH_MAX_LEN,
                            "%s%s/page.html", BASE_ROUTE_PATH, req->uri);

    if (path_len < 0 || path_len >= FILE_PATH_MAX_LEN) {
        return send_error_response(client_fd, 500);
    } else {
        return send_file_response(client_fd, file_path);
    }
}

enum result process_request(int client_fd, const char *request_buffer) {
    struct http_request req;
    if (parse_request(request_buffer, &req) == RESULT_ERR) {
        return send_error_response(client_fd, 400);
    }

    if (strcmp(req.method, "GET") != 0) {
        return send_error_response(client_fd, 405);
    }

    return handle_get_request(client_fd, &req);
}

enum result handle_connection(struct client_connection conn) {
    char request_buffer[REQUEST_BUFFER_SIZE];
    ssize_t bytes_received;
    enum result res;

    bytes_received = recv(conn.client_fd, request_buffer, REQUEST_BUFFER_SIZE - 1, 0);

    if (bytes_received < 0) {
        res = RESULT_ERR;
    } else if (bytes_received == 0) {
        res = RESULT_ERR;
    } else {
        request_buffer[bytes_received] = '\0';
        res = process_request(conn.client_fd, request_buffer);
    }

    close(conn.client_fd);
    return res;
}


enum result setup_server(int port, struct server_socket *server) {
    struct sockaddr_in address;
    int opt = 1;
    int server_fd = -1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return RESULT_ERR;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(server_fd);
        return RESULT_ERR;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(server_fd);
        return RESULT_ERR;
    }

    if (listen(server_fd, MAX_CONNECTIONS) < 0) {
        close(server_fd);
        return RESULT_ERR;
    }

    server->socket_fd = server_fd;
    printf("Server listening on port %d\n", port);
    return RESULT_OK;
}

enum result accept_connection(struct server_socket server, struct client_connection *conn) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_fd = accept(server.socket_fd, (struct sockaddr *)&client_addr, &client_addr_len);

    if (client_fd < 0) {
        perror("accept failed");
        conn->client_fd = -1;
        return RESULT_ERR;
    }
    conn->client_fd = client_fd;
    return RESULT_OK;
}


int main() {
    struct server_socket server;

    if (setup_server(PORT, &server) == RESULT_ERR) {
        perror("Server setup failed");
        return 1;
    }

    while (1) {
        struct client_connection conn;

        if (accept_connection(server, &conn) == RESULT_OK) {
             printf("Connection accepted\n");
             if (handle_connection(conn) == RESULT_ERR) {
                 fprintf(stderr, "Failed to handle connection fully.\n");
             } else {
                 printf("Connection handled successfully.\n");
             }
        }
    }

    close(server.socket_fd);
    return 0;
}