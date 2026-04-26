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
- Parallel processing using all CPU cores (configurable with -j)
- Binary file detection and early skipping
- Multiple path support (directories and files)
- Include/exclude pattern filtering
- Accurate token counting with optional tokenizer (supports HuggingFace vocab files)

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
| `-x`, `--exclude` | Exclude files matching pattern (e.g., `*test*`, `*/tmp/*`) |
| `-j`, `--jobs` | Number of parallel jobs (default: auto, -j1 = single threaded) |
| `--tokenizer-path` | Load tokenizer vocabulary from local file |
| `--tokenizer-url` | Load tokenizer from remote JSON URL (in-memory) |
| `--tokenizer-url-max-mb` | Max download size for `--tokenizer-url` (default `100`) |

### Tokenizer Sources

Use a local tokenizer vocab file:

```bash
./build/tokloc . --tokenizer-path /path/to/vocab.txt
```

Use a remote tokenizer JSON URL (downloaded into memory, not written to disk):

```bash
./build/tokloc . --tokenizer-url https://example.com/tokenizer.json
```

Override the default max URL download size (100 MB):

```bash
./build/tokloc . --tokenizer-url https://example.com/tokenizer.json --tokenizer-url-max-mb 250
```

Notes:
- `--tokenizer-path` and `--tokenizer-url` are mutually exclusive.
- URL tokenizer content must be valid JSON; non-JSON payloads are rejected.

### Include Patterns

Include only specific file types:

```bash
./build/tokloc . -i "*.cpp"
./build/tokloc . -i "*.cpp,*.h"
./build/tokloc src/ -i "*.cpp" -i "*.h"
```

### Exclude Patterns

Exclude specific file types or directories:

```bash
./build/tokloc . -x "*test*"              # Exclude test files
./build/tokloc . -x "*/tmp/*"             # Exclude tmp directories
./build/tokloc . -i "*.cpp" -x "*test*"   # Include cpp files but exclude test files
./build/tokloc . -x "*/node_modules/*" -x "*/build/*"  # Exclude multiple patterns
```

### Verbose Mode

```bash
./build/tokloc . --verbose
```

### Example Output

```
Type        Files   Lines     Empty     Tokens      
-------------------------------------------------------
code        3       2923      633       28373       
docs        2       198       73        1663        
other       1       3         0         7           
-------------------------------------------------------
Total       6       3124      706       30043       
Elapsed: 0.00s | files/s: 2515.63 | lines/s: 1605813.46 | tok/s: 12596202.06
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

- Uses parallel processing to utilize all CPU cores (configurable with `-j`)
- Binary files are detected and skipped early to save memory
- Streaming file processing avoids loading entire files into memory
- Large files (>10MB) are automatically chunked and processed in parallel
- All chunks are processed regardless of thread count (no truncation)
- Overlap handling preserves token boundaries at chunk edges
- Fast JSON parsing with yyjson for tokenizer vocab files
- Trie-based prefix matching for efficient tokenization

---

## Running Tests

```bash
cd build
./tests # recommended
# or
ctest -V
```

---

## Contributing

Contributions are welcome! Open an issue or submit a pull request.