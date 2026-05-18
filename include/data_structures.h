#pragma once
#include <string>
#include <vector>

#include <sys/types.h>

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

// CPU usage - requires two samples to compute a delta
struct CpuSample {
  long idle = 0;
  long total = 0;
};

// /proc/stat data
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