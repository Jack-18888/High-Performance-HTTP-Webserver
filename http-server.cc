#include "http-server.h"
#include <algorithm>
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/uio.h>

#define BUFFER_SIZE 1024
#define MAX_REQUEST_SIZE 65536

// const int buffer_size = 1024;

void pin_to_cpu_core(int core_id) {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(core_id, &mask);
    if (sched_setaffinity(0, sizeof(cpu_set_t), &mask) == -1) {
        perror("sched_setaffinity");
    }
}

void set_priority(int priority) {
    int result = setpriority(PRIO_PROCESS, 0, priority);

    if (result == -1) {
        perror("Failed to set priority"); // Print error message if setpriority fails
        // Check errno for specific error details
        if (errno == EACCES) {
            fprintf(stderr, "Permission denied: You likely need root privileges to set such a high priority.\n");
        } else if (errno == EINVAL) {
            fprintf(stderr, "Invalid priority value: The system might not support -20.\n");
        }
        exit(1); // Indicate an error
    } else {
        printf("Priority set to highest possible (-20 nice value).\n");
    }
}

// Helper function to reliably read an exact number of bytes
bool read_exact_bytes(int client_socket, char *buffer, size_t count) {
    size_t total_received = 0;
    while (total_received < count) {
        ssize_t bytes_received = recv(client_socket, buffer + total_received, count - total_received, 0);
        if (bytes_received <= 0) {
            // Error or connection closed unexpectedly
            return false;
        }
        total_received += bytes_received;
    }
    return true;
}

// Helper function to find a header value (case-insensitive)
std::string get_header_value(const std::string &headers, const std::string &name) {
    std::string search_name = name;
    std::transform(search_name.begin(), search_name.end(), search_name.begin(), ::tolower);

    // Look for the header (e.g., "content-length:")
    size_t pos = headers.find(search_name);
    if (pos == std::string::npos) {
        return "";
    }

    size_t start = pos + search_name.length();

    // Find the colon, case-insensitively, which should be right after the header name
    size_t colon_pos = headers.find(':', start - 1);
    if (colon_pos == std::string::npos) {
        return "";
    }

    // Find the start of the value (skip optional whitespace after ':')
    start = colon_pos + 1;
    while (start < headers.length() && (headers[start] == ' ' || headers[start] == '\t')) {
        start++;
    }

    // Find the end of the line (CRLF)
    size_t end = headers.find("\r\n", start);
    if (end == std::string::npos) {
        return "";
    }

    return headers.substr(start, end - start);
}

// Helper function to parse the HTTP request line
struct HTTPRequest parse_http_request(const std::string &request) {
    HTTPRequest req;
    size_t method_end = request.find(' ');
    size_t path_end = request.find(' ', method_end + 1);
    size_t version_end = request.find("\r\n", path_end + 1);

    req.method = request.substr(0, method_end);
    req.path = request.substr(method_end + 1, path_end - method_end - 1);
    req.version = request.substr(path_end + 1, version_end - path_end - 1);

    size_t body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        req.body = request.substr(body_start + 4);
    }

    return req;
}

// Function to read chunked request body
void read_request_body_chunked(int client_socket, std::string &request_buffer, const std::string &pre_read_body) {
    request_buffer.append(pre_read_body);

    while (true) {
        std::string size_line;
        char c;
        while (read_exact_bytes(client_socket, &c, 1) && (c != '\r')) {
            size_line += c;
        }
        if (!read_exact_bytes(client_socket, &c, 1) || c != '\n') {
            // Protocol violation or connection error
            return;
        }

        // Convert hex string to size_t
        size_t chunk_size = 0;
        try {
            chunk_size = std::stoul(size_line, nullptr, 16);
        } catch (...) {
            // Invalid chunk size format
            return;
        }

        if (chunk_size == 0) {
            // Read the final trailing \r\n after the 0-size chunk
            char crlf[2];
            read_exact_bytes(client_socket, crlf, 2);
            break;
        }

        // Read the chunk data
        char *chunk_data = new char[chunk_size];
        if (read_exact_bytes(client_socket, chunk_data, chunk_size)) {
            request_buffer.append(chunk_data, chunk_size);
        }
        delete[] chunk_data;

        // Read the trailing \r\n after the chunk data
        char crlf[2];
        read_exact_bytes(client_socket, crlf, 2);
    }
}

// Function to read request body based on Content-Length
void read_request_body(int client_socket, std::string &request_buffer, size_t content_length,
                       const std::string &pre_read_body) {

    // 1. Account for pre-read body data
    request_buffer.append(pre_read_body);

    size_t body_already_received = pre_read_body.length();

    // If we've already received the whole body (or more), we're done.
    if (body_already_received >= content_length) {
        return;
    }

    size_t remaining_to_read = content_length - body_already_received;

    // 2. Loop to read the remaining data
    while (remaining_to_read > 0) {
        // Buffer must be dynamic or large enough
        char temp_buffer[BUFFER_SIZE];

        // Read up to BUFFER_SIZE or the remaining amount
        size_t bytes_to_read = std::min((size_t)BUFFER_SIZE, remaining_to_read);

        ssize_t bytes_received = recv(client_socket, temp_buffer, bytes_to_read, 0);

        if (bytes_received <= 0) {
            // Connection closed before full Content-Length was received
            break;
        }

        request_buffer.append(temp_buffer, bytes_received);
        remaining_to_read -= bytes_received;
    }
}

