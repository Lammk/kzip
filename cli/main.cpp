#include "../include/kzip/kzip.h"
#include "../src/simd_matvec.h"
#include "../src/profile.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static void print_usage() {
  std::cout <<
    "kzip " KZIP_VERSION_STRING " - KZ compression format (method 0x4B5A)\n"
    "Architecture: scan&sort -> solid 8-32MB + manifest -> prefilter(7.92)\n"
    "           -> Lite/Ultra -> RNN SIMD -> rANS -> ZIP 0x4B5A\n"
    "\n"
    "Usage:\n"
    "  kzip a <archive.kzip> [files/dirs...] [-t N|--threads N] [--lite|--ultra|--auto] [-l<1..9>]\n"
    "  kzip x <archive.kzip> [-o <outdir>] [-t N]\n"
    "  kzip t <archive.kzip>            (check integrity)\n"
    "  kzip l <archive.kzip>            (list logical files)\n"
    "\n"
    "Examples:\n"
    "  kzip a docs.kzip ./docs -t 4 --ultra\n"
    "  kzip a docs.kzip ./docs --lite\n"
    "  kzip x docs.kzip -o ./out\n"
    "  kzip t docs.kzip\n";
}

static std::string fmt_size(double b) {
  const char* u[] = {"B","KB","MB","GB"};
  int i = 0;
  while (b >= 1024 && i < 3) { b /= 1024; i++; }
  char buf[64]; snprintf(buf, sizeof(buf), "%.2f %s", b, u[i]);
  return buf;
}

static int cmd_add(const std::string& arc, std::vector<std::string> inputs,
                   int threads, int level, kzip::ProfileOpt prof) {
  if (inputs.empty()) { std::cerr << "missing input files\n"; return 1; }
  kzip::Profile eff = kzip::Profile::Lite;
  if (prof == kzip::ProfileOpt::Ultra) eff = kzip::Profile::Ultra;
  else if (prof == kzip::ProfileOpt::Lite) eff = kzip::Profile::Lite;
  else eff = kzip::auto_profile();
  int H, E;
  kzip::profile_hidden_embed(eff, H, E);
  int eff_threads = kzip::effective_threads(threads);
  std::cout << "Backend: " << kzip::simd::matvec_backend_name()
            << " (lite:" << kzip::simd::matvec_backend_for_profile(1)
            << " ultra:" << kzip::simd::matvec_backend_for_profile(2) << ")"
            << " | profile: " << (eff == kzip::Profile::Ultra ? "ultra" : "lite")
            << " H=" << H << " E=" << E
            << " (~" << kzip::param_count_for_profile(eff == kzip::Profile::Ultra ? 2 : 1) << " params)"
            << " | threads: " << eff_threads
            << " (requested " << threads << ")"
            << " | cache ~" << (eff == kzip::Profile::Ultra ? "600KB" : "80KB")
            << "\n";
  auto t0 = std::chrono::steady_clock::now();
  uint64_t total_in = 0;
  for (auto& s : inputs) {
    std::error_code ec;
    ec.clear();
    if (fs::is_directory(s, ec) && !ec) {
      ec.clear();
      auto opts = fs::directory_options::skip_permission_denied;
      for (auto it = fs::recursive_directory_iterator(s, opts, ec);
           it != fs::recursive_directory_iterator(); ) {
        if (ec) { ec.clear(); it.increment(ec); ec.clear(); continue; }
        std::error_code ec2;
        bool d = it->is_directory(ec2);
        if (!ec2 && !d) total_in += it->file_size(ec2);
        it.increment(ec);
        if (ec) ec.clear();
      }
    } else {
      std::error_code ec3;
      uint64_t z = fs::file_size(s, ec3);
      if (!ec3) total_in += z;
    }
  }
  kzip::CompressOptions opt;
  opt.level = level; opt.threads = threads; opt.profile = prof;
  bool ok = kzip::compress_archive_ex(inputs, arc, opt,
    [&](uint64_t done, uint64_t total) {
      auto now = std::chrono::steady_clock::now();
      double el = std::chrono::duration<double>(now - t0).count();
      double speed = el > 0 ? (double)total_in * ((double)done / (total ? total : 1)) / 1024.0 / el : 0;
      std::cout << "\r[" << done << "/" << total << "] "
                << fmt_size((double)total_in * done / (total ? total : 1))
                << " | " << (int)speed << " KB/s" << std::flush;
    });
  std::cout << "\n";
  if (!ok) { std::cerr << "compression failed\n"; return 1; }
  auto t1 = std::chrono::steady_clock::now();
  double el = std::chrono::duration<double>(t1 - t0).count();
  uint64_t arc_size = 0;
  try { arc_size = fs::file_size(arc); } catch (...) {}
  double ratio = total_in ? 100.0 * (double)arc_size / (double)total_in : 0;
  std::cout << "OK: " << fmt_size((double)total_in) << " -> "
            << fmt_size((double)arc_size) << " (" << ratio << "%) in "
            << el << "s (" << (el > 0 ? (double)total_in / 1024.0 / el : 0) << " KB/s)\n";
  return 0;
}

