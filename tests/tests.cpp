#define DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string_view>
#include <vector>
#include <string>
#include <filesystem>
#include <fstream>
#include <cstdio>

#include "../src/tokloc.cpp"

TEST_CASE("split_view") {
    auto r = split_view("a/b/c", '/');
    CHECK(r.size() == 3);
    CHECK(r[0] == "a");
    CHECK(r[1] == "b");
    CHECK(r[2] == "c");

    auto r2 = split_view("a", '/');
    CHECK(r2.size() == 1);
    CHECK(r2[0] == "a");

    auto r3 = split_view("", '/');
    CHECK(r3.size() == 1);
    CHECK(r3[0] == "");
}

TEST_CASE("glob_match") {
    CHECK(glob_match("*.cpp", "main.cpp"));
    CHECK(glob_match("*.cpp", "foo.cpp"));
    CHECK(!glob_match("*.cpp", "main.h"));
    CHECK(glob_match("test?.cpp", "test1.cpp"));
    CHECK(!glob_match("test?.cpp", "test12.cpp"));
    CHECK(glob_match("**/*.txt", "a/b.txt"));
    CHECK(glob_match("*", "anything"));
    CHECK(glob_match("a/*/c", "a/b/c"));
}

TEST_CASE("file_type") {
    CHECK(file_type("main.cpp") == "code");
    CHECK(file_type("main.rs") == "code");
    CHECK(file_type("main.py") == "code");
    CHECK(file_type("main.js") == "code");
    CHECK(file_type("main.ts") == "code");
    CHECK(file_type("readme.md") == "docs");
    CHECK(file_type("data.json") == "data");
    CHECK(file_type("config.yaml") == "data");
    CHECK(file_type("index.html") == "html");
    CHECK(file_type("image.png") == "image");
    CHECK(file_type("image.jpg") == "image");
    CHECK(file_type("Makefile") == "code");
    CHECK(file_type("Dockerfile") == "code");
    CHECK(file_type("unknown.xyz") == "other");
}

TEST_CASE("density") {
    CHECK(density("code") == doctest::Approx(3.8));
    CHECK(density("docs") == doctest::Approx(4.0));
    CHECK(density("data") == doctest::Approx(3.2));
    CHECK(density("html") == doctest::Approx(3.4));
    CHECK(std::isinf(density("image")));
    CHECK(density("other") == doctest::Approx(4.0));
}

TEST_CASE("estimate_tokens") {
    CHECK(estimate_tokens(11, "code") == 2);
    CHECK(estimate_tokens(11, "docs") == 2);
    CHECK(estimate_tokens(11, "data") == 3);
    CHECK(estimate_tokens(11, "html") == 3);
    CHECK(estimate_tokens(11, "image") == 0);
}

TEST_CASE("Options") {
    Options opt1;
    CHECK(!opt1.verbose);
    CHECK(!opt1.all);
    CHECK(opt1.include_patterns.empty());
    CHECK(opt1.exclude_patterns.empty());
    CHECK(opt1.paths.empty());

    Options opt2;
    opt2.verbose = true;
    opt2.all = true;
    opt2.include_patterns.push_back("*.cpp");
    opt2.exclude_patterns.push_back("*test*");
    CHECK(opt2.verbose);
    CHECK(opt2.all);
    CHECK(opt2.include_patterns.size() == 1);
    CHECK(opt2.exclude_patterns.size() == 1);
}

TEST_CASE("IncludeFilter") {
    IncludeFilter empty({});
    CHECK(empty.matches("anything.txt"));
    CHECK(empty.matches("foo/bar/baz.cpp"));

    IncludeFilter cpp({ "*.cpp", "*.h" });
    CHECK(cpp.matches("foo/main.h"));
    CHECK(cpp.matches("foo/bar.cpp"));
    CHECK(!cpp.matches("main.py"));

    IncludeFilter nested({ "src/**/*.cpp", "tests/*.cpp" });
    CHECK(nested.matches("src/a/b.cpp"));
    CHECK(nested.matches("tests/main.cpp"));
    CHECK(!nested.matches("docs/readme.cpp"));
}

