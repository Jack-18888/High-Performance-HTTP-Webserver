#include "http-server.h"
#include <chrono>
#include <iostream>
#include <stdlib.h>
#include <thread>

using namespace std::chrono;

// --- Custom Endpoint Handlers for Testing ---

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

// Slow Endpoint: Simulates a 500ms blocking task
std::string handle_slow_task(const std::string &method, const std::string &path) {
    const int delay_ms = 500;
    // This sleep now happens inside a worker thread,
    // blocking only that thread, not the whole server.
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

std::string handle_post_echo(const std::string &method, const std::string &path) {
    std::string content = "POST received! Length unknown.";
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

// --- Main Program ---

int main() {
    const int server_port = 8080;

    // Determine optimal thread count: typically 2x Core Count for I/O-bound tasks
    size_t num_cores = std::thread::hardware_concurrency();
    size_t num_threads = num_cores > 0 ? num_cores * 2 : 8;

    HttpServer server(server_port, num_threads);

    server.add_endpoint("GET", "/status", handle_fast_check);
    server.add_endpoint("GET", "/slow", handle_slow_task);
    server.add_endpoint("POST", "/echo", handle_post_echo);

    std::cout << "Starting HIGH-PERFORMANCE HTTP Server (Hybrid Epoll + Thread Pool)." << std::endl;
    std::cout << "Architecture: Epoll handles connections; " << num_threads
              << " workers handle blocking I/O and processing." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;
    std::cout << "To test CONCURRENCY: Run 10 simultaneous requests to http://127.0.0.1:8080/slow" << std::endl;
    std::cout << "Expected Result: All requests should finish concurrently in ~500ms." << std::endl;
    std::cout << "--------------------------------------------------------" << std::endl;

    try {
        server.start();
    } catch (const std::exception &e) {
        std::cerr << "Server exception: " << e.what() << std::endl;
    }

    return 0;
}
