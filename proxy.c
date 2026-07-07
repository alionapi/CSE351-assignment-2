#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/wait.h>
#include <signal.h>

#define BUFFER_SIZE 8192
#define MAX_CACHE_ENTRIES 100
#define CACHE_DIR "./cache"

typedef struct {
    char url[512];
    char filename[256];
    time_t expire_time;
    int is_valid;
} CacheEntry;
CacheEntry cache_table[MAX_CACHE_ENTRIES];
int cache_count = 0;

void handle_sigchld(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
void init_cache() {
    struct stat st = { 0 };
    if (stat(CACHE_DIR, &st) == -1) {
        mkdir(CACHE_DIR, 0700);
    }
    memset(cache_table, 0, sizeof(cache_table));
}

void url_to_filename(const char* url, char* filename) {
    unsigned long hash = 5381;
    int c;
    const char* str = url;

    while ((c = *str++))
        hash = ((hash << 5) + hash) + c;

    sprintf(filename, "%s/%lu.cache", CACHE_DIR, hash);
}

int check_cache(const char* url, char** content, size_t* size) {
    char filename[256];
    url_to_filename(url, filename);

    for (int i = 0; i < cache_count; i++) {
        if (cache_table[i].is_valid && strcmp(cache_table[i].url, url) == 0) {
            if (cache_table[i].expire_time > time(NULL)) {
                FILE* fp = fopen(filename, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END);
                    *size = ftell(fp);
                    fseek(fp, 0, SEEK_SET);
                    *content = malloc(*size);
                    fread(*content, 1, *size, fp);
                    fclose(fp);
                    return 1;
                }
            }
            else {
                cache_table[i].is_valid = 0;
                unlink(filename);
            }
        }
    }
    return 0;
}

void save_to_cache(const char* url, const char* content, size_t size, int max_age) {
    char filename[256];
    url_to_filename(url, filename);

    FILE* fp = fopen(filename, "wb");
    if (fp) {
        fwrite(content, 1, size, fp);
        fclose(fp);

        int idx = cache_count % MAX_CACHE_ENTRIES;
        strncpy(cache_table[idx].url, url, sizeof(cache_table[idx].url) - 1);
        strncpy(cache_table[idx].filename, filename, sizeof(cache_table[idx].filename) - 1);
        cache_table[idx].expire_time = time(NULL) + max_age;
        cache_table[idx].is_valid = 1;

        if (cache_count < MAX_CACHE_ENTRIES)
            cache_count++;
    }
}

int parse_cache_control(const char* headers, int* is_cacheable, int* max_age) {
    *is_cacheable = 0;
    *max_age = 0;

    const char* cache_control = strstr(headers, "Cache-Control:");
    if (!cache_control)
        cache_control = strstr(headers, "cache-control:");

    if (cache_control) {
        const char* line_end = strstr(cache_control, "\r\n");
        if (!line_end) return 0;

        char line[512];
        int len = line_end - cache_control;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        strncpy(line, cache_control, len);
        line[len] = '\0';

        int has_private = (strstr(line, "private") != NULL);
        int has_public = (strstr(line, "public") != NULL);
        const char* max_age_str = strstr(line, "max-age=");

        if (has_private) {
            return 0;
        }

        if (has_public) {
            *is_cacheable = 1;
            if (max_age_str) {
                *max_age = atoi(max_age_str + 8);
                if (*max_age <= 0) {
                    *is_cacheable = 0;
                    return 0;
                }
            }
            else {
                *max_age = 3600;
            }
            return 1;
        }

        if (max_age_str && !has_private) {
            *is_cacheable = 1;
            *max_age = atoi(max_age_str + 8);
            if (*max_age <= 0) {
                *is_cacheable = 0;
                return 0;
            }
            return 1;
        }
    }

    return 0;
}

