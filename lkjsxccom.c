#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define PORT 8080
#define MAX_CONNECTIONS 24
#define REQUEST_BUFFER_SIZE 2048

#define RESPONSE_BUFFER_SIZE 2048
#define ERRORRESPONSE_BUFFER_SIZE 2048
#define FILE_BUFFER_SIZE 4096
#define METHOD_MAX_LEN 16
#define URI_MAX_LEN 256
#define VERSION_MAX_LEN 16
#define FILE_PATH_MAX_LEN 512
#define BASE_ROUTE_PATH "./routes"

enum result {
    RESULT_OK,
    RESULT_ERR,
    RESULT_ERR_AGAIN
};

enum method {
    METHOD_GET,
    METHOD_UNKNOWN
};

enum status_code {
    STATUS_OK = 200,
    STATUS_BAD_REQUEST = 400,
    STATUS_NOT_FOUND = 404,
    STATUS_INTERNAL_SERVER_ERROR = 500,
    STATUS_METHOD_NOT_ALLOWED = 405
};

struct connection {
    struct connection* next;
    int client_fd;

    char request_buffer[REQUEST_BUFFER_SIZE];
    size_t request_len;

    enum method method;
    char uri[URI_MAX_LEN];

    char response_header_buffer[RESPONSE_BUFFER_SIZE];
    char file_buffer[FILE_BUFFER_SIZE];
    char file_path[FILE_PATH_MAX_LEN];
    int status_code;
    FILE* file_to_send;
    long file_size;
    long bytes_sent;
    int headers_sent;
};

struct connection connection_pool[MAX_CONNECTIONS];
struct connection* connection_active = NULL;
struct connection* connection_free = NULL;

void initialize_connection_pool() {
    connection_free = NULL;
    connection_active = NULL;
    for (int i = MAX_CONNECTIONS - 1; i >= 0; --i) {
        connection_pool[i].client_fd = -1;
        connection_pool[i].next = connection_free;
        connection_free = &connection_pool[i];
    }
    printf("Connection pool initialized with %d connections.\n", MAX_CONNECTIONS);
}

struct connection* get_free_connection() {
    if (connection_free == NULL) {
        return NULL;
    }
    struct connection* conn = connection_free;
    connection_free = conn->next;

    conn->next = connection_active;
    connection_active = conn;

    conn->request_len = 0;
    conn->file_to_send = NULL;
    conn->file_size = 0;
    conn->bytes_sent = 0;
    conn->headers_sent = 0;
    conn->status_code = STATUS_OK;

    memset(conn->request_buffer, 0, REQUEST_BUFFER_SIZE);
    memset(conn->response_header_buffer, 0, RESPONSE_BUFFER_SIZE);
    memset(conn->file_buffer, 0, FILE_BUFFER_SIZE);
    memset(conn->uri, 0, URI_MAX_LEN);
    memset(conn->file_path, 0, FILE_PATH_MAX_LEN);

    return conn;
}

void release_connection(struct connection* conn) {
    if (conn == NULL)
        return;

    struct connection** ptr = &connection_active;
    while (*ptr != NULL) {
        if (*ptr == conn) {
            *ptr = conn->next;
            break;
        }
        ptr = &((*ptr)->next);
    }

    conn->next = connection_free;
    connection_free = conn;

    if (conn->client_fd != -1) {
        close(conn->client_fd);
        conn->client_fd = -1;
    }
    if (conn->file_to_send != NULL) {
        fclose(conn->file_to_send);
        conn->file_to_send = NULL;
    }
    printf("Released connection for fd %d\n", conn->client_fd);
}

void close_connection(struct connection* conn) {
    if (conn) {
        printf("Closing connection for fd %d\n", conn->client_fd);
        release_connection(conn);
    }
}

enum result setup_server_socket(int* listen_fd) {
    struct sockaddr_in server_addr;

    *listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (*listen_fd < 0) {
        perror("socket failed");
        return RESULT_ERR;
    }