static int cmd_extract(const std::string& arc, const std::string& out, int threads) {
  auto t0 = std::chrono::steady_clock::now();
  bool ok = kzip::decompress_archive(arc, out.empty() ? "." : out, threads,
    [&](uint64_t d, uint64_t t) {
      std::cout << "\r[" << d << "/" << t << "]" << std::flush;
    });
  std::cout << "\n";
  if (!ok) { std::cerr << "decompression failed\n"; return 1; }
  double el = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - t0).count();
  std::cout << "OK in " << el << "s\n";
  return 0;
}

static int cmd_test(const std::string& arc) {
  bool ok = kzip::test_archive(arc);
  std::cout << (ok ? "OK: integrity 100%\n" : "FAIL: data error\n");
  return ok ? 0 : 1;
}

static int cmd_list(const std::string& arc) {
  std::vector<kzip::ArchiveEntry> es;
  if (!kzip::list_archive(arc, es)) { std::cerr << "cannot read archive\n"; return 1; }
  printf("%-44s %12s %10s %s\n", "Name", "Uncomp", "Method", "Info");
  for (auto& e : es) {
    char m[24]; snprintf(m, sizeof(m), "0x%04X%s", e.method,
      e.method == 0x4B5A ? "(KZ)" : e.method == 0 ? "(store)" : "");
    std::string info;
    if (!e.solid_group.empty()) {
      info = "solid:" + e.solid_group + (e.profile == 2 ? " ultra" : " lite");
    } else if (e.is_dir) info = "<dir>";
    else info = "";
    printf("%-44s %12llu %10s %s\n", e.name.c_str(),
      (unsigned long long)e.uncomp_size, m, info.c_str());
  }
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 3) { print_usage(); return 1; }
  std::string cmd = argv[1];
  if (cmd == "a") {
    std::string arc = argv[2];
    std::vector<std::string> inputs;
    int threads = 0, level = 1;
    kzip::ProfileOpt prof = kzip::ProfileOpt::Auto;
    for (int i = 3; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--ultra") prof = kzip::ProfileOpt::Ultra;
      else if (a == "--lite") prof = kzip::ProfileOpt::Lite;
      else if (a == "--auto") prof = kzip::ProfileOpt::Auto;
      else if (a == "--threads" && i + 1 < argc) threads = std::stoi(argv[++i]);
      else if (a.rfind("--threads=", 0) == 0) threads = std::stoi(a.substr(10));
      else if (a == "-t" && i + 1 < argc) threads = std::stoi(argv[++i]);
      else if (a.rfind("-t", 0) == 0 && a.size() > 2) threads = std::stoi(a.substr(2));
      else if (a == "-l" && i + 1 < argc) level = std::stoi(argv[++i]);
      else if (a.rfind("-l", 0) == 0 && a.size() > 2) level = std::stoi(a.substr(2));
      else if (a.rfind("--level=", 0) == 0) level = std::stoi(a.substr(8));
      else inputs.push_back(a);
    }
    return cmd_add(arc, inputs, threads, level, prof);
  } else if (cmd == "x") {
    std::string arc = argv[2], out;
    int threads = 0;
    for (int i = 3; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "-o" && i + 1 < argc) out = argv[++i];
      else if (a.rfind("-o", 0) == 0 && a.size() > 2) out = a.substr(2);
      else if (a.rfind("--output=", 0) == 0) out = a.substr(9);
      else if (a == "-t" && i + 1 < argc) threads = std::stoi(argv[++i]);
      else if (a.rfind("-t", 0) == 0 && a.size() > 2) threads = std::stoi(a.substr(2));
    }
    return cmd_extract(arc, out, threads);
  } else if (cmd == "t") {
    return cmd_test(argv[2]);
  } else if (cmd == "l") {
    return cmd_list(argv[2]);
  }
  print_usage();
  return 1;
}
