#include "http-server.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <chrono> // For std::this_thread::sleep_for
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

// --- Utility Functions ---

/**
 * @brief Sets a file descriptor to non-blocking mode.
 * NOTE: Only used for the listening socket in this TPC model.
 */
int set_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return flags;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/**
 * @brief Extracts a header value (case-insensitive search).
 */
std::string get_header_value(const std::string &headers, const std::string &name) {
    std::string search_name = name;
    std::transform(search_name.begin(), search_name.end(), search_name.begin(), ::tolower);
    search_name += ":";

    size_t pos = headers.find(search_name);
    if (pos == std::string::npos) {
        return "";
    }

    size_t start = pos + search_name.length();

    while (start < headers.length() && (headers[start] == ' ' || headers[start] == '\t')) {
        start++;
    }

    size_t end = headers.find("\r\n", start);
    if (end == std::string::npos) {
        return "";
    }

    return headers.substr(start, end - start);
}

// Parses the initial request line and body.
HTTPRequest parse_http_request(const std::string &request) {
    HTTPRequest req;
    size_t line_end = request.find("\r\n");
    if (line_end == std::string::npos)
        return req;

    std::string first_line = request.substr(0, line_end);

    // 1. Parse Method, Path, Version
    size_t method_end = first_line.find(' ');
    if (method_end != std::string::npos) {
        req.method = first_line.substr(0, method_end);

        size_t path_end = first_line.find(' ', method_end + 1);
        if (path_end != std::string::npos) {
            req.path = first_line.substr(method_end + 1, path_end - method_end - 1);
            req.version = first_line.substr(path_end + 1);
        }
    }

    // Parse Body
    size_t body_start = request.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        req.body = request.substr(body_start + 4);
    }

    return req;
}

// --- HttpServer Core Implementation ---

HttpServer::HttpServer(int p, size_t num_threads) : port(p) {
    // Initialize the Thread Pool
    thread_pool = new ThreadPool(num_threads);
}

HttpServer::~HttpServer() {
    stop();
    if (thread_pool) {
        delete thread_pool;
        thread_pool = nullptr;
    }
    if (epoll_fd != -1) {
        close(epoll_fd);
    }
}

void HttpServer::create_socket() {
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == -1) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }
}

void HttpServer::config_socket_opt() {
    int opt = 1;
    // SO_REUSEADDR allows the socket to be bound to a port that is already in use
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt failed");
        exit(EXIT_FAILURE);
    }
    // The main listening socket MUST be non-blocking for epoll accept loop
    if (set_non_blocking(server_fd) == -1) {
        perror("set_non_blocking failed for server_fd");
        exit(EXIT_FAILURE);
    }
}

void HttpServer::bind_socket(struct sockaddr_in &address) {
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }
}

void HttpServer::listen_socket() {
    if (listen(server_fd, 1024) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }
    std::cout << "Server listening on port " << port << std::endl;
}

void HttpServer::setup_epoll() {
    // Create the epoll instance
    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1) {
        perror("epoll_create1 failed");
        exit(EXIT_FAILURE);
    }

    // Register the listening socket with epoll
    struct epoll_event event;
    event.events = EPOLLIN; // Monitor for input events (new connections)
    event.data.fd = server_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event) == -1) {
        perror("epoll_ctl: server_fd failed");
        exit(EXIT_FAILURE);
    }
}

/**
 * @brief Reads the entire request from the socket using a blocking loop.
 * This function is run by a single worker thread.
 * NOTE: This is slightly simplified to read headers and body sequentially.
 */
