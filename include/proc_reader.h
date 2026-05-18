#pragma once
#include <fstream>
#include <sstream>
#include <string>

#include <pwd.h>
#include <sys/types.h>

#include "data_structures.h"

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

// Resolve a uid to a username via getpwuid()
std::string uid_to_username(uid_t uid) {
  struct passwd *pw = getpwuid(uid);
  return pw ? pw->pw_name : std::to_string(uid);
}

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