#define DOCTEST_CONFIG_SUPER_FAST_ASSERTS
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <string_view>
#include <vector>
#include <string>
#include <filesystem>

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
    CHECK(density("code") == doctest::Approx(4.2));
    CHECK(density("docs") == doctest::Approx(3.6));
    CHECK(density("data") == doctest::Approx(3.3));
    CHECK(density("html") == doctest::Approx(3.7));
    CHECK(density("image") == doctest::Approx(1e9));
    CHECK(density("other") == doctest::Approx(4.0));
}

TEST_CASE("estimate_tokens") {
    CHECK(estimate_tokens(11, "code") == 2);
    CHECK(estimate_tokens(11, "docs") == 3);
    CHECK(estimate_tokens(11, "data") == 3);
    CHECK(estimate_tokens(11, "html") == 2);
    CHECK(estimate_tokens(11, "image") == 0);
}

TEST_CASE("Options") {
    Options opt1;
    CHECK(!opt1.verbose);
    CHECK(!opt1.all);
    CHECK(opt1.include_patterns.empty());
    CHECK(opt1.paths.empty());

    Options opt2;
    opt2.verbose = true;
    opt2.all = true;
    opt2.include_patterns.push_back("*.cpp");
    CHECK(opt2.verbose);
    CHECK(opt2.all);
    CHECK(opt2.include_patterns.size() == 1);
}