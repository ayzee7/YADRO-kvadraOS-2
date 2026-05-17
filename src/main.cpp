#include <arpa/inet.h>
#include <netinet/in.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// Data structures
struct ProcessInfo {
  int pid;
  std::string user;
  int priority;
  int nice;
  long virt; // virtual memory size
  long res;  // residual memory size
  char state;
  float cpu; // % since last sample
  float mem; // % of total RAM
  long time; // cpu time
  std::string command;
};

struct SystemData {
  float cpu_percent; // total CPU usage %
  long mem_total_kb;
  long mem_used_kb;
  long swap_total_kb;
  long swap_used_kb;
  long uptime;
  std::vector<ProcessInfo> processes;
};

// Global: collector thread writes, HTTP threads read
SystemData g_data;
std::mutex g_mutex;

// CPU usage - requires two samples to compute a delta
struct CpuSample {
  long idle = 0;
  long total = 0;
};

CpuSample read_cpu_sample() {
  std::ifstream f("/proc/stat");
  std::string label;
  long user, nice, system, idle, iowait, irq, softirq, steal, guest, guest_nice;
  f >> label >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
      steal >> guest >> guest_nice;

  CpuSample s;
  s.idle = idle + iowait;
  s.total = user + nice + system + idle + iowait + irq + softirq + steal +
            guest + guest_nice;
  return s;
}

// Returns CPU usage % between two samples
float compute_cpu_percent(const CpuSample &prev, const CpuSample &curr) {
  long delta_total = curr.total - prev.total;
  long delta_idle = curr.idle - prev.idle;
  if (delta_total == 0)
    return 0.0f;
  return 100.0f * (delta_total - delta_idle) / delta_total;
}

// Memory - /proc/meminfo
void read_meminfo(SystemData &data) {
  std::ifstream f("/proc/meminfo");
  std::string key;
  long value;
  long mem_available = 0;
  long swap_free = 0;

  while (f >> key >> value) {
    f.ignore(256, '\n'); // skip " kB\n"
    if (key == "MemTotal:")
      data.mem_total_kb = value;
    else if (key == "MemAvailable:")
      mem_available = value;
    else if (key == "SwapTotal:")
      data.swap_total_kb = value;
    else if (key == "SwapFree:")
      swap_free = value;
  }

  data.mem_used_kb = data.mem_total_kb - mem_available;
  data.swap_used_kb = data.swap_total_kb - swap_free;
}

// Process list
// CPU % is computed from the tick delta since the last sample,
// same diff approach as the total CPU calculation above.

// Ticks used by a process in the previous collection cycle
static std::unordered_map<int, long> g_prev_proc_ticks;

// Resolve a uid to a username via getpwuid()
std::string uid_to_username(uid_t uid) {
  struct passwd *pw = getpwuid(uid);
  return pw ? pw->pw_name : std::to_string(uid);
}

struct ProcStat {
  int pid = 0;
  std::string command;
  char state = '?';
  long utime = 0; // user-mode ticks
  long stime = 0; // kernel-mode ticks
  int priority = 0;
  int nice = 0;
  long vmsize = 0; // virtual memory size
  long rss = 0;    // residue memory size
  uid_t uid = 0;
};

// Parse /proc/[pid]/stat
bool read_proc_stat(int pid, ProcStat &out) {
  std::string path = "/proc/" + std::to_string(pid) + "/stat";
  std::ifstream f(path);
  if (!f)
    return false;

  std::string line;
  std::getline(f, line);

  // Find the command between the first '(' and last ')'
  size_t open = line.find('(');
  size_t close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos)
    return false;

  out.pid = std::stoi(line.substr(0, open));
  out.command = line.substr(open + 1, close - open - 1);

  // Everything after ')' is space-separated fields starting with state
  std::istringstream rest(line.substr(close + 2));
  long ppid, pgrp, session, tty, tpgid, flags;
  long minflt, cminflt, majflt, cmajflt;
  long cutime, cstime, num_threads, itrealvalue;
  rest >> out.state >> ppid >> pgrp >> session >> tty >> tpgid >> flags >>
      minflt >> cminflt >> majflt >> cmajflt >> out.utime >> out.stime >>
      cutime >> cstime >> out.priority >> out.nice >> num_threads >>
      itrealvalue;

  return true;
}