    int opt = 1;
    if (setsockopt(*listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(*listen_fd);
        return RESULT_ERR;
    }

    int flags = fcntl(*listen_fd, F_GETFL, 0);
    if (flags == -1) {
        perror("fcntl(F_GETFL) failed");
        close(*listen_fd);
        return RESULT_ERR;
    }
    if (fcntl(*listen_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        perror("fcntl(F_SETFL O_NONBLOCK) failed");
        close(*listen_fd);
        return RESULT_ERR;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(PORT);

    if (bind(*listen_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(*listen_fd);
        return RESULT_ERR;
    }

    if (listen(*listen_fd, MAX_CONNECTIONS) < 0) {
        perror("listen failed");
        close(*listen_fd);
        return RESULT_ERR;
    }

    printf("Server listening on port %d\n", PORT);
    return RESULT_OK;
}

enum result parse_request(struct connection* conn) {
    char method_str[METHOD_MAX_LEN];
    char uri_str[URI_MAX_LEN];
    char version_str[VERSION_MAX_LEN];

    conn->request_buffer[conn->request_len] = '\0';

    int items_parsed = sscanf(conn->request_buffer, "%15s %255s %15s", method_str, uri_str, version_str);

    if (items_parsed < 2) {
        fprintf(stderr, "Failed to parse request (items parsed: %d): %s\n", items_parsed, conn->request_buffer);
        conn->status_code = STATUS_BAD_REQUEST;
        return RESULT_ERR;
    }

    if (strncmp(method_str, "GET", METHOD_MAX_LEN) == 0) {
        conn->method = METHOD_GET;
    } else {
        conn->method = METHOD_UNKNOWN;
        conn->status_code = STATUS_METHOD_NOT_ALLOWED;

        strncpy(conn->uri, uri_str, URI_MAX_LEN - 1);
        conn->uri[URI_MAX_LEN - 1] = '\0';
        return RESULT_ERR;
    }

    strncpy(conn->uri, uri_str, URI_MAX_LEN - 1);
    conn->uri[URI_MAX_LEN - 1] = '\0';

    if (strstr(conn->uri, "..") != NULL) {
        fprintf(stderr, "Directory traversal attempt detected: %s\n", conn->uri);
        conn->status_code = STATUS_BAD_REQUEST;
        return RESULT_ERR;
    }

    printf("Parsed Request: Method=GET, URI=%s\n", conn->uri);
    return RESULT_OK;
}

enum result build_file_path(struct connection* conn) {
    const char* uri_target = conn->uri;

    int len = snprintf(conn->file_path, FILE_PATH_MAX_LEN, "%s%s/page.html", BASE_ROUTE_PATH, uri_target);

    if (len < 0 || len >= FILE_PATH_MAX_LEN) {
        fprintf(stderr, "Error formatting file path or path too long for URI: %s\n", conn->uri);
        conn->status_code = STATUS_INTERNAL_SERVER_ERROR;
        return RESULT_ERR;
    }

    return RESULT_OK;
}

const char* get_status_message(int status_code) {
    switch (status_code) {
        case STATUS_OK:
            return "OK";
        case STATUS_BAD_REQUEST:
            return "Bad Request";
        case STATUS_NOT_FOUND:
            return "Not Found";
        case STATUS_METHOD_NOT_ALLOWED:
            return "Method Not Allowed";
        case STATUS_INTERNAL_SERVER_ERROR:
            return "Internal Server Error";
        default:
            return "Unknown Status";
    }
}

enum result prepare_error_response(struct connection* conn) {
    conn->headers_sent = 0;
    conn->file_to_send = NULL;

    int len = snprintf(conn->response_header_buffer, RESPONSE_BUFFER_SIZE,
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-Length: %d\r\n"
                       "Connection: close\r\n\r\n"
                       "<html><body><h1>%d %s</h1></body></html>",
                       conn->status_code, get_status_message(conn->status_code),
                       (int)strlen("<html><body><h1>") + 3 + (int)strlen(get_status_message(conn->status_code)) + (int)strlen("</h1></body></html>"),
                       conn->status_code, get_status_message(conn->status_code));

    if (len < 0 || len >= RESPONSE_BUFFER_SIZE) {
        fprintf(stderr, "Error formatting error response buffer.\n");

        return RESULT_ERR;
    }
    conn->file_size = 0;
    conn->bytes_sent = 0;

    printf("Prepared error response: %d %s\n", conn->status_code, get_status_message(conn->status_code));
    return RESULT_OK;
}

enum result prepare_success_response(struct connection* conn) {
    struct stat file_stat;

    if (stat(conn->file_path, &file_stat) != 0) {
        perror("stat failed");
        conn->status_code = STATUS_NOT_FOUND;
        return prepare_error_response(conn);
    }

    if (!S_ISREG(file_stat.st_mode)) {
        fprintf(stderr, "Path is not a regular file: %s\n", conn->file_path);
        conn->status_code = STATUS_NOT_FOUND;
        return prepare_error_response(conn);
    }

    conn->file_size = file_stat.st_size;
    conn->file_to_send = fopen(conn->file_path, "rb");

    if (conn->file_to_send == NULL) {
        perror("fopen failed");
        conn->status_code = STATUS_INTERNAL_SERVER_ERROR;
        return prepare_error_response(conn);
    }

    conn->status_code = STATUS_OK;
    int len = snprintf(conn->response_header_buffer, RESPONSE_BUFFER_SIZE,
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: text/html\r\n"
                       "Content-Length: %ld\r\n"
                       "Connection: keep-alive\r\n\r\n",
                       conn->status_code, get_status_message(conn->status_code),
                       conn->file_size);

    if (len < 0 || len >= RESPONSE_BUFFER_SIZE) {
        fprintf(stderr, "Error formatting success response header.\n");
        fclose(conn->file_to_send);
        conn->file_to_send = NULL;
        conn->status_code = STATUS_INTERNAL_SERVER_ERROR;

        return RESULT_ERR;
    }
    conn->bytes_sent = 0;
    conn->headers_sent = 0;

    printf("Prepared success response for: %s (%ld bytes)\n", conn->file_path, conn->file_size);
    return RESULT_OK;
}

enum result send_response(struct connection* conn) {
    ssize_t bytes_written;

    if (!conn->headers_sent) {
        size_t header_len = strlen(conn->response_header_buffer);
        if (header_len > 0) {
            bytes_written = write(conn->client_fd, conn->response_header_buffer, header_len);

            if (bytes_written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    return RESULT_ERR_AGAIN;
                }
                perror("write headers failed");
                return RESULT_ERR;
            }
            if ((size_t)bytes_written < header_len) {
                fprintf(stderr, "Partial write on headers for fd %d\n", conn->client_fd);
                return RESULT_ERR;
            }

            conn->headers_sent = 1;
        } else {
            fprintf(stderr, "Warning: No headers to send for fd %d\n", conn->client_fd);
            conn->headers_sent = 1;
        }

        if (conn->file_to_send == NULL) {
            if (conn->status_code != STATUS_OK) {
                printf("Closing connection after sending error response for fd %d\n", conn->client_fd);
                return RESULT_ERR;
            } else {
                return RESULT_OK;
            }
        }
    }

    if (conn->headers_sent && conn->file_to_send != NULL) {
        size_t bytes_read = fread(conn->file_buffer, 1, FILE_BUFFER_SIZE, conn->file_to_send);

        if (bytes_read > 0) {
            ssize_t total_written_this_chunk = 0;
            while (total_written_this_chunk < bytes_read) {
                bytes_written = write(conn->client_fd,
                                      conn->file_buffer + total_written_this_chunk,
                                      bytes_read - total_written_this_chunk);

                if (bytes_written < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) {
                        if (fseek(conn->file_to_send, -total_written_this_chunk, SEEK_CUR) != 0) {
                            perror("fseek failed after partial write attempt");

                            return RESULT_ERR;
                        }

                        return RESULT_ERR_AGAIN;
                    }
                    perror("write file content failed");
                    return RESULT_ERR;
                }

                total_written_this_chunk += bytes_written;
                conn->bytes_sent += bytes_written;
            }

        } else {
            if (ferror(conn->file_to_send)) {
                perror("fread failed");
                return RESULT_ERR;
            }

            if (feof(conn->file_to_send)) {
                if (conn->bytes_sent == conn->file_size) {
                    printf("File sent completely for fd %d (%ld bytes).\n", conn->client_fd, conn->bytes_sent);
                    return RESULT_OK;
                } else {
                    fprintf(stderr, "EOF reached but bytes sent (%ld) != file size (%ld) for fd %d\n",
                            conn->bytes_sent, conn->file_size, conn->client_fd);
                    return RESULT_ERR;
                }
            }
        }
    }

    if (conn->headers_sent && conn->file_to_send != NULL && conn->bytes_sent < conn->file_size) {
        return RESULT_ERR_AGAIN;
    }

    if (conn->headers_sent && conn->file_to_send == NULL) {
        return RESULT_ERR;
    }

    if (conn->file_to_send != NULL) {
        return RESULT_ERR_AGAIN;
    }

    return RESULT_OK;
}

enum result handle_client_request(struct connection* conn) {
    ssize_t bytes_read;

    if (conn->request_len < REQUEST_BUFFER_SIZE - 1) {
        bytes_read = read(conn->client_fd,
                          conn->request_buffer + conn->request_len,
                          REQUEST_BUFFER_SIZE - 1 - conn->request_len);

        if (bytes_read < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (conn->request_len > 0) {
                } else {
                    return RESULT_ERR_AGAIN;
                }
            } else {
                perror("read failed");
                return RESULT_ERR;
            }
        } else if (bytes_read == 0) {
            printf("Connection closed by client (fd %d)\n", conn->client_fd);
            return RESULT_ERR;
        } else {
            conn->request_len += bytes_read;
            conn->request_buffer[conn->request_len] = '\0';

            if (strstr(conn->request_buffer, "\r\n\r\n") == NULL && conn->request_len < REQUEST_BUFFER_SIZE - 1) {
                printf("Potentially incomplete request received (%zu bytes), proceeding anyway...\n", conn->request_len);
            }
        }
    } else {
        fprintf(stderr, "Request buffer full for fd %d, closing connection.\n", conn->client_fd);
        conn->status_code = STATUS_BAD_REQUEST;
        prepare_error_response(conn);

        return RESULT_ERR;
    }

    enum result parse_res = parse_request(conn);
    if (parse_res != RESULT_OK) {
        prepare_error_response(conn);

    } else {
        if (conn->method == METHOD_GET) {
            enum result path_res = build_file_path(conn);
            if (path_res != RESULT_OK) {
                prepare_error_response(conn);

            } else {
                enum result prep_res = prepare_success_response(conn);
                if (prep_res != RESULT_OK) {
                }
            }
        } else {
            conn->status_code = STATUS_METHOD_NOT_ALLOWED;
            prepare_error_response(conn);
        }
    }

    if (conn->status_code == STATUS_OK || conn->file_to_send != NULL) {
        printf("Request handled for fd %d, proceeding to send response.\n", conn->client_fd);

        enum result send_res = send_response(conn);
        return send_res;

    } else {
        enum result send_res = send_response(conn);

        if (send_res == RESULT_OK) {
            printf("Error response sent successfully for fd %d, closing.\n", conn->client_fd);
            return RESULT_ERR;
        }
        return send_res;
    }
}

int main() {
    int listen_fd;
    int max_fd;
    fd_set readfds;
    fd_set writefds;

    initialize_connection_pool();

    if (setup_server_socket(&listen_fd) != RESULT_OK) {
        return EXIT_FAILURE;
    }

    printf("Server starting main loop...\n");

    while (1) {
        FD_ZERO(&readfds);
        FD_ZERO(&writefds);

        FD_SET(listen_fd, &readfds);
        max_fd = listen_fd;

        struct connection* conn = connection_active;
        while (conn != NULL) {
            if (conn->client_fd != -1) {
                FD_SET(conn->client_fd, &readfds);

                if (conn->headers_sent || conn->response_header_buffer[0] != '\0') {
                    FD_SET(conn->client_fd, &writefds);
                }

                if (conn->client_fd > max_fd) {
                    max_fd = conn->client_fd;
                }
            }
            conn = conn->next;
        }

        int activity = select(max_fd + 1, &readfds, &writefds, NULL, NULL);

        if (activity < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("select error");

            break;
        }

        if (FD_ISSET(listen_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_socket = accept(listen_fd, (struct sockaddr*)&client_addr, &addr_len);

            if (new_socket < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    perror("accept failed");
                }

            } else {
                int flags = fcntl(new_socket, F_GETFL, 0);
                if (flags == -1 || fcntl(new_socket, F_SETFL, flags | O_NONBLOCK) == -1) {
                    perror("fcntl O_NONBLOCK for new socket failed");
                    close(new_socket);

                } else {
                    struct connection* new_conn = get_free_connection();
                    if (new_conn == NULL) {
                        fprintf(stderr, "Max connections reached, rejecting new connection\n");

                        close(new_socket);
                    } else {
                        new_conn->client_fd = new_socket;

                        printf("New connection accepted, fd %d\n", new_socket);
                    }
                }
            }
        }

        struct connection* current_conn = connection_active;
        struct connection* next_conn = NULL;
        while (current_conn != NULL) {
            next_conn = current_conn->next;
            int should_close = 0;

            if (current_conn->client_fd == -1) {
                current_conn = next_conn;
                continue;
            }

            if (FD_ISSET(current_conn->client_fd, &readfds)) {
                if (!current_conn->headers_sent && current_conn->file_to_send == NULL) {
                    printf("Handling read event for fd %d\n", current_conn->client_fd);
                    enum result res = handle_client_request(current_conn);
                    if (res == RESULT_ERR) {
                        printf("Error handling request for fd %d, closing.\n", current_conn->client_fd);
                        should_close = 1;
                    }

                } else {
                    printf("Ignoring read event for fd %d while sending response.\n", current_conn->client_fd);
                }
            }

            if (!should_close && FD_ISSET(current_conn->client_fd, &writefds)) {
                if (current_conn->headers_sent || current_conn->response_header_buffer[0] != '\0') {
                    printf("Handling write event for fd %d\n", current_conn->client_fd);
                    enum result res = send_response(current_conn);
                    if (res == RESULT_ERR) {
                        printf("Error sending response for fd %d, closing.\n", current_conn->client_fd);
                        should_close = 1;
                    } else if (res == RESULT_OK) {
                        printf("Response sent completely for fd %d.\n", current_conn->client_fd);

                        should_close = 1;
                    }
                }
            }

            if (should_close) {
                close_connection(current_conn);
            }

            current_conn = next_conn;
        }
    }

    close(listen_fd);
    printf("Server shut down.\n");

    return EXIT_SUCCESS;
}