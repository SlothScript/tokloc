#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <string_view>
#include <algorithm>
#include <unordered_map>
#include <map>
#include <chrono>
#include <iomanip>
#include <cctype>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <CLI/CLI.hpp>

namespace fs = std::filesystem;

// ===================== OPTIONS =====================

struct Options {
    bool verbose = false;
    bool all = false;
    bool parallel = true;
    std::vector<std::string> include_patterns;
    std::vector<std::string> paths;
};

// ===================== SPLIT =====================

static std::vector<std::string_view> split_view(std::string_view s, char sep) {
    std::vector<std::string_view> out;
    size_t start = 0;

    while (true) {
        size_t end = s.find(sep, start);
        out.push_back(s.substr(start, end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return out;
}

// ===================== GLOB =====================

static bool glob_match(std::string_view pat, std::string_view str) {
    size_t p = 0, s = 0, star = std::string_view::npos, match = 0;

    while (s < str.size()) {
        if (p < pat.size() && (pat[p] == '?' || pat[p] == str[s])) {
            p++; s++;
        } else if (p < pat.size() && pat[p] == '*') {
            star = p++;
            match = s;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            s = ++match;
        } else return false;
    }

    while (p < pat.size() && pat[p] == '*') p++;
    return p == pat.size();
}

// ===================== PATTERN MATCH =====================

static bool match_pattern(
    const std::vector<std::string>& pat,
    const std::vector<std::string_view>& path
) {
    size_t i = 0, j = 0;

    while (i < pat.size() && j < path.size()) {
        const auto& p = pat[i];

        if (p == "**") {
            if (i + 1 == pat.size()) return true;

            for (size_t k = j; k <= path.size(); ++k) {
                std::vector<std::string_view> sub(path.begin() + k, path.end());
                std::vector<std::string> rest(pat.begin() + i + 1, pat.end());
                if (match_pattern(rest, sub)) return true;
            }
            return false;
        }

        if (!glob_match(p, path[j])) return false;

        i++; j++;
    }

    return i == pat.size() && j == path.size();
}

// ===================== INCLUDE FILTER =====================

class IncludeFilter {
    std::vector<std::vector<std::string>> patterns;

public:
    IncludeFilter(const std::vector<std::string>& incl_pats) {
        for (const auto& pat : incl_pats) {
            auto parts = split_view(pat, '/');
            std::vector<std::string> converted;
            for (auto p : parts) converted.emplace_back(p);
            patterns.push_back(std::move(converted));
        }
    }

    bool matches(std::string_view rel) const {
        if (patterns.empty()) return true;

        auto path = split_view(rel, '/');

        for (const auto& pat : patterns) {
            if (path.size() >= pat.size()) {
                for (size_t start = 0; start <= path.size() - pat.size() + 1; ++start) {
                    std::vector<std::string_view> sub(path.begin() + start, path.end());
                    if (match_pattern(pat, sub)) return true;
                }
            }
            std::string filename = std::string(rel);
            size_t sep = filename.find_last_of("/\\");
            if (sep != std::string::npos) {
                filename = filename.substr(sep + 1);
            } else if (!filename.empty() && filename[0] == '.') {
                std::string_view fn = rel;
                size_t dot = fn.find('.');
                if (dot != std::string_view::npos) {
                    filename = std::string(fn.substr(dot));
                }
            }

            std::string_view fn_sv(filename);
            for (const auto& p : pat) {
                if (glob_match(p, fn_sv)) return true;
            }
        }
        return false;
    }
};

// ===================== GITIGNORE ENGINE =====================

class IgnoreFilter {
    struct Rule {
        std::vector<std::string> parts;
        bool negate = false;
        bool dir_only = false;
        bool anchored = false;
    };

    std::vector<Rule> rules;

public:
    IgnoreFilter(const fs::path& root) {
        auto add_default = [&](std::string s, bool dir_only = false) {
            Rule r;
            if (dir_only) r.dir_only = true;
            if (!s.empty() && s.back() == '/') s.pop_back();
            auto parts = split_view(s, '/');
            for (auto p : parts) r.parts.emplace_back(p);
            rules.push_back(std::move(r));
        };

        add_default(".git", true);

        fs::path file = root / ".gitignore";
        if (!fs::exists(file)) return;

        std::ifstream in(file);
        std::string line;

        while (std::getline(in, line)) {
            line.erase(0, line.find_first_not_of(" \t\r\n"));
            line.erase(line.find_last_not_of(" \t\r\n") + 1);

            if (line.empty() || line[0] == '#') continue;

            Rule r;

            if (line[0] == '!') {
                r.negate = true;
                line.erase(0, 1);
            }

            if (!line.empty() && line.back() == '/') {
                r.dir_only = true;
                line.pop_back();
            }

            if (!line.empty() && line[0] == '/') {
                r.anchored = true;
                line.erase(0, 1);
            }

            auto parts = split_view(line, '/');
            for (auto p : parts) r.parts.emplace_back(p);

            rules.push_back(std::move(r));
        }
    }

    bool is_ignored(std::string_view rel, bool is_dir) const {
        auto path = split_view(rel, '/');

        bool ignored = false;

        for (const auto& r : rules) {
            if (r.dir_only && !is_dir) continue;

            bool match = false;

            if (r.anchored) {
                if (path.size() < r.parts.size()) continue;

                std::vector<std::string_view> sub(
                    path.begin(),
                    path.begin() + r.parts.size()
                );

                match = match_pattern(r.parts, sub);
            } else {
                for (size_t start = 0; start <= path.size(); ++start) {
                    std::vector<std::string_view> sub(
                        path.begin() + start,
                        path.end()
                    );

                    if (match_pattern(r.parts, sub)) {
                        match = true;
                        break;
                    }
                }
            }

            if (match) ignored = !r.negate;
        }

        return ignored;
    }
};

// ===================== BINARY DETECTION =====================

static bool is_binary_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;

    char buffer[8192];
    f.read(buffer, sizeof(buffer));
    std::streamsize bytes_read = f.gcount();

    for (std::streamsize i = 0; i < bytes_read; ++i) {
        unsigned char c = static_cast<unsigned char>(buffer[i]);
        if (c == 0 || (c < 32 && c != 9 && c != 10 && c != 13)) {
            return true;
        }
    }
    return false;
}

// ===================== LINE STATS =====================

struct LineStats {
    long long non_empty = 0;
    long long empty = 0;
};

static LineStats count_lines(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return {-1, -1};

    LineStats st;
    std::string line;

    while (std::getline(f, line)) {
        if (line.find_first_not_of(" \t\r\n") == std::string::npos)
            st.empty++;
        else
            st.non_empty++;
    }

    return st;
}

// ===================== FILE TYPE =====================

static std::string file_type(const std::string& rel) {
    fs::path p(rel);

    std::string ext = p.extension().string();
    for (auto& c : ext) c = std::tolower((unsigned char)c);

    std::string name = p.filename().string();
    for (auto& c : name) c = std::tolower((unsigned char)c);

    auto has = [&](std::initializer_list<std::string> xs) {
        for (auto& x : xs) if (x == ext) return true;
        return false;
    };

    if (has({".c",".cpp",".h",".hpp",".rs",".go",".py",".js",".ts"})) return "code";
    if (has({".md",".txt"})) return "docs";
    if (has({".json",".yaml",".yml",".xml"})) return "data";
    if (has({".html",".htm"})) return "html";
    if (has({".png",".jpg",".jpeg",".gif",".webp",".svg"})) return "image";

    if (name == "makefile" || name == "dockerfile") return "code";

    return "other";
}

// ===================== TOKEN ESTIMATION =====================

static double density(const std::string& type) {
    if (type == "code") return 4.2;
    if (type == "docs") return 3.6;
    if (type == "data") return 3.3;
    if (type == "html") return 3.7;
    if (type == "image") return 1e9;
    return 4.0;
}

static long long estimate_tokens(size_t byte_size, const std::string& type) {
    if (type == "image") return 0;
    return (long long)(byte_size / density(type));
}

// ===================== FILE STATS =====================

struct FileStats {
    std::string path;
    long long non_empty;
    long long empty;
    long long tokens;
};

// ===================== PROCESS FILE =====================

static std::optional<FileStats> process_file(
    const fs::path& full_path,
    const std::string& display_path,
    bool is_binary
) {
    if (is_binary) {
        auto byte_size = fs::file_size(full_path);
        std::string t = file_type(display_path);
        long long tok = estimate_tokens(byte_size, t);
        return FileStats{display_path, 0, 0, tok};
    }

    auto ls = count_lines(full_path);
    if (ls.non_empty < 0) return std::nullopt;

    auto byte_size = fs::file_size(full_path);
    std::string t = file_type(display_path);
    long long tok = estimate_tokens(byte_size, t);

    return FileStats{display_path, ls.non_empty, ls.empty, tok};
}

// ===================== WALKER (SINGLE THREAD) =====================

static std::vector<FileStats> walk_single(
    const fs::path& root_in,
    const Options& opt,
    const IgnoreFilter& filter,
    const IncludeFilter& include_filter
) {
    std::vector<FileStats> out;
    fs::path root = fs::weakly_canonical(root_in);

    for (auto it = fs::recursive_directory_iterator(
        root,
        fs::directory_options::skip_permission_denied);
        it != fs::recursive_directory_iterator();
        ++it)
    {
        auto& e = *it;

        auto rel = e.path().lexically_relative(root);
        std::string rel_s = rel.generic_string();

        bool is_dir = e.is_directory();

        if (!opt.all && filter.is_ignored(rel_s, is_dir)) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }

        if (!include_filter.matches(rel_s)) continue;

        if (!e.is_regular_file()) continue;

        bool is_bin = is_binary_file(e.path());
        auto stat = process_file(e.path(), rel_s, is_bin);

        if (stat) out.push_back(*stat);
    }

    return out;
}

// ===================== PARALLEL PROCESSOR =====================

class ParallelProcessor {
    const Options& opt;
    const IncludeFilter& include_filter;
    std::mutex mutex;
    std::vector<FileStats> results;

public:
    ParallelProcessor(const Options& o, const IncludeFilter& inc)
        : opt(o), include_filter(inc) {}

    void add_result(FileStats&& stat) {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(std::move(stat));
    }

    std::vector<FileStats> get_results() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::move(results);
    }

    void process_directory(const fs::path& root) {
        IgnoreFilter filter(root);
        fs::path canonical = fs::weakly_canonical(root);

        std::vector<fs::path> files;

        for (auto it = fs::recursive_directory_iterator(
            canonical,
            fs::directory_options::skip_permission_denied);
            it != fs::recursive_directory_iterator();
            ++it)
        {
            auto& e = *it;

            auto rel = e.path().lexically_relative(canonical);
            std::string rel_s = rel.generic_string();

            bool is_dir = e.is_directory();

            if (!opt.all && filter.is_ignored(rel_s, is_dir)) {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }

            if (!include_filter.matches(rel_s)) continue;

            if (!e.is_regular_file()) continue;

            files.push_back(e.path());
        }

        size_t num_threads = std::thread::hardware_concurrency();
        if (num_threads == 0) num_threads = 4;
        num_threads = std::min(num_threads, files.size());
        if (num_threads == 0) return;

        std::atomic<size_t> index(0);

        auto worker = [&]() {
            while (true) {
                size_t i = index.fetch_add(1);
                if (i >= files.size()) break;

                const auto& file = files[i];
                bool is_bin = is_binary_file(file);
                auto stat = process_file(file, file.filename().generic_string(), is_bin);

                if (stat) add_result(std::move(*stat));
            }
        };

        std::vector<std::thread> threads;
        for (size_t i = 0; i < num_threads; ++i) {
            threads.emplace_back(worker);
        }
        for (auto& t : threads) {
            t.join();
        }
    }

    void process_file_single(const fs::path& path) {
        bool is_bin = is_binary_file(path);
        auto stat = process_file(path, path.filename().generic_string(), is_bin);
        if (stat) add_result(std::move(*stat));
    }
};