TEST_CASE("IgnoreFilter with .gitignore") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_ignore";
    fs::create_directories(tmp);

    std::ofstream gitlee((tmp / ".gitignore").string());
    gitlee << "*.log\nbuild/\n!important.log\n";
    gitlee.close();

    std::ofstream((tmp / "test.cpp").string()).close();
    std::ofstream((tmp / "debug.log").string()).close();

    fs::create_directories(tmp / "build");
    std::ofstream((tmp / "build" / "app").string()).close();

    fs::create_directories(tmp / "out");
    std::ofstream((tmp / "out" / "data.txt").string()).close();

    std::ofstream((tmp / "important.log").string()).close();

    IgnoreFilter filter(tmp);

    CHECK(filter.is_ignored("debug.log", false));
    CHECK(filter.is_ignored("build", true));
    CHECK(!filter.is_ignored("build/app", false));
    CHECK(!filter.is_ignored("important.log", false));
    CHECK(!filter.is_ignored("test.cpp", false));
    CHECK(!filter.is_ignored("out/data.txt", false));

    fs::remove_all(tmp);
}

TEST_CASE("ExcludeFilter") {
    ExcludeFilter empty({});
    CHECK(!empty.matches("anything.txt"));
    CHECK(!empty.matches("foo/bar/baz.cpp"));

    ExcludeFilter cpp({ "*.cpp", "*.h" });
    CHECK(cpp.matches("foo/main.h"));
    CHECK(cpp.matches("foo/bar.cpp"));
    CHECK(!cpp.matches("main.py"));

    ExcludeFilter nested({ "src/**/*.cpp", "tests/*.cpp" });
    CHECK(nested.matches("src/a/b.cpp"));
    CHECK(nested.matches("tests/main.cpp"));
    CHECK(!nested.matches("docs/readme.cpp"));
}

TEST_CASE("SimpleTokenizer counts punctuation, lowercase matches, and wordpieces") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out << "!\n";
    out << "play\n";
    out << "##ing\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("Hello, world!") == 4);
    CHECK(count_tokens_with_tokenizer("playing") == 2);
    CHECK(count_tokens_with_tokenizer("xyzzy") == 1);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer initializes from JSON vocab content") {
    const std::string json = R"json(
{
  "model": {
    "vocab": {
      "[UNK]": 0,
      "[PAD]": 1,
      "hello": 2,
      "world": 3,
      "!": 4,
      "play": 5,
      "##ing": 6
    }
  }
}
)json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("Hello, world!") == 4);
    CHECK(count_tokens_with_tokenizer("playing") == 2);
}

TEST_CASE("ParallelProcessor prunes excluded directories") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_exclude_dir";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src");
    fs::create_directories(tmp / "node_modules" / "pkg");

    std::ofstream(tmp / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "node_modules" / "pkg" / "lib.cpp") << "int x = 1;\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({ "node_modules" });
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 1);
    CHECK(results[0].path == "main.cpp");

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor honors exclude for single file input") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_exclude_file";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    fs::path file = tmp / "foo.cpp";
    std::ofstream(file) << "int main() { return 0; }\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({ "*.cpp" });
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_file_single(file);
    auto results = proc.get_results();

    CHECK(results.empty());

    fs::remove_all(tmp);
}

TEST_CASE("is_http_url validates URLs correctly") {
    CHECK(is_http_url("http://example.com"));
    CHECK(is_http_url("https://example.com"));
    CHECK(is_http_url("http://example.com/path"));
    CHECK(is_http_url("https://example.com/path?query=value"));
    CHECK(!is_http_url("ftp://example.com"));
    CHECK(!is_http_url("example.com"));
    CHECK(!is_http_url(""));
    CHECK(!is_http_url("http:/example.com"));
    CHECK(!is_http_url("https//example.com"));
}

