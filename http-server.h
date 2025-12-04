#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <algorithm>
#include <arpa/inet.h>
#include <asm-generic/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <functional>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>

#define BUFFER_SIZE 1024
#define MAX_EVENTS 128

// Define a type for our request handler functions:
// They take the method and path, and return the full HTTP response string.
using RequestHandler = std::function<std::string(const std::string &, const std::string &)>;

// Enum to represent different HTTP request body types.
enum HTTP_REQ_TYPE { UNKNOWN, NO_BODY, CHUNKED, CONTENT_LENGTH };

struct ActiveConnData {
    std::string msg;
    size_t header_end_pos;
    HTTP_REQ_TYPE req_type;
    size_t content_length;

    ActiveConnData() : header_end_pos(0), req_type(UNKNOWN), content_length(0) { msg.reserve(BUFFER_SIZE); }
};

// Simple structure to hold parsed HTTP request details (Method, Path, Version, Body).
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
    int server_fd = -1, epoll_fd = -1;
    bool running = false;
    struct epoll_event events[MAX_EVENTS];
    std::vector<Route> routes;

    // Core socket setup methods
    void create_socket();
    void config_socket_opt();
    void bind_socket(struct sockaddr_in &address);
    void listen_socket();

    // Epoll setup methods
    void setup_epoll();
    void handle_new_connection();
    void handle_client_data(int client_socket);

    // The main loop (currently blocking and single-threaded)
    void main_loop(struct sockaddr_in *address, socklen_t *addrlen);

    // Read full http request from client
    std::string read_full_request(int client_socket);

    // Request/Response handling
    std::string get_response(const std::string &request);

  public:
    // Constructor: Takes the port number
    HttpServer(int p) : port(p) {}
    ~HttpServer() { stop(); }

    // Control methods
    void start();
    void stop();

    // Route configuration
    void add_endpoint(const std::string &method, const std::string &path, RequestHandler handler);
};

// Utility function to parse the basic HTTP request line
HTTPRequest parse_http_request(const std::string &request);

#endif // HTTP_SERVER_H
