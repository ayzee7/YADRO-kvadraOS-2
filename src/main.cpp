#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <semaphore>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "data_structures.h"
#include "json_helpers.h"
#include "proc_reader.h"

namespace fs = std::filesystem;

// Global: collector thread writes, HTTP threads read
SystemData g_data;
std::mutex g_mutex;
std::binary_semaphore g_main_to_thread{0};
std::binary_semaphore g_thread_to_main{0};

// Ticks used by a process in the previous collection cycle
std::unordered_map<int, long> g_prev_proc_ticks;

std::vector<ProcessInfo> collect_processes(long mem_total_kb,
                                           long delta_total_ticks) {
  std::vector<ProcessInfo> result;
  std::unordered_map<int, long> curr_proc_ticks;
  long hz = sysconf(_SC_CLK_TCK);
  long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);

  for (auto &entry : fs::directory_iterator("/proc")) {
    // We only care about numeric directory names (PIDs)
    if (!entry.is_directory())
      continue;
    int pid = 0;
    std::string entry_name = entry.path().filename().string();
    for (char c : entry_name) {
      if (!std::isdigit(c)) {
        pid = -1;
        break;
      }
    }
    if (pid == -1)
      continue;
    pid = std::stoi(entry_name);

    ProcStat ps;
    if (!read_proc_stat(pid, ps))
      continue;

    long ticks = ps.utime + ps.stime;
    curr_proc_ticks[pid] = ticks;
    long time = ticks * 100 / hz;

    float cpu_pct = 0.0f;
    auto it = g_prev_proc_ticks.find(pid);

    if (it != g_prev_proc_ticks.end() && delta_total_ticks > 0) {
      long delta_proc = ticks - it->second;
      cpu_pct = 100.0f * delta_proc * n_cpus / delta_total_ticks;
    }

    read_proc_status(pid, ps);
    float mem_pct = mem_total_kb > 0 ? 100.0f * ps.rss / mem_total_kb : 0.0f;

    ProcessInfo p;
    p.pid = pid;
    p.user = uid_to_username(ps.uid);
    p.priority = ps.priority;
    p.nice = ps.nice;
    p.state = ps.state;
    p.virt = ps.vmsize;
    p.res = ps.rss;
    p.cpu = cpu_pct;
    p.mem = mem_pct;
    p.time = time;
    p.command = ps.command;
    result.push_back(p);
  }

  // accessed only from collector_thread
  // can use without mutex in this case
  g_prev_proc_ticks = std::move(curr_proc_ticks);

  return result;
}

// Collect everything into a SystemData snapshot
SystemData collect(const CpuSample &prev_cpu, const CpuSample &curr_cpu) {
  SystemData data{};

  data.cpu_percent = compute_cpu_percent(prev_cpu, curr_cpu);

  read_meminfo(data);
  read_proc_uptime(data);

  long delta_total = curr_cpu.total - prev_cpu.total;
  data.processes = collect_processes(data.mem_total_kb, delta_total);

  return data;
}

// Build an HTTP response string
std::string http_response(int status, const std::string &content_type,
                          const std::string &body) {
  std::string status_text;
  if (status == 200)
    status_text = "OK";
  else if (status == 404)
    status_text = "Not Found";
  else
    status_text = "Internal Server Error";
  std::ostringstream resp;
  resp << "HTTP/1.1 " << status << " " << status_text << "\r\n"
       << "Content-Type: " << content_type << "\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Connection: close\r\n"
       << "\r\n"
       << body;
  return resp.str();
}

// Handle one HTTP client connection
void handle_client(int client_fd) {
  try {
    // Read the request (we only need the first line)
    // no real need to read in for loop on localhost
    std::array<char, 4096> req_buf;
    ssize_t n = recv(client_fd, req_buf.data(), req_buf.size() - 1, 0);
    if (n <= 0) {
      close(client_fd);
      return;
    }
    req_buf[n] = '\0';

    std::string request(req_buf.data());
    std::string response;

    auto process_request = [&](const std::string &file,
                               const std::string &content_type) -> void {
      std::string rd_file = read_file(file);
      if (rd_file.empty()) {
        response = http_response(404, "text/plain", file + " not found");
      } else {
        response = http_response(200, content_type, rd_file);
      }
    };

    if (request.find("GET /api/stats") != std::string::npos) {
      // Return current metrics as JSON
      std::lock_guard<std::mutex> lock(g_mutex);
      std::string body = build_json(g_data);
      response = http_response(200, "application/json", body);
    } else if (request.find("GET /style.css") != std::string::npos) {
      process_request("frontend/style.css", "text/css");
    } else if (request.find("GET /script.js") != std::string::npos) {
      process_request("frontend/script.js", "application/javascript");
    } else if (request.find("GET /") != std::string::npos) {
      process_request("frontend/index.html", "text/html");
    } else {
      response = http_response(404, "text/plain", "Not found");
    }

    size_t sent = 0;
    while (sent < response.size()) {
      ssize_t n =
          send(client_fd, response.data() + sent, response.size() - sent, 0);
      if (n <= 0)
        break;
      sent += n;
    }

    close(client_fd);
  } catch (const std::exception &e) {
    std::cerr << "handle_client error: " << e.what() << '\n';
    close(client_fd);
  }
}

// Background thread: refresh metrics every 2 seconds
void collector_thread() {
  g_main_to_thread.acquire();
  CpuSample prev = read_cpu_sample();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    CpuSample curr = read_cpu_sample();
    SystemData fresh = collect(prev, curr);
    prev = curr;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_data = fresh;

    g_thread_to_main.release();
  }
}

// Main: start collector, then listen for HTTP connections
int main() {
  const int PORT = 8080;

  // Start the data collection thread
  std::thread(collector_thread).detach();
  // main never returns, so detached threads can be used for this scope

  // Give collector_thread a signal to start working
  g_main_to_thread.release();
  // And wait until it collects initial data samples
  g_thread_to_main.acquire();

  // Create the TCP socket
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    perror("socket fail");
    return 1;
  }

  // Allow quick restart without "address already in use"
  int opt = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (bind(server_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
    perror("bind fail");
    close(server_fd);
    return 1;
  }
  if (listen(server_fd, 16) < 0) {
    perror("listen");
    close(server_fd);
    return 1;
  }

  std::cout << "mytop running on http://localhost:" << PORT << "\n";

  // Accept connections in a simple loop; spawn a thread per client
  while (true) {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(
        server_fd, reinterpret_cast<sockaddr *>(&client_addr), &client_len);
    if (client_fd < 0) {
      perror("accept fail");
      continue;
    }

    // Handle each client in its own thread
    std::thread(handle_client, client_fd).detach();
  }

  return 0;
}