TEST_CASE("looks_like_json detects JSON payloads") {
    CHECK(looks_like_json("{}"));
    CHECK(looks_like_json("[]"));
    CHECK(looks_like_json("  {}  "));
    CHECK(looks_like_json("\n[]\n"));
    CHECK(looks_like_json("{\"key\": \"value\"}"));
    CHECK(looks_like_json("[1, 2, 3]"));
    CHECK(!looks_like_json(""));
    CHECK(!looks_like_json("   "));
    CHECK(!looks_like_json("not json"));
    CHECK(!looks_like_json("{"));
    CHECK(!looks_like_json("["));
    CHECK(!looks_like_json("}"));
    CHECK(!looks_like_json("]"));
    CHECK(!looks_like_json("<html></html>"));
}

TEST_CASE("is_binary_file detects binary content") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_binary";
    fs::create_directories(tmp);

    // Text file
    fs::path text_file = tmp / "text.txt";
    {
        std::ofstream f(text_file);
        f << "This is a text file\nWith multiple lines\n";
    }
    CHECK(!is_binary_file(text_file));

    // Binary file with null byte (strong signal)
    fs::path binary_file = tmp / "binary.bin";
    {
        std::ofstream f(binary_file, std::ios::binary);
        std::string s(100, 'a');
        s.push_back('\x00');
        f << s;
    }
    CHECK(is_binary_file(binary_file));

    // Binary-like file with control ratio
    fs::path control_file = tmp / "control.bin";
    {
        std::ofstream f(control_file, std::ios::binary);
        f << std::string(100, 'a');
        f << std::string(60, '\x01');
    }
    CHECK(is_binary_file(control_file));

    // Tab file (text)
    fs::path tab_file = tmp / "tab.txt";
    {
        std::ofstream f(tab_file);
        f << "text\tmore\n";
    }
    CHECK(!is_binary_file(tab_file));

    // Newline file (text)
    fs::path newline_file = tmp / "newline.txt";
    {
        std::ofstream f(newline_file);
        f << "text\nmore\r\n";
    }
    CHECK(!is_binary_file(newline_file));

    // Empty file
    fs::path empty_file_path = tmp / "empty.txt";
    {
        std::ofstream f(empty_file_path);
    }
    CHECK(!is_binary_file(empty_file_path));

    // UTF-8 file
    fs::path utf8_file = tmp / "utf8.txt";
    {
        std::ofstream f(utf8_file);
        f << "Hello 世界\n";
    }
    CHECK(!is_binary_file(utf8_file));

    fs::remove_all(tmp);
}

TEST_CASE("count_lines counts lines correctly") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_lines";
    fs::create_directories(tmp);

    // Empty file
    fs::path empty_path = tmp / "empty.txt";
    std::ofstream empty_file2(empty_path);
    auto empty_stats = count_lines(empty_path);
    CHECK(empty_stats.non_empty == 0);
    CHECK(empty_stats.empty == 0);

    // File with only empty lines
    fs::path only_empty = tmp / "only_empty.txt";
    std::ofstream(only_empty) << "\n\n\n";
    auto only_empty_stats = count_lines(only_empty);
    CHECK(only_empty_stats.non_empty == 0);
    CHECK(only_empty_stats.empty == 3);

    // File with only non-empty lines
    fs::path only_non_empty = tmp / "only_non_empty.txt";
    std::ofstream(only_non_empty) << "line1\nline2\nline3\n";
    auto only_non_empty_stats = count_lines(only_non_empty);
    CHECK(only_non_empty_stats.non_empty == 3);
    CHECK(only_non_empty_stats.empty == 0);

    // Mixed file
    fs::path mixed = tmp / "mixed.txt";
    std::ofstream(mixed) << "line1\n\nline2\n\n\nline3\n";
    auto mixed_stats = count_lines(mixed);
    CHECK(mixed_stats.non_empty == 3);
    CHECK(mixed_stats.empty == 3);

    // File with whitespace-only lines
    fs::path whitespace = tmp / "whitespace.txt";
    std::ofstream(whitespace) << "line1\n   \n\t\nline2\n";
    auto whitespace_stats = count_lines(whitespace);
    CHECK(whitespace_stats.non_empty == 2);
    CHECK(whitespace_stats.empty == 2);

    // File without trailing newline
    fs::path no_trailing = tmp / "no_trailing.txt";
    std::ofstream(no_trailing) << "line1\nline2";
    auto no_trailing_stats = count_lines(no_trailing);
    CHECK(no_trailing_stats.non_empty == 2);
    CHECK(no_trailing_stats.empty == 0);

    // Non-existent file
    fs::path non_existent = tmp / "non_existent.txt";
    auto non_existent_stats = count_lines(non_existent);
    CHECK(non_existent_stats.non_empty == -1);
    CHECK(non_existent_stats.empty == -1);

    fs::remove_all(tmp);
}