// ===================== SUMMARY =====================

static void print_summary(const std::vector<FileStats>& v, double sec) {
    struct Agg { long long f=0,l=0,e=0,t=0; };
    std::map<std::string, Agg> m;

    for (auto& x : v) {
        auto t = file_type(x.path);
        auto& a = m[t];
        a.f++; a.l += x.non_empty; a.e += x.empty; a.t += x.tokens;
    }

    long long tf=0, tl=0, te=0, tt=0;

    std::cout << std::left
              << std::setw(12) << "Type"
              << std::setw(8) << "Files"
              << std::setw(10) << "Lines"
              << std::setw(10) << "Empty"
              << std::setw(12) << "Tokens"
              << "\n";

    std::cout << std::string(55,'-') << "\n";

    for (auto& [k,a] : m) {
        tf+=a.f; tl+=a.l; te+=a.e; tt+=a.t;

        std::cout << std::left
                  << std::setw(12) << k
                  << std::setw(8) << a.f
                  << std::setw(10) << a.l
                  << std::setw(10) << a.e
                  << std::setw(12) << a.t
                  << "\n";
    }

    std::cout << std::string(55,'-') << "\n";

    std::cout << std::left
              << std::setw(12) << "Total"
              << std::setw(8) << tf
              << std::setw(10) << tl
              << std::setw(10) << te
              << std::setw(12) << tt
              << "\n";

    double total_lines = tl + te;
    double files_per_s = sec > 0.0 ? static_cast<double>(tf) / sec : 0.0;
    double lines_per_s = sec > 0.0 ? static_cast<double>(total_lines) / sec : 0.0;
    double toks_per_s  = sec > 0.0 ? static_cast<double>(tt) / sec : 0.0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Elapsed: " << sec << "s"
              << " | files/s: " << files_per_s
              << " | lines/s: " << lines_per_s
              << " | tok/s: "  << toks_per_s
              << "\n";
    std::cout << std::defaultfloat;
}

