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
#include <cstring>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <limits>
#include <curl/curl.h>
#include <CLI/CLI.hpp>
#include "tokenizer.cpp"

namespace fs = std::filesystem;

// ===================== OPTIONS =====================

struct Options {
    bool verbose = false;
    bool all = false;
    size_t num_threads = 0;
    std::vector<std::string> include_patterns;
    std::vector<std::string> exclude_patterns;
    std::vector<std::string> paths;
    std::string tokenizer_path;
    std::string tokenizer_url;
    std::size_t tokenizer_url_max_mb = 100;
};

static bool is_http_url(const std::string& s) {
    return s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0;
}

static bool looks_like_json(const std::string& payload) {
    std::size_t start = 0;
    while (start < payload.size() && std::isspace(static_cast<unsigned char>(payload[start]))) {
        ++start;
    }
    if (start >= payload.size()) return false;

    std::size_t end = payload.size();
    while (end > start && std::isspace(static_cast<unsigned char>(payload[end - 1]))) {
        --end;
    }
    if (end <= start) return false;

    const char first = payload[start];
    const char last = payload[end - 1];
    const bool obj = first == '{' && last == '}';
    const bool arr = first == '[' && last == ']';
    return obj || arr;
}

struct UrlDownloadResult {
    bool ok = false;
    std::string payload;
    std::string content_type;
    std::string error;
    long http_status = 0;
};

struct UrlDownloadContext {
    std::string* out = nullptr;
    std::size_t max_bytes = 0;
    bool size_exceeded = false;
};

static std::size_t url_write_callback(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* ctx = static_cast<UrlDownloadContext*>(userdata);
    const std::size_t bytes = size * nmemb;
    if (bytes == 0 || ctx == nullptr || ctx->out == nullptr) return bytes;

    if (ctx->out->size() + bytes > ctx->max_bytes) {
        ctx->size_exceeded = true;
        return 0;
    }

    ctx->out->append(ptr, bytes);
    return bytes;
}

static UrlDownloadResult download_url_to_memory(
    const std::string& url,
    std::size_t max_bytes
) {
    UrlDownloadResult result;
    if (!is_http_url(url)) {
        result.error = "URL must start with http:// or https://";
        return result;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        result.error = "Failed to initialize curl";
        return result;
    }

    UrlDownloadContext ctx{&result.payload, max_bytes, false};
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "tokloc/1.0");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, url_write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);

    CURLcode code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.http_status);

    char* content_type = nullptr;
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    if (content_type != nullptr) {
        result.content_type = content_type;
    }

    if (ctx.size_exceeded) {
        result.error = "Download exceeded max size";
        curl_easy_cleanup(curl);
        return result;
    }

    if (code != CURLE_OK) {
        result.error = curl_easy_strerror(code);
        curl_easy_cleanup(curl);
        return result;
    }

    if (result.http_status < 200 || result.http_status >= 300) {
        result.error = "HTTP request failed with status " + std::to_string(result.http_status);
        curl_easy_cleanup(curl);
        return result;
    }

    result.ok = true;
    curl_easy_cleanup(curl);
    return result;
}

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

            const std::vector<std::string> rest(pat.begin() + i + 1, pat.end());
            for (size_t k = j; k <= path.size(); ++k) {
                std::vector<std::string_view> sub(path.begin() + k, path.end());
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

        std::string_view fn = rel;
        if (auto sep = rel.find_last_of("/\\"); sep != std::string_view::npos) {
            fn = rel.substr(sep + 1);
        }

        for (const auto& pat : patterns) {
            if (path.size() >= pat.size()) {
                for (size_t start = 0; start <= path.size() - pat.size() + 1; ++start) {
                    std::vector<std::string_view> sub(path.begin() + start, path.end());
                    if (match_pattern(pat, sub)) return true;
                }
            }

            if (pat.size() <= 1) {
                for (const auto& p : pat) {
                    if (glob_match(p, fn)) return true;
                }
            }
        }
        return false;
    }
};

// ===================== EXCLUDE FILTER =====================