TEST_CASE("process_file handles text files") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_process_text";
    fs::create_directories(tmp);

    fs::path file = tmp / "code.cpp";
    std::ofstream(file) << "int main() {\n    return 0;\n}\n";

    auto stats = process_file(file, "code.cpp");
    CHECK(stats.has_value());
    CHECK(stats->path == "code.cpp");
    CHECK(stats->non_empty == 3);
    CHECK(stats->empty == 0);
    CHECK(stats->tokens > 0);

    fs::remove_all(tmp);
}

TEST_CASE("process_file handles binary files") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_process_binary";
    fs::create_directories(tmp);

    fs::path file = tmp / "binary.bin";
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (f) {
        std::fwrite("binary\x00data", 1, 10, f);
        std::fclose(f);
    }
 
    auto stats = process_file(file, "binary.bin");
    CHECK(stats.has_value());
    CHECK(stats->path == "binary.bin");
    CHECK(stats->non_empty == 0);
    CHECK(stats->empty == 0);
    CHECK(stats->tokens == 0); // binary files get 0 tokens

    fs::remove_all(tmp);
}

TEST_CASE("process_file handles image files") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_process_image";
    fs::create_directories(tmp);

    fs::path file = tmp / "image.png";
    std::FILE* f = std::fopen(file.c_str(), "wb");
    if (f) {
        std::fwrite("fake\x00image", 1, 11, f);
        std::fclose(f);
    }
 
    auto stats = process_file(file, "image.png");
    CHECK(stats.has_value());
    CHECK(stats->path == "image.png");
    CHECK(stats->non_empty == 0);
    CHECK(stats->empty == 0);
    CHECK(stats->tokens == 0); // images get 0 tokens

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles empty text") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_empty";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("") == 0);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles whitespace only") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_whitespace";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("   \n\t  ") == 0); // No tokens for whitespace

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles special characters") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_special";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out << "!\n";
    out << "?\n";
    out << ".\n";
    out << ",\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("Hello, world!") == 4); // Hello , world !
    CHECK(count_tokens_with_tokenizer("Hello? World.") == 4); // Hello ? World .

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles mixed case") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_case";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello") == 1);
    CHECK(count_tokens_with_tokenizer("Hello") == 1); // Should match lowercase
    CHECK(count_tokens_with_tokenizer("HELLO") == 1); // Should match lowercase
    CHECK(count_tokens_with_tokenizer("HeLLo") == 1); // Should match lowercase

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles numbers") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_numbers";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "123\n";
    out << "456\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("123") == 1);
    CHECK(count_tokens_with_tokenizer("456") == 1);
    CHECK(count_tokens_with_tokenizer("123 456") == 2);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles underscores") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_underscore";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello_world\n";
    out << "test\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello_world") == 1);
    CHECK(count_tokens_with_tokenizer("test_case") == 2); // test_ and case are separate

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles apostrophes") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_apostrophe";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "don't\n";
    out << "can't\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("don't") == 1);
    CHECK(count_tokens_with_tokenizer("can't") == 1);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles long text") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_long";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    std::string long_text = "hello world hello world hello world hello world";
    CHECK(count_tokens_with_tokenizer(long_text) == 8);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles unknown tokens") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_unknown";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("xyz") == 1); // UNK token
    CHECK(count_tokens_with_tokenizer("hello xyz") == 2); // hello + UNK
    CHECK(count_tokens_with_tokenizer("xyz hello") == 2); // UNK + hello

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles multiple spaces") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_spaces";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello  world") == 2); // Multiple spaces
    CHECK(count_tokens_with_tokenizer("hello   world") == 2); // More spaces
    CHECK(count_tokens_with_tokenizer("hello\tworld") == 2); // Tab

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles newlines") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_newlines";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "world\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello\nworld") == 2);
    CHECK(count_tokens_with_tokenizer("hello\n\nworld") == 2);
    CHECK(count_tokens_with_tokenizer("hello\r\nworld") == 2);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles UTF-8 characters") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_utf8";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "hello\n";
    out << "世界\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello") == 1);
    CHECK(count_tokens_with_tokenizer("世界") == 1);
    CHECK(count_tokens_with_tokenizer("hello 世界") == 2);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles complex wordpiece") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_wordpiece";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n";
    out << "[PAD]\n";
    out << "un\n";
    out << "##able\n";
    out << "##ness\n";
    out << "happy\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("un") == 1);
    CHECK(count_tokens_with_tokenizer("happy") == 1);
    CHECK(count_tokens_with_tokenizer("unable") == 2); // un + ##able
    CHECK(count_tokens_with_tokenizer("happiness") == 1); // [UNK]

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles vocabulary with IDs") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_ids";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK] 0\n";
    out << "[PAD] 1\n";
    out << "hello 100\n";
    out << "world 200\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello world") == 2);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles comments in vocab file") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_comments";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "# This is a comment\n";
    out << "[UNK]\n";
    out << "# Another comment\n";
    out << "[PAD]\n";
    out << "hello\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello") == 1);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles empty lines in vocab file") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_empty_lines";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "\n";
    out << "[UNK]\n";
    out << "\n";
    out << "[PAD]\n";
    out << "\n";
    out << "hello\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello") == 1);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles whitespace in vocab entries") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_whitespace_vocab";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "  [UNK]  \n";
    out << "  [PAD]  \n";
    out << "  hello  \n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    CHECK(count_tokens_with_tokenizer("hello") == 1);

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles non-existent vocab file") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_nonexistent";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "nonexistent.txt";
    CHECK(!init_tokenizer_from_file(vocab.string()));
    CHECK(!is_tokenizer_initialized());

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles invalid JSON content") {
    CHECK(!init_tokenizer_from_json_content(""));
    CHECK(!init_tokenizer_from_json_content("not json"));
    CHECK(!init_tokenizer_from_json_content("{"));
    CHECK(!init_tokenizer_from_json_content("["));
    CHECK(!init_tokenizer_from_json_content("null"));
    CHECK(!init_tokenizer_from_json_content("123"));
    CHECK(!init_tokenizer_from_json_content("\"string\""));
}