// ===================== MAIN =====================

#ifndef TESTING
int main(int argc, char** argv) {
    CLI::App app{"Count lines and estimate tokens in code files"};

    Options opt;
    app.add_flag("-v,--verbose", opt.verbose, "Show detailed file-by-file statistics");
    app.add_flag("-a,--all", opt.all, "Include files/folders usually ignored");
    app.add_option("-i,--include", opt.include_patterns, "Include files matching pattern");
    app.add_flag("-S,--no-parallel", opt.parallel, "Disable parallel processing");

    std::vector<std::string> paths;
    app.add_option("paths", paths, "Paths to scan")
        ->required()
        ->expected(1, -1);

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (e.get_exit_code() == 1 || strcmp(e.what(), "paths is required") == 0) {
            std::cerr << "Error: Missing required argument 'paths'\n\n";
        }
        std::cerr << app.help();
        return 1;
    }

    opt.paths = paths;
    opt.parallel = !opt.parallel;

    auto start = std::chrono::steady_clock::now();
    std::vector<FileStats> all_results;

    IncludeFilter include_filter(opt.include_patterns);

    for (const auto& path_str : opt.paths) {
        fs::path p(path_str);

        if (!fs::exists(p)) {
            std::cerr << "Error: Path does not exist: " << path_str << "\n";
            continue;
        }

        if (fs::is_regular_file(p)) {
            ParallelProcessor proc(opt, include_filter);
            proc.process_file_single(p);
            auto results = proc.get_results();
            all_results.insert(all_results.end(),
                std::make_move_iterator(results.begin()),
                std::make_move_iterator(results.end()));
        } else if (fs::is_directory(p)) {
            if (opt.verbose) {
                std::cout << "Scanning: " << p << "\n\n";
            }

            if (opt.parallel) {
                ParallelProcessor proc(opt, include_filter);
                proc.process_directory(p);
                auto results = proc.get_results();
                all_results.insert(all_results.end(),
                    std::make_move_iterator(results.begin()),
                    std::make_move_iterator(results.end()));
            } else {
                auto results = walk_single(p, opt, IgnoreFilter(p), include_filter);
                all_results.insert(all_results.end(),
                    std::make_move_iterator(results.begin()),
                    std::make_move_iterator(results.end()));
            }
        }
    }

    if (opt.verbose) {
        std::cout << "\n\n" << std::flush;
    }

    auto end = std::chrono::steady_clock::now();

    print_summary(all_results,
        std::chrono::duration<double>(end - start).count());

    return 0;
}
#endif