// Read from /proc/[pid]/status (VmSize, VmRSS, Uid)
void read_proc_status(int pid, ProcStat &out) {
  std::string path = "/proc/" + std::to_string(pid) + "/status";
  std::ifstream f(path);
  std::string key;
  while (f >> key) {
    if (key == "VmSize:") {
      f >> out.vmsize;
    }
    if (key == "VmRSS:") {
      f >> out.rss;
    }
    if (key == "Uid:") {
      f >> out.uid;
    }
    f.ignore(256, '\n');
  }
}

// Read from /proc/uptime
void read_proc_uptime(SystemData &data) {
  std::ifstream f("/proc/uptime");
  double value;
  f >> value;
  data.uptime = static_cast<long>(value);
}

std::vector<ProcessInfo> collect_processes(long mem_total_kb,
                                           long delta_total_ticks) {
  std::vector<ProcessInfo> result;
  std::unordered_map<int, long> curr_proc_ticks;
  long hz = sysconf(_SC_CLK_TCK);

  fs::directory_iterator proc_dir("/proc");

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
      long n_cpus = sysconf(_SC_NPROCESSORS_ONLN);
      cpu_pct = 100.0f * delta_proc * n_cpus / delta_total_ticks;
    }

    read_proc_status(pid, ps);
    long rss_kb = ps.rss;
    float mem_pct = mem_total_kb > 0 ? 100.0f * rss_kb / mem_total_kb : 0.0f;

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

// Escape a string for safe embedding in a JSON value
std::string json_escape(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// Build the JSON response string from current SystemData
std::string build_json(const SystemData &data) {
  std::ostringstream json;
  json << "{";
  json << "\"cpu_percent\":" << data.cpu_percent << ",";
  json << "\"mem_total_kb\":" << data.mem_total_kb << ",";
  json << "\"mem_used_kb\":" << data.mem_used_kb << ",";
  json << "\"swap_total_kb\":" << data.swap_total_kb << ",";
  json << "\"swap_used_kb\":" << data.swap_used_kb << ",";
  json << "\"uptime\":" << data.uptime << ",";
  json << "\"processes\": [";
  for (size_t i = 0; i < data.processes.size(); ++i) {
    const auto &p = data.processes[i];
    json << "{" << "\"pid\":" << p.pid << "," << "\"user\":\""
         << json_escape(p.user) << "\"," << "\"priority\":" << p.priority << ","
         << "\"nice\":" << p.nice << "," << "\"virt\":" << p.virt << ","
         << "\"res\":" << p.res << "," << "\"state\":\"" << p.state << "\","
         << "\"cpu\":" << p.cpu << "," << "\"mem\":" << p.mem << ","
         << "\"time\":" << p.time << "," << "\"command\":\""
         << json_escape(p.command) << "\"" << "}";
    if (i + 1 < data.processes.size())
      json << ",";
    json << "";
  }
  json << "]}";
  return json.str();
}

std::string read_file(const std::string &path) {
  std::ifstream f(path);
  if (!f)
    return "";

  std::ostringstream buf;
  buf << f.rdbuf();

  return buf.str();
}

// Build an HTTP response string
std::string http_response(int status, const std::string &content_type,
                          const std::string &body) {
  std::string status_text = (status == 200) ? "OK" : "Not Found";
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
  // Read the request (we only need the first line)
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
      response = http_response(404, "text/plain", file + "not found");
    } else {
      response = http_response(200, content_type, rd_file);
    }
  };

  if (request.find("GET /api/stats") != std::string::npos) {
    // Return current metrics as JSON
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string body = build_json(g_data);
    response = http_response(200, "application/json", body);
  }
  else if (request.find("GET /style.css") != std::string::npos) {
    process_request("frontend/style.css", "text/css");
  }
  else if (request.find("GET /script.js") != std::string::npos) {
    process_request("frontend/script.js", "application/javascript");
  }
  else if (request.find("GET /") != std::string::npos) {
    process_request("frontend/index.html", "text/html");
  }
  else {
    response = http_response(404, "text/plain", "Not found");
  }

  send(client_fd, response.data(), response.size(), 0);
  close(client_fd);
}

// Background thread: refresh metrics every 2 seconds
void collector_thread() {
  CpuSample prev = read_cpu_sample();

  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(2));

    CpuSample curr = read_cpu_sample();
    SystemData fresh = collect(prev, curr);
    prev = curr;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_data = fresh;
  }
}

// Main: start collector, then listen for HTTP connections
int main() {
  const int PORT = 8080;

  // Start the data collection thread
  std::thread(collector_thread).detach();

  // Give the collector a moment to fill g_data before the first request
  std::this_thread::sleep_for(std::chrono::seconds(1));

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