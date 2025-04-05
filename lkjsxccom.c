#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#define PORT 8080
#define MAX_CONNECTIONS 10
#define REQUEST_BUFFER_SIZE 16384
#define RESPONSE_BUFFER_SIZE 512
#define FILE_BUFFER_SIZE 1024
#define METHOD_MAX_LEN 16
#define URI_MAX_LEN 256
#define VERSION_MAX_LEN 16
#define FILE_PATH_MAX_LEN 512
#define BASE_ROUTE_PATH "./routes"

typedef enum {
    OK,
    ERR
} Result;

typedef struct {
    char method[METHOD_MAX_LEN];
    char uri[URI_MAX_LEN];
    char version[VERSION_MAX_LEN];
} HttpRequest;

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

Result send_all(int sockfd, const char *buffer, size_t length) {
    size_t total_sent = 0;
    while (total_sent < length) {
        ssize_t sent = send(sockfd, buffer + total_sent, length - total_sent, 0);
        if (sent < 0) {
            perror("send");
            return ERR;
        }
        if (sent == 0) {
             fprintf(stderr, "send: Connection closed unexpectedly\n");
             return ERR;
        }
        total_sent += (size_t)sent;
    }
    return OK;
}

Result send_response_header(int client_fd, int status_code, const char *content_type, long content_length) {
    char response_header[RESPONSE_BUFFER_SIZE];
    int header_len = snprintf(response_header, RESPONSE_BUFFER_SIZE,
                              "HTTP/1.1 %d %s\r\n"
                              "Content-Type: %s\r\n"
                              "Content-Length: %ld\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              status_code, http_status_message(status_code),
                              content_type, content_length);

    if (header_len < 0 || header_len >= RESPONSE_BUFFER_SIZE) {
        fprintf(stderr, "send_response_header: Failed to format header or buffer too small\n");
        return ERR;
    }

    return send_all(client_fd, response_header, (size_t)header_len);
}

Result send_error_response(int client_fd, int status_code) {
    char body[RESPONSE_BUFFER_SIZE];
    const char *status_msg = http_status_message(status_code);
    int body_len = snprintf(body, RESPONSE_BUFFER_SIZE,
                           "<html><body><h1>%d %s</h1></body></html>",
                           status_code, status_msg);

    if (body_len < 0 || body_len >= RESPONSE_BUFFER_SIZE) {
         fprintf(stderr, "send_error_response: Failed to format body or buffer too small\n");
         body_len = 0;
    }

    if (send_response_header(client_fd, status_code, "text/html", body_len) == ERR) {
        return ERR;
    }

    if (body_len > 0) {
        return send_all(client_fd, body, (size_t)body_len);
    }
    
    return OK; 
}


Result send_file_response(int client_fd, const char *file_path) {
    FILE *file = fopen(file_path, "rb");
    if (!file) {
        perror("fopen");
        perror(file_path);
        if (errno == ENOENT) {
            return send_error_response(client_fd, 404);
        } else {
            return send_error_response(client_fd, 500);
        }
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size < 0) {
        perror("ftell");
        fclose(file);
        return send_error_response(client_fd, 500);
    }

    if (send_response_header(client_fd, 200, "text/html", file_size) == ERR) {
        fclose(file);
        return ERR;
    }

    char file_buffer[FILE_BUFFER_SIZE];
    size_t bytes_read;
    while ((bytes_read = fread(file_buffer, 1, FILE_BUFFER_SIZE, file)) > 0) {
        if (send_all(client_fd, file_buffer, bytes_read) == ERR) {
            fclose(file);
            return ERR;
        }
    }

    if (ferror(file)) {
        perror("fread");
        fclose(file);
        return ERR;
    }

    fclose(file);
    return OK;
}

Result parse_request(const char *buffer, HttpRequest *req) {
    int matched = sscanf(buffer, "%15s %255s %15s", req->method, req->uri, req->version);
    if (matched != 3) {
        fprintf(stderr, "parse_request: Failed to parse request line\n");
        return ERR;
    }
    return OK;
}


Result handle_connection(int client_fd) {
    char request_buffer[REQUEST_BUFFER_SIZE];
    HttpRequest req;
    char file_path[FILE_PATH_MAX_LEN];
    Result res = OK;

    ssize_t bytes_received = recv(client_fd, request_buffer, REQUEST_BUFFER_SIZE - 1, 0);

    if (bytes_received < 0) {
        perror("recv");
        res = ERR;
    } else if (bytes_received == 0) {
        fprintf(stderr, "handle_connection: Client disconnected\n");
        res = ERR;
    } else {
        request_buffer[bytes_received] = '\0';

        if (parse_request(request_buffer, &req) == ERR) {
            res = send_error_response(client_fd, 400);
        } else {
            if (strcmp(req.method, "GET") != 0) {
                res = send_error_response(client_fd, 405);
            } else {
                int path_len = snprintf(file_path, FILE_PATH_MAX_LEN,
                                        "%s%s/page.html", BASE_ROUTE_PATH, req.uri);

                if (path_len < 0 || path_len >= FILE_PATH_MAX_LEN) {
                    fprintf(stderr, "handle_connection: File path too long or formatting error\n");
                    res = send_error_response(client_fd, 500);
                } else {
                    res = send_file_response(client_fd, file_path);
                }
            }
        }
    }

    close(client_fd);
    return res;
}


int setup_server(int port) {
    int server_fd;
    struct sockaddr_in address;
    int opt = 1;

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("socket failed");
        return -1;
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) {
        perror("setsockopt SO_REUSEADDR");
        close(server_fd);
        return -1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        close(server_fd);
        return -1;
    }

    if (listen(server_fd, MAX_CONNECTIONS) < 0) {
        perror("listen failed");
        close(server_fd);
        return -1;
    }

    printf("Server listening on port %d\n", port);
    return server_fd;
}

int main() {
    int server_fd = setup_server(PORT);
    if (server_fd < 0) {
        return EXIT_FAILURE;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_addr_len);

        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        printf("Connection accepted\n");
        
        if (handle_connection(client_fd) == ERR) {
             fprintf(stderr, "Failed to handle connection fully.\n");
        } else {
             printf("Connection handled successfully.\n");
        }
    }

    close(server_fd);
    return EXIT_SUCCESS;
}