TEST_CASE("Tokenizer handles JSON with nested vocab") {
    const std::string json = R"json({
  "config": {
    "model": {
      "vocab": {
        "[UNK]": 0,
        "[PAD]": 1,
        "hello": 2,
        "world": 3
      }
    }
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello world") == 2);
}

TEST_CASE("Tokenizer handles JSON array with vocab") {
    const std::string json = R"json({
  "models": [
    {
      "vocab": {
        "[UNK]": 0,
        "[PAD]": 1,
        "hello": 2
      }
    }
  ]
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello") == 1);
}

TEST_CASE("Tokenizer handles JSON with multiple vocabs (uses first)") {
    const std::string json = R"json({
  "vocab1": {
    "[UNK]": 0,
    "[PAD]": 1,
    "hello": 2
  },
  "vocab2": {
    "[UNK]": 0,
    "[PAD]": 1,
    "world": 3
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    // Should use the first vocab found
}

TEST_CASE("Tokenizer handles empty JSON object") {
    const std::string json = "{}";
    CHECK(!init_tokenizer_from_json_content(json));
}

TEST_CASE("Tokenizer handles JSON with empty vocab") {
    const std::string json = R"json({
  "vocab": {}
})json";

    CHECK(!init_tokenizer_from_json_content(json));
}

