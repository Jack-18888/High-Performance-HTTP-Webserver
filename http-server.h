#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <functional>
#include <netinet/in.h>
#include <string>
#include <unistd.h>
#include <vector>

// Define a type for our request handler functions:
// They take the method and path, and return the full HTTP response string.
using RequestHandler = std::function<std::string(const std::string &, const std::string &)>;

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
    int server_fd = -1;
    bool running = false;
    std::vector<Route> routes;

    // Core socket setup methods
    void create_socket();
    void config_socket_opt();
    void bind_socket(struct sockaddr_in &address);
    void listen_socket();

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