int parse_request(const char* request, char* method, char* url, char* version, char* host, int* port, char* path) {
    char req_copy[BUFFER_SIZE];
    strncpy(req_copy, request, BUFFER_SIZE - 1);
    req_copy[BUFFER_SIZE - 1] = '\0';

    char* line = strtok(req_copy, "\r\n");
    if (!line) return 0;

    if (sscanf(line, "%s %s %s", method, url, version) != 3)
        return 0;

    if (strcmp(method, "GET") != 0)
        return 0;

    if (strcmp(version, "HTTP/1.0") != 0)
        return 0;

    char* host_header = strstr(request, "Host:");
    if (!host_header)
        host_header = strstr(request, "host:");

    if (!host_header)
        return 0;

    char host_value[256];
    if (sscanf(host_header, "%*s %s", host_value) != 1)
        return 0;

    char* newline = strchr(host_value, '\r');
    if (newline) *newline = '\0';
    newline = strchr(host_value, '\n');
    if (newline) *newline = '\0';

    char* port_sep = strchr(host_value, ':');
    if (port_sep) {
        *port_sep = '\0';
        *port = atoi(port_sep + 1);
        strcpy(host, host_value);
    }
    else {
        *port = 80;
        strcpy(host, host_value);
    }

    if (strncmp(url, "http://", 7) == 0) {
        char* host_start = url + 7;
        char* path_start = strchr(host_start, '/');

        if (path_start) {
            char url_host_part[256];
            int len = path_start - host_start;
            if (len >= sizeof(url_host_part)) len = sizeof(url_host_part) - 1;
            strncpy(url_host_part, host_start, len);
            url_host_part[len] = '\0';

            char url_host_str[256];
            int url_port = 80;

            char* colon = strchr(url_host_part, ':');
            if (colon) {
                *colon = '\0';
                url_port = atoi(colon + 1);
                strcpy(url_host_str, url_host_part);
            }
            else {
                strcpy(url_host_str, url_host_part);
            }

            if (strcmp(url_host_str, host) != 0 || url_port != *port)
                return 0;

            strcpy(path, path_start);
        }
        else {
            char url_host_part[256];
            strcpy(url_host_part, host_start);

            char url_host_str[256];
            int url_port = 80;

            char* colon = strchr(url_host_part, ':');
            if (colon) {
                *colon = '\0';
                url_port = atoi(colon + 1);
                strcpy(url_host_str, url_host_part);
            }
            else {
                strcpy(url_host_str, url_host_part);
            }

            if (strcmp(url_host_str, host) != 0 || url_port != *port)
                return 0;

            strcpy(path, "/");
        }
    }
    else if (url[0] == '/') {
        strcpy(path, url);
    }
    else {
        return 0;
    }

    struct hostent* he = gethostbyname(host);
    if (!he)
        return 0;

    return 1;
}

void send_error(int client_fd) {
    const char* response = "HTTP/1.0 400 Bad Request\r\n\r\n";
    send(client_fd, response, strlen(response), 0);
}

void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    char method[16], url[512], version[16];
    char host[256], path[512];
    int port;

    memset(buffer, 0, BUFFER_SIZE);
    int bytes_read = recv(client_fd, buffer, BUFFER_SIZE - 1, 0);

    if (bytes_read <= 0) {
        close(client_fd);
        return;
    }

    if (!parse_request(buffer, method, url, version, host, &port, path)) {
        send_error(client_fd);
        close(client_fd);
        return;
    }

    char full_url[1024];
    snprintf(full_url, sizeof(full_url), "http://%s:%d%s", host, port, path);

    char* cached_content = NULL;
    size_t cached_size = 0;
    if (check_cache(full_url, &cached_content, &cached_size)) {
        send(client_fd, cached_content, cached_size, 0);
        free(cached_content);
        close(client_fd);
        return;
    }

    struct hostent* he = gethostbyname(host);
    if (!he) {
        send_error(client_fd);
        close(client_fd);
        return;
    }

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        close(client_fd);
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        close(client_fd);
        return;
    }

    char request[BUFFER_SIZE];
    snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
    send(server_fd, request, strlen(request), 0);

    char* response = malloc(BUFFER_SIZE * 100);
    size_t total_size = 0;
    size_t response_capacity = BUFFER_SIZE * 100;
    int n;
    int headers_parsed = 0;
    int is_cacheable = 0;
    int max_age = 0;

    while ((n = recv(server_fd, buffer, BUFFER_SIZE, 0)) > 0) {
        if (total_size + n > response_capacity) {
            response_capacity = response_capacity * 2;
            response = realloc(response, response_capacity);
        }
        memcpy(response + total_size, buffer, n);
        total_size += n;
        send(client_fd, buffer, n, 0);

        if (!headers_parsed && total_size >= 4) {
            for (size_t i = 0; i <= total_size - 4; i++) {
                if (response[i] == '\r' && response[i + 1] == '\n' &&
                    response[i + 2] == '\r' && response[i + 3] == '\n') {
                    char headers[8192];
                    size_t header_len = i + 4;
                    if (header_len < sizeof(headers)) {
                        memcpy(headers, response, header_len);
                        headers[header_len] = '\0';
                        parse_cache_control(headers, &is_cacheable, &max_age);
                    }
                    headers_parsed = 1;
                    break;
                }
            }
        }
    }

    if (is_cacheable && max_age > 0 && total_size > 0) {
        save_to_cache(full_url, response, total_size, max_age);
    }

    free(response);
    close(server_fd);
    close(client_fd);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        return 1;
    }

    int port = atoi(argv[1]);

    signal(SIGCHLD, handle_sigchld);

    init_cache();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return 1;
    }

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);

        if (client_fd < 0)
            continue;

        pid_t pid = fork();
        if (pid == 0) {
            close(server_fd);
            handle_client(client_fd);
            exit(0);
        }
        else {
            close(client_fd);
        }
    }

    close(server_fd);
    return 0;
}
