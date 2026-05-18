#pragma once
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>

#include "data_structures.h"

// Escape a string for safe embedding in a JSON value
std::string json_escape(const std::string &s) {
  std::ostringstream o;
  for (auto c = s.cbegin(); c != s.cend(); c++) {
    switch (*c) {
    case '"':
      o << "\\\"";
      break;
    case '\\':
      o << "\\\\";
      break;
    case '\b':
      o << "\\b";
      break;
    case '\f':
      o << "\\f";
      break;
    case '\n':
      o << "\\n";
      break;
    case '\r':
      o << "\\r";
      break;
    case '\t':
      o << "\\t";
      break;
    default:
      if ('\x00' <= *c && *c <= '\x1f') {
        o << "\\u" << std::hex << std::setw(4) << std::setfill('0')
          << static_cast<int>(*c);
      } else {
        o << *c;
      }
    }
  }
  return o.str();
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