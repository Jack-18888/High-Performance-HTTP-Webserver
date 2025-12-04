#include "http-server.h"

#define MAX_REQUEST_SIZE 65536

int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return flags;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// Robust, line-based header search using string_view (Zero Copy)
std::string_view get_header_value(std::string_view headers, std::string_view name) {
    size_t pos = 0;
    size_t len = headers.length();

    while (pos < len) {
        size_t end = headers.find("\r\n", pos);
        if (end == std::string_view::npos)
            break;

        // We have a line from pos to end
        // Check if it starts with 'name' (case insensitive)
        if (end - pos >= name.length()) {
            bool match = true;
            for (size_t i = 0; i < name.length(); ++i) {
                if (std::tolower(headers[pos + i]) != std::tolower(name[i])) {
                    match = false;
                    break;
                }
            }

            if (match) {
                size_t colon_idx = pos + name.length();
                if (colon_idx < end && headers[colon_idx] == ':') {
                    // Found the header! Extract value
                    size_t val_start = colon_idx + 1;
                    // Skip leading whitespace
                    while (val_start < end && (headers[val_start] == ' ' || headers[val_start] == '\t')) {
                        val_start++;
                    }
                    return headers.substr(val_start, end - val_start);
                }
            }
        }

        pos = end + 2; // Move to next line (skip \r\n)
    }
    return ""; // Returns empty string_view
}

bool body_contains_chunked_terminator(std::string_view msg, size_t header_end) {
    // Check for 0\r\n\r\n inside the body part of the message.
    if (msg.size() < header_end)
        return false;

    // Case 1: The body is JUST 0\r\n\r\n (empty chunked body)
    if (msg.size() >= header_end + 5) {
        // Use string_view comparison
        if (msg.substr(header_end, 5) == "0\r\n\r\n")
            return true;
    }

    // Case 2: Standard chunked body ending with ...\r\n0\r\n\r\n
    return msg.find("\r\n0\r\n\r\n", header_end) != std::string_view::npos;
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

    // Set socket to non-blocking mode
    if (set_non_blocking(server_fd) == -1) {
        perror("set_non_blocking failed");
        exit(EXIT_FAILURE);
    }
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

void HttpServer::setup_epoll() {
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1");
        exit(EXIT_FAILURE);
    }

    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: server_fd");
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

void HttpServer::main_loop(struct sockaddr_in *address, socklen_t *addrlen) {
    std::unordered_map<int, ActiveConnData> client_connections;
    while (true) {
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

        for (int i = 0; i < num_events; i++) {
            int current_fd = events[i].data.fd;

            if (current_fd == server_fd) {
                // --- server_fd has new connection requests ---
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t addrlen = sizeof(client_addr);
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // No more new connections
                        } else {
                            perror("accept");
                            break;
                        }
                    }

                    // Add connection to epoll
                    set_non_blocking(client_fd);
                    struct epoll_event client_event;
                    client_event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    client_event.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_event);

                    // Initialize state
                    client_connections[client_fd] = ActiveConnData();
                }
            } else {
                // --- event is from a client_fd ---
                int client_fd = current_fd;

                ActiveConnData &active_data = client_connections[client_fd];

                ssize_t total_read = 0;
                char buffer[BUFFER_SIZE];
                bool close_conn = false;

                // Keep reading from client_fd until bytes_read indicates wait or close
                while (true) {
                    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);

                    if (bytes_read > 0) {
                        active_data.msg.append(buffer, bytes_read);
                        total_read += bytes_read;
                    } else if (bytes_read == 0) {
                        // Client closed connection
                        close_conn = true;
                        break;
                    } else {
                        // bytes_read < 0
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No more data to read now
                            break;
                        } else {
                            // Socket error
                            perror("recv error");
                            close_conn = true;
                            break;
                        }
                    }
                }

                if (close_conn) {
                    close(client_fd);
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    client_connections.erase(client_fd);
                    continue; // Continue to next event
                }

                // --- Parsing and State Check ---

                // State 1: Header UNKNOWN
                if (active_data.req_type == UNKNOWN) {
                    size_t header_pos = active_data.msg.find("\r\n\r\n");

                    if (header_pos != std::string::npos) {
                        active_data.header_end_pos = header_pos + 4;

                        // Create a string_view window over the existing msg buffer.
                        std::string_view headers = std::string_view(active_data.msg).substr(0, header_pos + 2);

                        std::string_view te = get_header_value(headers, "Transfer-Encoding");
                        std::string_view cl = get_header_value(headers, "Content-Length");

                        if (te.find("chunked") != std::string_view::npos) {
                            active_data.req_type = CHUNKED;
                        } else if (!cl.empty()) {
                            active_data.req_type = CONTENT_LENGTH;
                            active_data.content_length = std::stoul(std::string(cl));

                            // OPTIMIZATION 2: Reserve memory for the full expected body
                            // We know exactly how much space we need (Headers + Body).
                            // This ensures only 1 reallocation happens for the rest of the connection.
                            try {
                                size_t total_expected = active_data.header_end_pos + active_data.content_length;
                                // Simple sanity check to prevent malicious 100GB allocation attempts
                                if (total_expected < 100 * 1024 * 1024) {
                                    active_data.msg.reserve(total_expected);
                                }
                            } catch (...) {
                                // Allocation failed, we can try to proceed with default growth or close
                            }
                        } else {
                            active_data.req_type = NO_BODY;
                        }
                    } else {
                        // Header incomplete, wait for next event
                        struct epoll_event mod_event;
                        mod_event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                        mod_event.data.fd = client_fd;
                        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &mod_event);
                        continue;
                    }
                }

                bool request_is_complete = false;

                // State 2 & 3: Body Check
                if (active_data.req_type == CONTENT_LENGTH) {
                    if (active_data.msg.size() >= active_data.header_end_pos + active_data.content_length) {
                        request_is_complete = true;
                    }
                } else if (active_data.req_type == CHUNKED) {
                    // Pass the string view of the message to avoid copying
                    if (body_contains_chunked_terminator(active_data.msg, active_data.header_end_pos)) {
                        request_is_complete = true;
                    }
                } else if (active_data.req_type == NO_BODY) {
                    request_is_complete = true;
                }

                // --- Complete Request Handling (using writev for zero-copy) ---
                if (request_is_complete) {
                    std::string response = get_response(active_data.msg);
                    size_t sent = write(client_fd, response.c_str(), response.size());

                    if (sent == -1) {
                        perror("writev error");
                    }

                    // Cleanup
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
                    close(client_fd);
                    client_connections.erase(client_fd);
                } else {
                    // Re-arm for next data chunk
                    struct epoll_event mod_event;
                    mod_event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    mod_event.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client_fd, &mod_event);
                }
            }
        }
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

    setup_epoll();

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