std::string HttpServer::read_full_request_blocking(int client_fd) {
    std::string request_buffer;
    request_buffer.reserve(BUFFER_SIZE); // Reserve initial space
    char temp_buffer[BUFFER_SIZE];
    const char *end_delimiter = "\r\n\r\n";
    size_t delimiter_len = 4;
    size_t content_length = 0;
    bool headers_complete = false;

    // Phase 1: Read Headers until "\r\n\r\n" is found
    while (request_buffer.size() < MAX_REQUEST_SIZE) {
        // Use blocking recv(). The worker thread will block here,
        // but the other workers and main thread are free.
        ssize_t bytes_received = recv(client_fd, temp_buffer, BUFFER_SIZE, 0);

        if (bytes_received <= 0) {
            return ""; // Connection error or closed
        }

        request_buffer.append(temp_buffer, bytes_received);

        if (!headers_complete) {
            size_t header_pos = request_buffer.find(end_delimiter);
            if (header_pos != std::string::npos) {
                headers_complete = true;

                std::string headers_only = request_buffer.substr(0, header_pos);
                std::string cl_str = get_header_value(headers_only, "Content-Length");

                if (!cl_str.empty()) {
                    try {
                        content_length = std::stoul(cl_str);
                    } catch (...) {
                        // Ignore malformed CL, treat as 0 or close later
                    }
                }

                // If headers are complete and Content-Length is non-zero, proceed to body reading
                if (content_length > 0)
                    break;
            }
        }

        // If headers are complete but no body expected (GET/HEAD), break
        if (headers_complete && content_length == 0)
            break;
    }

    // Phase 2: Read Body (only if Content-Length was found and is non-zero)
    if (content_length > 0) {
        size_t header_end_pos = request_buffer.find(end_delimiter) + delimiter_len;
        size_t body_already_received = request_buffer.size() - header_end_pos;
        size_t remaining_to_read = content_length > body_already_received ? content_length - body_already_received : 0;

        while (remaining_to_read > 0) {
            ssize_t bytes_to_read = std::min((size_t)BUFFER_SIZE, remaining_to_read);
            ssize_t bytes_received = recv(client_fd, temp_buffer, bytes_to_read, 0);

            if (bytes_received <= 0) {
                return ""; // Connection closed before full body received
            }

            request_buffer.append(temp_buffer, bytes_received);
            remaining_to_read -= bytes_received;
        }
    }

    return request_buffer;
}

std::string HttpServer::get_response(const std::string &request) {
    // Find matching handler and generate response
    HTTPRequest http_request = parse_http_request(request);

    std::string default_response =
        "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\nContent-Length: 13\r\n\r\n404 Not Found";

    for (const auto &route : routes) {
        if (route.method == http_request.method && route.path == http_request.path) {
            return route.handler(http_request.method, http_request.path);
        }
    }

    return default_response;
}

/**
 * @brief The worker task: executes the full blocking I/O cycle for one client.
 * This is the task that gets delegated to the ThreadPool.
 */
void HttpServer::handle_client_blocking(int client_fd) {
    // 1. Read the full request (BLOCKING I/O - the worker thread is tied up here)
    std::string request = read_full_request_blocking(client_fd);

    if (!request.empty()) {
        // 2. Process request (BLOCKING CPU/DELAY)
        std::string response = get_response(request);

        // 3. Send response (BLOCKING I/O)
        ssize_t bytes_sent = send(client_fd, response.c_str(), response.size(), 0);
        if (bytes_sent == -1) {
            // Log error, but don't crash the server
            perror("send failed in worker");
        }
    } else {
        // Handle case where client disconnected before sending full request
    }

    // 4. Cleanup and close
    close(client_fd);
}

/**
 * @brief The main server loop uses epoll only to accept connections and dispatch tasks.
 * This loop MUST remain non-blocking.
 */
void HttpServer::main_loop(struct sockaddr_in *address, socklen_t *addrlen) {
    while (running) {
        // Wait for events (blocks until an event occurs or timeout)
        int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, 100); // 100ms timeout to check 'running' flag

        if (num_events < 0) {
            if (errno == EINTR)
                continue;
            perror("epoll_wait failed");
            break;
        }

        for (int i = 0; i < num_events; i++) {
            int current_fd = events[i].data.fd;

            if (current_fd == server_fd) {
                // Event on listening socket: new connection
                while (true) {
                    struct sockaddr_in client_addr;
                    socklen_t addrlen_temp = sizeof(client_addr);

                    // accept is non-blocking because server_fd is non-blocking
                    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addrlen_temp);

                    if (client_fd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            break; // No more new connections to accept
                        } else {
                            perror("accept error");
                            break;
                        }
                    }

                    // CRITICAL STEP: Delegate the full connection handling to the thread pool
                    try {
                        // The lambda captures 'this' to call the member function, and client_fd by value
                        thread_pool->enqueue(&HttpServer::handle_client_blocking, this, client_fd);
                    } catch (const std::exception &e) {
                        std::cerr << "Error enqueueing task: " << e.what() << std::endl;
                        close(client_fd); // Close socket if task fails to queue
                    }
                }
            }
        }
    }
}

void HttpServer::start() {
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);

    create_socket();
    config_socket_opt();
    bind_socket(address);
    listen_socket();

    setup_epoll();

    running = true;

    // The main_loop now runs the non-blocking I/O multiplexer
    main_loop(&address, &addrlen);
}

void HttpServer::stop() {
    running = false;
    if (server_fd != -1) {
        close(server_fd);
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, server_fd, nullptr);
        server_fd = -1;
    }
    if (thread_pool) {
        thread_pool->shutdown();
    }
}

void HttpServer::add_endpoint(const std::string &method, const std::string &path, RequestHandler handler) {
    routes.emplace_back(method, path, handler);
}