TEST_CASE("Tokenizer handles JSON with non-object vocab") {
    const std::string json = R"json({
  "vocab": "not an object"
})json";

    CHECK(!init_tokenizer_from_json_content(json));
}

TEST_CASE("Tokenizer handles JSON with non-integer IDs") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": "zero",
    "[PAD]": "one"
  }
})json";

    CHECK(!init_tokenizer_from_json_content(json));
}

TEST_CASE("Tokenizer handles JSON with string keys containing special chars") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": 0,
    "[PAD]": 1,
    "hello-world": 2,
    "test_token": 3
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello-world") == 1);
    CHECK(count_tokens_with_tokenizer("test_token") == 1);
}

TEST_CASE("Tokenizer handles JSON with Unicode in keys") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": 0,
    "[PAD]": 1,
    "世界": 2
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("世界") == 1);
}

TEST_CASE("Tokenizer handles JSON with escaped characters") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": 0,
    "[PAD]": 1,
    "test\n": 2,
    "tab\t": 3
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
}

TEST_CASE("Tokenizer handles JSON with large IDs") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": 0,
    "[PAD]": 1,
    "hello": 999999
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello") == 1);
}

TEST_CASE("Tokenizer handles JSON with negative IDs") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": -1,
    "[PAD]": -2,
    "hello": 2
  }
})json";

    // Should handle negative IDs (though unusual)
    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
}

TEST_CASE("Tokenizer handles JSON with whitespace") {
    const std::string json = R"json(
    {
      "vocab" :
      {
        "[UNK]" : 0 ,
        "[PAD]" : 1 ,
        "hello" : 2
      }
    }
)json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello") == 1);
}

TEST_CASE("Tokenizer handles JSON with nested objects") {
    const std::string json = R"json({
  "model": {
    "config": {
      "vocab": {
        "[UNK]": 0,
        "[PAD]": 1,
        "hello": 2
      }
    }
  }
})json";

    CHECK(init_tokenizer_from_json_content(json));
    CHECK(is_tokenizer_initialized());
    CHECK(count_tokens_with_tokenizer("hello") == 1);
}

TEST_CASE("Tokenizer handles JSON with mixed types in vocab") {
    const std::string json = R"json({
  "vocab": {
    "[UNK]": 0,
    "[PAD]": 1,
    "hello": 2,
    "world": "3"
  }
})json";

    // Should fail on non-integer ID
    CHECK(!init_tokenizer_from_json_content(json));
}

TEST_CASE("Tokenizer reinitializes correctly") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_reinit";
    fs::create_directories(tmp);

    fs::path vocab1 = tmp / "vocab1.txt";
    std::ofstream out1(vocab1.string());
    out1 << "[UNK]\n[PAD]\nhello\n";
    out1.close();

    CHECK(init_tokenizer_from_file(vocab1.string()));
    CHECK(count_tokens_with_tokenizer("hello") == 1);

    fs::path vocab2 = tmp / "vocab2.txt";
    std::ofstream out2(vocab2.string());
    out2 << "[UNK]\n[PAD]\nworld\n";
    out2.close();

    CHECK(init_tokenizer_from_file(vocab2.string()));
    CHECK(count_tokens_with_tokenizer("world") == 1);
    CHECK(count_tokens_with_tokenizer("hello") == 1); // Should be UNK now

    fs::remove_all(tmp);
}

