# High-Performance C++ HTTP/1.1 Server

A highly concurrent, non-blocking HTTP server built from scratch using the **Reactor Pattern** on Linux. This project demonstrates the transition from a single-threaded event loop to a multi-threaded Master/Worker architecture capable of handling tens of thousands of concurrent connections with low latency.

## 🚀 Key Features

* **Asynchronous I/O:** Uses `epoll` with Edge-Triggered (`EPOLLET`) mode for maximum efficiency.
* **Multi-Threaded Architecture:** Scalable Master/Worker design utilizing a dedicated `epoll` instance per CPU core to eliminate cross-thread contention.
* **Non-blocking Networking:** Entirely non-blocking socket I/O to ensure a single slow connection doesn't stall the system.
* **Zero-Copy Principles:** Utilizes `std::string_view` for header parsing and `writev` (Scatter-Gather I/O) to minimize memory copying during responses.
* **Custom HTTP/1.1 Parser:** Supports `Content-Length` and `Chunked` transfer encodings.

## 🏗️ Architecture

The server follows the **Reactor-per-Thread** model:

1. **Master Thread:** Monitors the listening socket. When a new connection arrives, it performs an `accept()` and assigns the `client_fd` to a Worker thread using a **Round-Robin** load-balancing strategy.
2. **Worker Threads:** Each worker thread maintains its own `epoll` instance and an `eventfd`-signaled input queue.
3. **Handoff:** The Master pushes the FD to a worker's queue and signals via `eventfd`. The worker wakes up, moves the FD into its own interest set, and handles all subsequent I/O.

## 📊 Performance Benchmarks

Tested using `wrk` on a 16-core machine.

### High Concurrency (1000 Connections)

```bash
wrk -t10 -c1000 -d30s --latency http://localhost:8080

```

* **Throughput:** ~35,432 Requests/sec
* **Median Latency:** ~27.49 ms
* **Total Requests:** 1.08 Million in 30s

### Single Connection Latency

```bash
wrk -t1 -c1 -d10s --latency http://localhost:8080

```

* **Avg Latency:** 554.71 μs

## 🛠️ Build & Run

### Prerequisites

* Linux (Kernel 2.6.27+ for `eventfd`)
* C++17 Compatible Compiler (GCC/Clang)
* CMake

### Build

```bash
mkdir build && cd build
cmake ..
make

```

### Run

```bash
./bin/HybridHttpServer

```

## 🔍 Implementation Details

### Edge-Triggered I/O

The server uses `EPOLLET`. To prevent data loss, the `worker_loop` is designed to "drain" the socket by calling `recv` in a loop until `EAGAIN` or `EWOULDBLOCK` is returned.

### Thread Safety

* **Single-Producer/Single-Consumer:** Each worker has its own dedicated queue, reducing mutex contention during the handoff from the Master.
* **Atomic State:** The `running` flag uses `std::atomic` for graceful shutdowns across all threads.

## 🗺️ Roadmap

* [ ] Implement `SO_REUSEPORT` for kernel-level load balancing (Zero-Master architecture).
* [ ] Replace lock-based queues with SPSC Lock-Free Ring Buffers to reduce handoff latency.
* [ ] Add support for Keep-Alive timeouts to prune idle connections.