class ExcludeFilter {
    std::vector<std::vector<std::string>> patterns;

public:
    ExcludeFilter(const std::vector<std::string>& excl_pats) {
        for (const auto& pat : excl_pats) {
            auto parts = split_view(pat, '/');
            std::vector<std::string> converted;
            for (auto p : parts) converted.emplace_back(p);
            patterns.push_back(std::move(converted));
        }
    }

    bool matches(std::string_view rel) const {
        if (patterns.empty()) return false; // No patterns means nothing is excluded

        auto path = split_view(rel, '/');

        std::string_view fn = rel;
        if (auto sep = rel.find_last_of("/\\"); sep != std::string_view::npos) {
            fn = rel.substr(sep + 1);
        }

        for (const auto& pat : patterns) {
            if (path.size() >= pat.size()) {
                for (size_t start = 0; start <= path.size() - pat.size() + 1; ++start) {
                    std::vector<std::string_view> sub(path.begin() + start, path.end());
                    if (match_pattern(pat, sub)) return true;
                }
            }

            if (pat.size() <= 1) {
                for (const auto& p : pat) {
                    if (glob_match(p, fn)) return true;
                }
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

    constexpr size_t CHUNK_SIZE = 4096;
    constexpr size_t MAX_CHECK = 64 * 1024;

    char buffer[CHUNK_SIZE];
    size_t total_read = 0;
    size_t suspicious_bytes = 0;
    size_t total_bytes = 0;

    while (f && total_read < MAX_CHECK) {
        f.read(buffer, CHUNK_SIZE);
        std::streamsize n = f.gcount();
        if (n <= 0) break;

        total_read += n;

        for (std::streamsize i = 0; i < n; ++i) {
            unsigned char c = static_cast<unsigned char>(buffer[i]);
            total_bytes++;

            if (c == 0) return true;

            if (c < 32 && c != 9 && c != 10 && c != 13) return true;
        }
    }

    if (total_bytes == 0) return false;

    double ratio = static_cast<double>(suspicious_bytes) / total_bytes;

    return ratio > 0.30;
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

    if (has({".c",".cpp",".cxx",".cc",".h",".hpp",".hxx",".rs",".go",".py",".js",".ts",".jsx",".tsx",".java",".rb",".php",".swift",".kt",".cs",".scala",".m",".mm"})) return "code";
    if (has({".md",".txt"})) return "docs";
    if (has({".json",".yaml",".yml",".xml"})) return "data";
    if (has({".html",".htm"})) return "html";
    if (has({".png",".jpg",".jpeg",".gif",".webp",".svg"})) return "image";

    if (name == "makefile" || name == "dockerfile") return "code";

    return "other";
}

// ===================== TOKEN ESTIMATION =====================

static double density(const std::string& type) {
    if (type == "code")  return 3.8;
    if (type == "docs")  return 4.0;
    if (type == "data")  return 3.2;
    if (type == "html")  return 3.4;
    if (type == "image")  return std::numeric_limits<double>::infinity();
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
    const std::string& display_path
) {
    bool is_bin = is_binary_file(full_path);
    
    if (is_bin) {
        std::string t = file_type(display_path);
        return FileStats{display_path, 0, 0, 0};
    }

    auto ls = count_lines(full_path);
    if (ls.non_empty < 0) return std::nullopt;

    auto byte_size = fs::file_size(full_path);
    std::string t = file_type(display_path);
    long long tok = 0;

    if (!is_bin && t != "image" && is_tokenizer_initialized()) {
        std::ifstream f(full_path);
        if (f) {
            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            int token_count = count_tokens_with_tokenizer(content);
            if (token_count >= 0) {
                tok = token_count;
            } else {
                tok = estimate_tokens(byte_size, t);
            }
        } else {
            tok = estimate_tokens(byte_size, t);
        }
    } else {
        tok = estimate_tokens(byte_size, t);
    }

    return FileStats{display_path, ls.non_empty, ls.empty, tok};
}

static constexpr size_t LARGE_FILE_THRESHOLD = 10 * 1024 * 1024;
static constexpr size_t CHUNK_SIZE = 4 * 1024 * 1024;
static constexpr size_t CHUNK_OVERLAP = 1024;

static std::vector<FileStats> process_large_file(
    const fs::path& full_path,
    const std::string& display_path,
    size_t num_threads
) {
    std::vector<FileStats> results;
    
    bool is_bin = is_binary_file(full_path);
    if (is_bin) {
        auto ls = count_lines(full_path);
        results.push_back(FileStats{display_path, ls.non_empty, ls.empty, 0});
        return results;
    }
    
    auto byte_size = fs::file_size(full_path);
    size_t num_chunks = (byte_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
    
    std::vector<std::string> chunks(num_chunks);
    std::atomic<size_t> chunks_loaded(0);
    
    auto load_worker = [&]() {
        while (true) {
            size_t idx = chunks_loaded.fetch_add(1);
            if (idx >= num_chunks) break;
            
            std::ifstream f(full_path);
            if (!f) continue;
            
            f.seekg(idx * CHUNK_SIZE);
            size_t remaining = byte_size - idx * CHUNK_SIZE;
            size_t to_read = std::min(CHUNK_SIZE, remaining);
            
            chunks[idx].resize(to_read);
            f.read(&chunks[idx][0], to_read);
            chunks[idx].resize(f.gcount());
        }
    };
    
    std::vector<std::thread> load_threads;
    size_t load_thread_count = std::min(num_chunks, size_t(4));
    for (size_t i = 0; i < load_thread_count; ++i) {
        load_threads.emplace_back(load_worker);
    }
    for (auto& t : load_threads) t.join();
    
    std::vector<long long> token_counts(num_chunks, 0);
    std::atomic<size_t> chunk_idx(0);
    
    auto tokenize_worker = [&]() {
        while (true) {
            size_t i = chunk_idx.fetch_add(1);
            if (i >= num_chunks) break;
            
            if (!chunks[i].empty()) {
                std::string_view sv(chunks[i]);
                size_t offset = 0;
                if (i > 0) {
                    offset = CHUNK_OVERLAP;
                }
                if (offset < sv.size()) {
                    std::string_view trimmed = sv.substr(offset);
                    int tc = count_tokens_with_tokenizer(std::string(trimmed));
                    token_counts[i] = tc >= 0 ? tc : 0;
                }
            }
        }
    };
    
    std::vector<std::thread> token_threads;
    for (size_t i = 0; i < num_threads; ++i) {
        token_threads.emplace_back(tokenize_worker);
    }
    for (auto& t : token_threads) t.join();
    
    long long total_tokens = 0;
    size_t total_lines = 0;
    size_t empty_lines = 0;
    
    auto ls = count_lines(full_path);
    if (ls.non_empty >= 0) {
        total_lines = ls.non_empty;
        empty_lines = ls.empty;
    }
    
    total_tokens = 0;
    for (size_t i = 0; i < num_chunks; ++i) {
        total_tokens += token_counts[i];
    }
    
    results.push_back(FileStats{display_path, static_cast<long long>(total_lines), static_cast<long long>(empty_lines), total_tokens});
    
    return results;
}

// ===================== WALKER (SINGLE THREAD) =====================

static std::vector<FileStats> walk_single(
    const fs::path& root_in,
    const Options& opt,
    const IgnoreFilter& filter,
    const IncludeFilter& include_filter,
    const ExcludeFilter& exclude_filter
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

        if (exclude_filter.matches(rel_s)) {
            if (is_dir) it.disable_recursion_pending();
            continue;
        }

        if (!e.is_regular_file()) continue;

        auto stat = process_file(e.path(), rel_s);

        if (stat) {
            if (opt.verbose) {
                std::cout << std::left << std::setw(50) << stat->path 
                          << ": " << std::right << std::setw(6) << stat->non_empty 
                          << " lines, " << std::setw(5) << stat->empty 
                          << " empty, " << std::setw(8) << stat->tokens 
                          << " tokens\n";
            }
            out.push_back(*stat);
        }
    }

    return out;
}

// ===================== PARALLEL PROCESSOR =====================

class ParallelProcessor {
    const Options& opt;
    const IncludeFilter& include_filter;
    const ExcludeFilter& exclude_filter;
    const IgnoreFilter& ignore_filter;
    std::mutex mutex;
    std::vector<FileStats> results;

public:
    ParallelProcessor(const Options& o, const IncludeFilter& inc, const ExcludeFilter& exc, const IgnoreFilter& ign)
        : opt(o), include_filter(inc), exclude_filter(exc), ignore_filter(ign) {}

    void add_result(FileStats&& stat) {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(std::move(stat));
    }
    
    void print_verbose(const FileStats& stat) {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << std::left << std::setw(50) << stat.path 
                  << ": " << std::right << std::setw(6) << stat.non_empty 
                  << " lines, " << std::setw(5) << stat.empty 
                  << " empty, " << std::setw(8) << stat.tokens 
                  << " tokens\n";
    }

    std::vector<FileStats> get_results() {
        std::lock_guard<std::mutex> lock(mutex);
        return results;
    }

    void process_directory(const fs::path& root) {
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

            if (!opt.all && ignore_filter.is_ignored(rel_s, is_dir)) {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }

            if (!include_filter.matches(rel_s)) continue;

            if (exclude_filter.matches(rel_s)) {
                if (is_dir) it.disable_recursion_pending();
                continue;
            }

            if (!e.is_regular_file()) continue;

            files.push_back(e.path());
        }

        if (opt.verbose) {
            std::cout << "Found " << files.size() << " files to process\n\n";
        }

        size_t num_threads = opt.num_threads;
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }
        num_threads = std::min(num_threads, files.size());
        if (num_threads == 0) return;

        std::atomic<size_t> index(0);

        auto worker = [&]() {
            while (true) {
                size_t i = index.fetch_add(1);
                if (i >= files.size()) break;

                const auto& file = files[i];
                auto byte_size = fs::file_size(file);
                std::string rel_path = opt.verbose ? file.lexically_relative(canonical).generic_string() : file.filename().generic_string();
                
                if (byte_size > LARGE_FILE_THRESHOLD && is_tokenizer_initialized()) {
                    auto stats = process_large_file(file, rel_path, num_threads);
                    for (auto& stat : stats) {
                        if (opt.verbose) print_verbose(stat);
                        add_result(std::move(stat));
                    }
                } else if (opt.verbose) {
                    auto stat = process_file(file, rel_path);
                    if (stat) {
                        print_verbose(*stat);
                        add_result(std::move(*stat));
                    }
                } else {
                    auto stat = process_file(file, rel_path);
                    if (stat) add_result(std::move(*stat));
                }
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
        std::string rel_s = path.generic_string();
        if (!include_filter.matches(rel_s)) return;
        if (exclude_filter.matches(rel_s)) return;

        auto stat = process_file(path, path.filename().generic_string());
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
    app.add_option("-x,--exclude", opt.exclude_patterns, "Exclude files matching pattern");
    app.add_option("-j,--jobs", opt.num_threads, "Number of parallel jobs (default: auto, -j1 = single threaded)");
    app.add_option("--tokenizer-path", opt.tokenizer_path, "Path to tokenizer vocabulary file");
    app.add_option("--tokenizer-url", opt.tokenizer_url, "HTTP(S) URL to tokenizer JSON");
    app.add_option("--tokenizer-url-max-mb", opt.tokenizer_url_max_mb, "Max tokenizer download size in MB (default: 100)");

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
    if (opt.num_threads == 0) {
        opt.num_threads = std::thread::hardware_concurrency();
        if (opt.num_threads == 0) opt.num_threads = 4;
    }

    const CURLcode curl_init = curl_global_init(CURL_GLOBAL_DEFAULT);
    const bool curl_ready = (curl_init == CURLE_OK);
    if (!curl_ready) {
        std::cerr << "Warning: Failed to initialize curl runtime: "
                  << curl_easy_strerror(curl_init) << "\n";
    }
    struct CurlGlobalGuard {
        bool enabled = false;
        ~CurlGlobalGuard() {
            if (enabled) curl_global_cleanup();
        }
    } curl_guard{curl_ready};

    // Initialize tokenizer if source provided
    if (!opt.tokenizer_path.empty() && !opt.tokenizer_url.empty()) {
        std::cerr << "Error: Use either --tokenizer-path or --tokenizer-url, not both.\n";
        return 1;
    }

    if (!opt.tokenizer_url.empty()) {
        if (opt.tokenizer_url_max_mb == 0) {
            std::cerr << "Error: --tokenizer-url-max-mb must be greater than 0.\n";
            return 1;
        }
        if (opt.tokenizer_url_max_mb > (std::numeric_limits<std::size_t>::max() / (1024ULL * 1024ULL))) {
            std::cerr << "Error: --tokenizer-url-max-mb is too large.\n";
            return 1;
        }

        const std::size_t max_bytes = opt.tokenizer_url_max_mb * 1024ULL * 1024ULL;
        auto download = download_url_to_memory(opt.tokenizer_url, max_bytes);
        if (!download.ok) {
            std::cerr << "Warning: Failed to download tokenizer from URL: " << opt.tokenizer_url << "\n";
            std::cerr << "Reason: " << download.error << "\n";
            std::cerr << "Falling back to token estimation.\n";
        } else if (!looks_like_json(download.payload)) {
            std::cerr << "Warning: Tokenizer URL content is not JSON.\n";
            std::cerr << "Falling back to token estimation.\n";
        } else if (!init_tokenizer_from_json_content(download.payload)) {
            std::cerr << "Warning: Failed to initialize tokenizer from JSON content.\n";
            std::cerr << "Falling back to token estimation.\n";
        } else if (opt.verbose) {
            std::cout << "Loaded tokenizer from URL: " << opt.tokenizer_url
                      << " (" << download.payload.size() << " bytes)\n";
        }
    } else if (!opt.tokenizer_path.empty()) {
        if (init_tokenizer_from_file(opt.tokenizer_path)) {
            if (opt.verbose) {
                std::cout << "Loaded tokenizer from: " << opt.tokenizer_path << "\n";
            }
        } else {
            std::cerr << "Warning: Failed to load tokenizer from: " << opt.tokenizer_path << "\n";
            std::cerr << "Falling back to token estimation.\n";
        }
    }

    auto start = std::chrono::steady_clock::now();
    std::vector<FileStats> all_results;

    IncludeFilter include_filter(opt.include_patterns);

    for (const auto& pat : opt.include_patterns) {
        bool has_star = pat.find('*') != std::string::npos;
        bool has_q = pat.find('?') != std::string::npos;
        bool has_dstar = pat.find("**") != std::string::npos;
        if (!has_star && !has_q && !has_dstar) {
            std::cerr << "Warning: Include pattern '" << pat << "' has no glob chars, treating as literal\n";
        }
    }
     
    for (const auto& pat : opt.exclude_patterns) {
        bool has_star = pat.find('*') != std::string::npos;
        bool has_q = pat.find('?') != std::string::npos;
        bool has_dstar = pat.find("**") != std::string::npos;
        if (!has_star && !has_q && !has_dstar) {
            std::cerr << "Warning: Exclude pattern '" << pat << "' has no glob chars, treating as literal\n";
        }
    }

    for (const auto& path_str : opt.paths) {
        fs::path p(path_str);

        if (!fs::exists(p)) {
            std::cerr << "Error: Path does not exist: " << path_str << "\n";
            continue;
        }

        if (fs::is_regular_file(p)) {
            IgnoreFilter ignore_filter(p.parent_path());
            ExcludeFilter exclude_filter(opt.exclude_patterns);
            ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);
            proc.process_file_single(p);
            auto results = proc.get_results();
            all_results.insert(all_results.end(),
                std::make_move_iterator(results.begin()),
                std::make_move_iterator(results.end()));
        } else if (fs::is_directory(p)) {
            if (opt.verbose) {
                std::cout << "Scanning: " << p << "\n\n";
            }

            IgnoreFilter ignore_filter(p);
            ExcludeFilter exclude_filter(opt.exclude_patterns);
            if (opt.num_threads > 0) {
                ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);
                proc.process_directory(p);
                auto results = proc.get_results();
                all_results.insert(all_results.end(),
                    std::make_move_iterator(results.begin()),
                    std::make_move_iterator(results.end()));
             } else {
                 auto results = walk_single(p, opt, ignore_filter, include_filter, exclude_filter);
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
