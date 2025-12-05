#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "thread-pool.h"
#include <atomic>
#include <functional>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <unistd.h>
#include <vector>

// Constants
#define MAX_EVENTS 1000
#define BUFFER_SIZE 4096
#define MAX_REQUEST_SIZE 4096

// Type definitions
using RequestHandler = std::function<std::string(const std::string &, const std::string &)>;

// Simple structure to hold parsed HTTP request details
struct HTTPRequest {
    std::string method;
    std::string path;
    std::string version;
    std::string body;
};

// Structure to store route definitions.
struct Route {
    std::string method;
    std::string path;
    RequestHandler handler;

    Route(const std::string &m, const std::string &p, RequestHandler h) : method(m), path(p), handler(h) {}
};

class HttpServer {
  private:
    int port;
    int server_fd = -1;
    int epoll_fd = -1; // Only for listening socket now
    std::atomic<bool> running = false;
    std::vector<Route> routes;

    ThreadPool *thread_pool = nullptr;

    // epoll event storage
    struct epoll_event events[MAX_EVENTS];

    // Core socket setup methods
    void create_socket();
    void config_socket_opt(); // Only sets reuseaddr/reuseport for listening socket
    void bind_socket(struct sockaddr_in &address);
    void listen_socket();

    void setup_epoll(); // Only registers the listening socket

    // The main epoll loop (NON-BLOCKING I/O MULTIPLEXER)
    void main_loop(struct sockaddr_in *address, socklen_t *addrlen);

    // --- Worker Task Function (Executed by Thread Pool) ---
    // This function performs the entire blocking I/O cycle for one client.
    void handle_client_blocking(int client_fd);

    // Request/Response handling
    std::string get_response(const std::string &request);

    // Robust blocking read function (used by worker threads)
    std::string read_full_request_blocking(int client_fd);

  public:
    // Constructor: Takes the port number and number of threads
    HttpServer(int p, size_t num_threads);
    ~HttpServer();

    // Control methods
    void start();
    void stop();

    // Route configuration
    void add_endpoint(const std::string &method, const std::string &path, RequestHandler handler);
};

// Utility functions (defined in CPP)
HTTPRequest parse_http_request(const std::string &request);
std::string get_header_value(const std::string &headers, const std::string &name);
int set_non_blocking(int fd); // Will be modified to only be used on the listening socket

#endif // HTTP_SERVER_H