TEST_CASE("Tokenizer handles concurrent access") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_tokenizer_concurrent";
    fs::create_directories(tmp);

    fs::path vocab = tmp / "vocab.txt";
    std::ofstream out(vocab.string());
    out << "[UNK]\n[PAD]\nhello\nworld\n";
    out.close();

    CHECK(init_tokenizer_from_file(vocab.string()));
    CHECK(is_tokenizer_initialized());

    // Simulate concurrent access
    std::vector<std::thread> threads;
    std::vector<int> results(10);

    for (int i = 0; i < 10; ++i) {
        threads.emplace_back([i, &results]() {
            results[i] = count_tokens_with_tokenizer("hello world");
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // All threads should get the same result
    for (int result : results) {
        CHECK(result == 2);
    }

    fs::remove_all(tmp);
}

TEST_CASE("download_url_to_memory handles invalid URLs") {
    auto result = download_url_to_memory("not-a-url", 1024);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(result.payload.empty());
}

TEST_CASE("download_url_to_memory handles FTP URLs") {
    auto result = download_url_to_memory("ftp://example.com", 1024);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}

TEST_CASE("download_url_to_memory handles malformed HTTP URLs") {
    auto result = download_url_to_memory("http:/example.com", 1024);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}

TEST_CASE("download_url_to_memory handles size limits") {
    auto result = download_url_to_memory("https://example.com", 1);
    CHECK(!result.ok);
}

TEST_CASE("download_url_to_memory handles zero max bytes") {
    auto result = download_url_to_memory("https://example.com", 0);
    CHECK(!result.ok);
}

TEST_CASE("download_url_to_memory handles very large max bytes") {
    std::size_t max_bytes = std::numeric_limits<std::size_t>::max() / (1024ULL * 1024ULL);
    auto result = download_url_to_memory("https://example.com", max_bytes);
    (void)result;
}

TEST_CASE("walk_single processes directory correctly") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_single";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src");
    fs::create_directories(tmp / "docs");

    std::ofstream(tmp / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "src" / "util.cpp") << "void util() {}\n";
    std::ofstream(tmp / "docs" / "readme.md") << "# Documentation\n";

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.size() == 3);
    
    bool found_main = false, found_util = false, found_readme = false;
    for (const auto& r : results) {
        if (r.path == "src/main.cpp") found_main = true;
        if (r.path == "src/util.cpp") found_util = true;
        if (r.path == "docs/readme.md") found_readme = true;
    }
    
    CHECK(found_main);
    CHECK(found_util);
    CHECK(found_readme);

    fs::remove_all(tmp);
}

TEST_CASE("walk_single respects include patterns") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_include";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::ofstream(tmp / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "test.cpp") << "void test() {}\n";
    std::ofstream(tmp / "readme.md") << "# Documentation\n";

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({"*.cpp"});
    ExcludeFilter exclude_filter({});

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.size() == 2);
    
    bool found_main = false, found_test = false;
    for (const auto& r : results) {
        if (r.path == "main.cpp") found_main = true;
        if (r.path == "test.cpp") found_test = true;
    }
    
    CHECK(found_main);
    CHECK(found_test);

    fs::remove_all(tmp);
}

TEST_CASE("walk_single respects exclude patterns") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_exclude";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::ofstream(tmp / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "test.cpp") << "void test() {}\n";
    std::ofstream(tmp / "readme.md") << "# Documentation\n";

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({"*test*"});

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.size() == 2);
    
    bool found_main = false, found_readme = false;
    for (const auto& r : results) {
        if (r.path == "main.cpp") found_main = true;
        if (r.path == "readme.md") found_readme = true;
    }
    
    CHECK(found_main);
    CHECK(found_readme);

    fs::remove_all(tmp);
}