void HttpServer::create_socket() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }
}

void HttpServer::config_socket_opt() {
    int opt = 1;

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt");
        exit(EXIT_FAILURE);
    }

    // if (setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) < 0) {
    //     perror("setsockopt SO_SNDBUF failed");
    // }
    //
    // if (setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) < 0) {
    //     perror("setsockopt SO_RCVBUF failed");
    // }
}

void HttpServer::bind_socket(struct sockaddr_in &address) {
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("bind failed");
        exit(EXIT_FAILURE);
    }
}

void HttpServer::listen_socket() {
    if (listen(server_fd, 1024) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }
}

std::string HttpServer::get_response(const std::string &request) {
    // Placeholder for processing the request and generating a response
    HTTPRequest http_request = parse_http_request(request);
    std::string response = "HTTP/1.1 404 Not Found\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: 13\r\n"
                           "\r\n"
                           "404 Not Found";
    for (const auto &route : routes) {
        if (route.method == http_request.method && route.path == http_request.path) {
            response = route.handler(http_request.method, http_request.path);
            break;
        }
    }

    return response;
}

std::string HttpServer::read_full_request(int client_socket) {
    char temp_buffer[BUFFER_SIZE];
    std::string request_buffer;

    const char *end_delimiter = "\r\n\r\n";
    size_t delimiter_len = 4;

    size_t old_size = 0; // Tracks size *before* new append

    // Read until delimeter found or max size reached
    while (request_buffer.size() < MAX_REQUEST_SIZE) {

        // Calculate the maximum number of bytes we can still read
        size_t bytes_to_read = std::min((size_t)BUFFER_SIZE, MAX_REQUEST_SIZE - request_buffer.size());

        ssize_t bytes_received = recv(client_socket, temp_buffer, bytes_to_read, 0);

        if (bytes_received <= 0) {
            return request_buffer;
        }

        old_size = request_buffer.size(); // Store size before append

        request_buffer.append(temp_buffer, bytes_received);

        if (request_buffer.size() >= delimiter_len) {
            size_t search_start_idx = 0;
            if (old_size >= delimiter_len - 1) {
                search_start_idx = old_size - (delimiter_len - 1);
            }

            size_t delimiter_pos = request_buffer.find(end_delimiter, search_start_idx);

            if (delimiter_pos != std::string::npos) {
                break;
            }
        }
    }

    if (request_buffer.size() >= MAX_REQUEST_SIZE) {
        return request_buffer;
    }

    size_t delimiter_pos = request_buffer.find(end_delimiter); // Safe now, since we know it exists
    std::string headers_only = request_buffer.substr(0, delimiter_pos);

    std::string pre_read_body_data = request_buffer.substr(delimiter_pos + delimiter_len);

    request_buffer.resize(delimiter_pos + delimiter_len);

    // Check Transfer-Encoding
    std::string te_value = get_header_value(headers_only, "transfer-encoding");
    if (te_value.find("chunked") != std::string::npos) {

        read_request_body_chunked(client_socket, request_buffer, pre_read_body_data);

    } else {
        // Check Content-Length
        std::string cl_str = get_header_value(headers_only, "content-length");
        if (!cl_str.empty()) {
            size_t content_length = 0;
            try {
                content_length = std::stoul(cl_str);
            } catch (...) {
                return request_buffer;
            }

            read_request_body(client_socket, request_buffer, content_length, pre_read_body_data);

        } else {
            // Neither Content-Length nor chunked: request is complete (no body expected)
        }
    }

    // Return the full request (Headers + Body)
    return request_buffer;
}

void HttpServer::main_loop(struct sockaddr_in *address, socklen_t *addrlen) {
    while (running) {
        int client_socket;
        if ((client_socket = accept(server_fd, (struct sockaddr *)address, addrlen)) < 0) {
            perror("accept");
            continue; // Continue the loop to try accepting the next connection
        }

        char buffer[1024] = {0};
        ssize_t bytes_received = recv(client_socket, buffer, 1024, 0);
        std::string request, response;
        if (bytes_received > 0) {
            request = std::string(buffer, bytes_received);
            response = get_response(request);
        }

        ssize_t bytes_sent = send(client_socket, response.c_str(), response.size(), 0);
        if (bytes_sent == -1) {
            perror("send failed");
        }

        close(client_socket);
    }
}

void HttpServer::start() {
    // pin_to_cpu_core(3); // Pin to CPU core 3 for performance

    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    create_socket();
    config_socket_opt();
    bind_socket(address);
    listen_socket();

    running = true;

    main_loop(&address, &addrlen);
}

void HttpServer::stop() {
    running = false;
    if (server_fd != -1) {
        close(server_fd);
        server_fd = -1;
    }
}

void HttpServer::add_endpoint(const std::string &method, const std::string &path, RequestHandler handler) {
    routes.emplace_back(method, path, handler);
}
