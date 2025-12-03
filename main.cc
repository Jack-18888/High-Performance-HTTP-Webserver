#include "HTTP-Server.h"
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono;

// --- Custom Endpoint Handlers for Testing ---

// 1. Fast Endpoint: Returns immediately (simulates a basic health check)
std::string handle_fast_check(const std::string &method, const std::string &path) {
    std::string content = "Status: OK";
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: " +
                           std::to_string(content.length()) +
                           "\r\n"
                           "Connection: close\r\n"
                           "\r\n" +
                           content;
    return response;
}

// 2. Slow Endpoint: Simulates a heavy, CPU-bound or I/O-bound task (e.g., database query)
std::string handle_slow_task(const std::string &method, const std::string &path) {
    const int delay_ms = 500; // 0.5 seconds delay

    // Simulate heavy computation or a database query
    std::this_thread::sleep_for(milliseconds(delay_ms));

    std::string content = "Task complete after " + std::to_string(delay_ms) + "ms delay.";
    std::string response = "HTTP/1.1 200 OK\r\n"
                           "Content-Type: text/plain\r\n"
                           "Content-Length: " +
                           std::to_string(content.length()) +
                           "\r\n"
                           "Connection: close\r\n"
                           "\r\n" +
                           content;
    return response;
}

int main() {
    const int server_port = 8080;
    HttpServer server(server_port);

    // 1. Add Fast Endpoint
    server.add_endpoint("GET", "/status", handle_fast_check);

    // 2. Add Slow Endpoint
    server.add_endpoint("GET", "/slow", handle_slow_task);

    // The server runs in the main thread, blocking execution here.
    std::cout << "Starting Blocking HTTP Server on port " << server_port << "." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;
    std::cout << "Test Endpoints:" << std::endl;
    std::cout << "  - Fast: GET http://127.0.0.1:8080/status" << std::endl;
    std::cout << "  - Slow: GET http://127.0.0.1:8080/slow" << std::endl;
    std::cout << "!!! NOTE: Server will only handle ONE request at a time. !!!" << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    try {
        server.start();
    } catch (const std::exception &e) {
        std::cerr << "Server exception: " << e.what() << std::endl;
    }

    return 0;
}