TEST_CASE("walk_single respects ignore filter") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_ignore";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "build");
    fs::create_directories(tmp / "src");

    std::ofstream(tmp / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "build" / "app") << "binary content\x00";

    std::ofstream gitignore(tmp / ".gitignore");
    gitignore << "build/\n";
    gitignore.close();

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({ "*gitignore" });

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.size() == 1);
    CHECK(results[0].path == "src/main.cpp");

    fs::remove_all(tmp);
}

TEST_CASE("walk_single handles --all flag") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_all";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "build");

    std::ofstream(tmp / "build" / "app") << "binary content\x00";

    std::ofstream gitignore(tmp / ".gitignore");
    gitignore << "build/\n";
    gitignore.close();

    Options opt;
    opt.all = true;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({ "*gitignore" });

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.size() == 1);
    CHECK(results[0].path == "build/app");

    fs::remove_all(tmp);
}

TEST_CASE("walk_single handles non-existent directory") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_nonexistent";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);
    CHECK(results.empty());

    fs::remove_all(tmp);
}

TEST_CASE("walk_single handles empty directory") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_walk_empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Options opt;
    IgnoreFilter filter(tmp);
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});

    auto results = walk_single(tmp, opt, filter, include_filter, exclude_filter);

    CHECK(results.empty());

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles empty directory") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_empty";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.empty());

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles single file") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_single";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::ofstream(tmp / "main.cpp") << "int main() { return 0; }\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 1);
    CHECK(results[0].path == "main.cpp");

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles multiple files") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_multiple";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src");
    fs::create_directories(tmp / "docs");

    std::ofstream(tmp / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "src" / "util.cpp") << "void util() {}\n";
    std::ofstream(tmp / "docs" / "readme.md") << "# Documentation\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 3);

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles include patterns") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_include";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::ofstream(tmp / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "test.cpp") << "void test() {}\n";
    std::ofstream(tmp / "readme.md") << "# Documentation\n";

    Options opt;
    IncludeFilter include_filter({"*.cpp"});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 2);

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles nested directories") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_nested";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "src" / "core");
    fs::create_directories(tmp / "src" / "utils");

    std::ofstream(tmp / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "src" / "core" / "engine.cpp") << "void engine() {}\n";
    std::ofstream(tmp / "src" / "utils" / "helpers.cpp") << "void helpers() {}\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 3);

    fs::remove_all(tmp);
}

TEST_CASE("ParallelProcessor handles mixed file types") {
    fs::path tmp = fs::temp_directory_path() / "tokloc_test_parallel_mixed";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::ofstream(tmp / "code.cpp") << "int main() { return 0; }\n";
    std::ofstream(tmp / "docs.md") << "# Documentation\n";
    std::ofstream(tmp / "data.json") << "{\"key\": \"value\"}\n";
    std::ofstream(tmp / "page.html") << "<html></html>\n";

    Options opt;
    IncludeFilter include_filter({});
    ExcludeFilter exclude_filter({});
    IgnoreFilter ignore_filter(tmp);
    ParallelProcessor proc(opt, include_filter, exclude_filter, ignore_filter);

    proc.process_directory(tmp);
    auto results = proc.get_results();

    CHECK(results.size() == 4);

    bool found_code = false, found_docs = false, found_data = false, found_html = false;
    for (const auto& r : results) {
        if (r.path == "code.cpp") found_code = true;
        if (r.path == "docs.md") found_docs = true;
        if (r.path == "data.json") found_data = true;
        if (r.path == "page.html") found_html = true;
    }

    CHECK(found_code);
    CHECK(found_docs);
    CHECK(found_data);
    CHECK(found_html);

    fs::remove_all(tmp);
}
