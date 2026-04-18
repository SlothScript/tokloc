# tokloc

A fast CLI tool that counts lines of code and estimates tokens in your project.

## About

**tokloc** scans directories and gives you a breakdown of files by type, lines of code, and estimated token counts. It respects `.gitignore` by default, so you don't get node_modules or build artifacts cluttering your results.

---

## Features

- Counts lines (total, non-empty, empty)
- Estimates token counts using type-specific density ratios
- Classifies files by type (code, docs, data, html, image, other)
- Respects `.gitignore` rules
- Parallel processing using all CPU cores
- Binary file detection and early skipping
- Multiple path support (directories and files)
- Include pattern filtering

---

## Installation

### Clone the Repository

```bash
git clone https://github.com/nathanielcole/tokloc.git
cd tokloc
```

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Build with Tests

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build .
```

### Install (Optional)

```bash
cmake --install .
```

### Requirements

- C++17 compiler
- CMake 3.10+
- [CLI11](https://github.com/CLIUtils/CLI11) (fetched automatically)
- [doctest](https://github.com/onqtam/doctest) for tests

---

## Usage

### Basic Usage

```bash
./build/tokloc /path/to/your/project
```

### Multiple Paths

```bash
./build/tokloc dir1 dir2
./build/tokloc file.txt src/
./build/tokloc src/ tests/ README.md
```

### Options

| Flag | Description |
|------|-------------|
| `-v`, `--verbose` | Show scanning progress for directories |
| `-a`, `--all` | Include ignored files (.gitignore rules) |
| `-i`, `--include` | Include only files matching pattern (e.g., `*.cpp`, `*.py`) |
| `-S`, `--no-parallel` | Disable parallel processing (single-threaded) |

### Include Patterns

Include only specific file types:

```bash
./build/tokloc . -i "*.cpp"
./build/tokloc . -i "*.cpp,*.h"
./build/tokloc src/ -i "*.cpp" -i "*.h"
```

### Verbose Mode

```bash
./build/tokloc . --verbose
```

### Example Output

```
Type        Files   Lines     Empty     Tokens
-------------------------------------------------------
code        7       1779      381       15994
data        2       1949      27        37352
docs        4       388       87        5025
other       35      18547     2084      1374190
-------------------------------------------------------
Total       48      22663     2579      1432561
Elapsed: 0.12s | files/s: 410.65 | lines/s: 215952.16 | tok/s: 12255948.05
```

### File Types

| Type | Extensions |
|------|------------|
| code | .c, .cpp, .h, .hpp, .rs, .go, .py, .js, .ts |
| docs | .md, .txt |
| data | .json, .yaml, .yml, .xml |
| html | .html, .htm |
| image | .png, .jpg, .jpeg, .gif, .webp, .svg |

---

## Token Estimation

Tokens are estimated using type-specific density ratios (characters per token):

| Type | Chars/Token |
|------|-------------|
| Code | 4.2 |
| Docs | 3.6 |
| Data | 3.3 |
| HTML | 3.7 |

Images return 0 tokens.

---

## Performance

- Uses parallel processing to utilize all CPU cores
- Binary files are detected and skipped early to save memory
- Streaming file processing avoids loading entire files into memory

---

## Running Tests

```bash
cd build
./tests/tests # recommended
# or
ctest -V
```

---

## Contributing

Contributions are welcome! Open an issue or submit a pull request.