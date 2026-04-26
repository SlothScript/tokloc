#include <string>
#include <vector>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <memory>
#include <limits>
#include <array>
#include <functional>
#include <yyjson.h>


struct TrieNode {
    int id = -1;
    std::array<TrieNode*, 128> children{};
    ~TrieNode() {
        for (auto* child : children) {
            delete child;
        }
    }
};

class Tokenizer {
private:
    struct BasicToken {
        std::string text;
        bool is_word;
    };

    std::unordered_map<std::string, int> vocab_;
    std::unordered_map<int, std::string> id_to_token_;
    std::unordered_map<std::string, int> suffix_vocab_;
    std::string unk_token_ = "[UNK]";
    std::string pad_token_ = "[PAD]";
    int unk_token_id_ = 0;
    int pad_token_id_ = 1;
    mutable std::mutex mutex_;
    mutable std::mutex cache_mutex_;
    bool is_loaded_ = false;
    mutable std::unordered_map<std::string, std::vector<int>> wp_cache_;
    mutable std::unordered_map<std::string, int> exact_cache_;
    TrieNode* trie_root_ = nullptr;

    bool load_from_entries(const std::vector<std::pair<std::string, int>>& entries) {
        vocab_.clear();
        id_to_token_.clear();
        suffix_vocab_.clear();

        vocab_[unk_token_] = unk_token_id_;
        vocab_[pad_token_] = pad_token_id_;
        id_to_token_[unk_token_id_] = unk_token_;
        id_to_token_[pad_token_id_] = pad_token_;

        int next_id = 2;
        for (const auto& [token_raw, parsed_id] : entries) {
            std::string token = token_raw;
            int id = parsed_id;

            size_t token_start = token.find_first_not_of(" \t\n\r");
            if (token_start == std::string::npos) {
                continue;
            }
            token.erase(0, token_start);

            size_t token_end = token.find_last_not_of(" \t\n\r");
            if (token_end != std::string::npos) {
                token.erase(token_end + 1);
            }

            if (token.empty()) {
                continue;
            }

            if (token == unk_token_) {
                id = unk_token_id_;
            } else if (token == pad_token_) {
                id = pad_token_id_;
            } else {
                id = next_id++;
            }

            vocab_[token] = id;
            id_to_token_[id] = token;
            
            if (token.size() >= 2 && token[0] == '#' && token[1] == '#') {
                suffix_vocab_[token] = id;
            }
            
            if (!token.empty()) {
                if (!trie_root_) {
                    trie_root_ = new TrieNode();
                }
                TrieNode* node = trie_root_;
                for (unsigned char ch : token) {
                    if (ch >= 128) continue;
                    if (!node->children[ch]) {
                        node->children[ch] = new TrieNode();
                    }
                    node = node->children[ch];
                }
                node->id = id;
            }
        }

        is_loaded_ = true;
        return true;
    }

    static bool is_word_char(unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '\'' || c >= 0x80;
    }

    static std::vector<BasicToken> basic_tokenize(const std::string& text) {
        std::vector<BasicToken> out;
        size_t i = 0;
        while (i < text.size()) {
            unsigned char c = static_cast<unsigned char>(text[i]);
            if (std::isspace(c)) {
                ++i;
                continue;
            }

            size_t start = i;
            bool is_word = is_word_char(c);
            if (is_word) {
                while (i < text.size()) {
                    unsigned char d = static_cast<unsigned char>(text[i]);
                    if (!is_word_char(d)) break;
                    ++i;
                }
            } else {
                ++i;
            }

            out.push_back(BasicToken{text.substr(start, i - start), is_word});
        }
        return out;
    }

    inline int vocab_lookup(const std::string& token) const {
        auto it = vocab_.find(token);
        if (it != vocab_.end()) return it->second;
        return -1;
    }

    bool lookup_hash(const std::string& token) const {
        return vocab_.find(token) != vocab_.end();
    }

    inline int suffix_lookup(const std::string& token) const {
        if (!suffix_vocab_.empty()) {
            auto it = suffix_vocab_.find(token);
            if (it != suffix_vocab_.end()) return it->second;
        }
        auto it = vocab_.find(token);
        if (it != vocab_.end()) return it->second;
        return -1;
    }
    
    int trie_longest_prefix(const std::string& word) const {
        if (!trie_root_) return -1;
        TrieNode* node = trie_root_;
        int best_id = -1;
        
        for (size_t i = 0; i < word.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(word[i]);
            if (ch >= 128 || !node->children[ch]) break;
            node = node->children[ch];
            if (node->id >= 0) {
                best_id = node->id;
            }
        }
        return best_id;
    }

    std::vector<int> trie_encode_recursive(const std::string& word) const {
        std::vector<int> result;
        if (word.empty()) return result;
        
        TrieNode* node = trie_root_;
        size_t start = 0;
        
        while (start < word.size()) {
            node = trie_root_;
            int best_id = -1;
            size_t best_len = 0;
            size_t pos = start;
            
            while (pos < word.size()) {
                unsigned char ch = static_cast<unsigned char>(word[pos]);
                if (ch >= 128 || !node->children[ch]) break;
                node = node->children[ch];
                pos++;
                
                if (node->id >= 0) {
                    best_id = node->id;
                    best_len = pos - start;
                }
            }
            
            if (best_id < 0) {
                if (start == 0) {
                    result.push_back(unk_token_id_);
                }
                break;
            }
            
            result.push_back(best_id);
            start += best_len;
        }
        
        return result;
    }
    
    int trie_longest_prefix_suffix(const std::string& word) const {
        if (!trie_root_) return -1;
        TrieNode* node = trie_root_;
        int best_id = -1;
        
        for (size_t i = 0; i < word.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(word[i]);
            if (ch >= 128 || !node->children[ch]) break;
            node = node->children[ch];
            if (node->id >= 0) {
                best_id = node->id;
            }
        }
        return best_id;
    }

    std::vector<int> wordpiece_encode(const std::string& word) const {
        if (word.empty()) return {unk_token_id_};

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = wp_cache_.find(word);
            if (it != wp_cache_.end()) {
                return it->second;
            }
        }
        
        // Use trie to find longest continuous prefix
        TrieNode* node = trie_root_;
        size_t prefix_len = 0;
        int prefix_id = -1;
        
        for (size_t i = 0; i < word.size(); ++i) {
            unsigned char ch = static_cast<unsigned char>(word[i]);
            if (ch >= 128 || !node->children[ch]) break;
            node = node->children[ch];
            if (node->id >= 0) {
                prefix_len = i + 1;
                prefix_id = node->id;
            }
        }
        
        // If full word found in trie, return it
        if (prefix_len == word.size() && prefix_id >= 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            wp_cache_[word] = {prefix_id};
            return {prefix_id};
        }
        
        // Try to find a valid prefix using vocab lookup, then look for suffix
        // This handles cases like "happiness" -> UNK("happi") + ##ness
        for (size_t split = 1; split < word.size(); ++split) {
            std::string prefix = word.substr(0, split);
            std::string rest = word.substr(split);
            
            // Only try if prefix is in vocab
            auto prefix_id_check = vocab_lookup(prefix);
            if (prefix_id_check < 0) continue;
            
            // Try to find a matching suffix (longest first)
            for (size_t suffix_len = rest.size(); suffix_len >= 1; --suffix_len) {
                std::string suffix = "##" + rest.substr(0, suffix_len);
                auto suffix_id = vocab_lookup(suffix);
                
                if (suffix_id >= 0) {
                    std::vector<int> tokens = {prefix_id_check, suffix_id};
                    
                    if (suffix_len < rest.size()) {
                        std::string remaining = rest.substr(suffix_len);
                        auto more = wordpiece_encode(remaining);
                        if (!more.empty() && more[0] != unk_token_id_) {
                            tokens.insert(tokens.end(), more.begin(), more.end());
                        }
                    }
                    
                    std::lock_guard<std::mutex> lock(cache_mutex_);
                    wp_cache_[word] = tokens;
                    return tokens;
                }
            }
        }
        
        // If no split found, try to find any valid prefix in vocab
        if (prefix_id < 0) {
            for (size_t len = word.size(); len >= 1; --len) {
                std::string prefix = word.substr(0, len);
                auto lookup_id = vocab_lookup(prefix);
                if (lookup_id >= 0) {
                    prefix_id = lookup_id;
                    prefix_len = len;
                    break;
                }
            }
        }
        
        // If still no prefix found, return UNK
        if (prefix_id < 0) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            wp_cache_[word] = {unk_token_id_};
            return {unk_token_id_};
        }
        
        // If full word found in vocab, return it
        if (prefix_len == word.size()) {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            wp_cache_[word] = {prefix_id};
            return {prefix_id};
        }
        
        // Try to find suffix for the remainder
        std::string rest = word.substr(prefix_len);
        
        for (size_t suffix_len = rest.size(); suffix_len >= 1; --suffix_len) {
            std::string suffix = "##" + rest.substr(0, suffix_len);
            auto suffix_id = vocab_lookup(suffix);
            
            if (suffix_id >= 0) {
                std::vector<int> tokens = {prefix_id, suffix_id};
                
                if (suffix_len < rest.size()) {
                    std::string remaining = rest.substr(suffix_len);
                    auto more = wordpiece_encode(remaining);
                    if (!more.empty() && more[0] != unk_token_id_) {
                        tokens.insert(tokens.end(), more.begin(), more.end());
                    }
                }
                
                std::lock_guard<std::mutex> lock(cache_mutex_);
                wp_cache_[word] = tokens;
                return tokens;
            }
        }
        
        // No suffix found - return just the prefix
        std::lock_guard<std::mutex> lock(cache_mutex_);
        wp_cache_[word] = {prefix_id};
        return {prefix_id};
    }

    std::vector<int> encode_basic_token(const BasicToken& basic) const {
        if (basic.text.empty()) {
            return {};
        }

        int id = vocab_lookup(basic.text);
        if (id >= 0) {
            return {id};
        }

        std::string lower = basic.text;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (lower != basic.text) {
            id = vocab_lookup(lower);
            if (id >= 0) {
                return {id};
            }
        }

        if (!basic.is_word) {
            return {unk_token_id_};
        }

        if (basic.text.find('_') != std::string::npos) {
            std::vector<int> tokens;
            size_t start = 0;
            while (start < basic.text.size()) {
                size_t pos = basic.text.find('_', start);
                std::string part = (pos == std::string::npos) 
                    ? basic.text.substr(start)
                    : basic.text.substr(start, pos - start);
                
                if (!part.empty()) {
                    auto sub_pieces = wordpiece_encode(part);
                    tokens.insert(tokens.end(), sub_pieces.begin(), sub_pieces.end());
                }
                
                if (pos == std::string::npos) break;
                start = pos + 1;
            }
            if (!tokens.empty()) {
                return tokens;
            }
        }
        
        auto pieces = wordpiece_encode(basic.text);
        if (pieces.empty() || (pieces.size() == 1 && pieces[0] == unk_token_id_)) {
            pieces = wordpiece_encode(lower);
        }
        return pieces;
    }

public:
    Tokenizer() {
        vocab_[unk_token_] = unk_token_id_;
        vocab_[pad_token_] = pad_token_id_;
        id_to_token_[unk_token_id_] = unk_token_;
        id_to_token_[pad_token_id_] = pad_token_;
        trie_root_ = new TrieNode();
        {
            TrieNode* node = trie_root_;
            for (unsigned char ch : unk_token_) {
                if (ch >= 128) continue;
                if (!node->children[ch]) node->children[ch] = new TrieNode();
                node = node->children[ch];
            }
            node->id = unk_token_id_;
        }
        {
            TrieNode* node = trie_root_;
            for (unsigned char ch : pad_token_) {
                if (ch >= 128) continue;
                if (!node->children[ch]) node->children[ch] = new TrieNode();
                node = node->children[ch];
            }
            node->id = pad_token_id_;
        }
    }
    
    ~Tokenizer() {
        delete trie_root_;
    }

    bool load_from_file(const std::string& filepath) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        std::ifstream file(filepath);
        if (!file.is_open()) {
            return false;
        }

        std::string line;
        int next_id = 2;
        std::vector<std::pair<std::string, int>> entries;
        
        while (std::getline(file, line)) {
            size_t start = line.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) {
                continue;
            }
            line.erase(0, start);

            size_t end = line.find_last_not_of(" \t\n\r");
            if (end != std::string::npos) {
                line.erase(end + 1);
            }
            
            if (line.empty() || (line[0] == '#' && (line.size() == 1 || std::isspace(static_cast<unsigned char>(line[1]))))) {
                continue;
            }
            
            std::string token;
            int id = -1;
            
            size_t space_pos = line.find_last_of(" \t");
            if (space_pos != std::string::npos) {
                std::string id_str = line.substr(space_pos + 1);
                try {
                    id = std::stoi(id_str);
                    token = line.substr(0, space_pos);
                } catch (...) {
                    token = line;
                    id = next_id++;
                }
            } else {
                token = line;
                id = next_id++;
            }
            
            size_t token_start = token.find_first_not_of(" \t\n\r");
            if (token_start == std::string::npos) {
                continue;
            }
            token.erase(0, token_start);

            size_t token_end = token.find_last_not_of(" \t\n\r");
            if (token_end != std::string::npos) {
                token.erase(token_end + 1);
            }
            
            if (!token.empty()) {
                entries.emplace_back(token, id);
            }
        }
        
        file.close();
        return load_from_entries(entries);
    }

    bool load_from_json_vocab(const std::vector<std::pair<std::string, int>>& entries) {
        std::lock_guard<std::mutex> lock(mutex_);
        return load_from_entries(entries);
    }

    std::vector<int> encode(const std::string& text) const {
        if (!is_loaded_) {
            return {unk_token_id_};
        }

        std::vector<int> tokens;

        for (const auto& basic : basic_tokenize(text)) {
            auto encoded = encode_basic_token(basic);
            tokens.insert(tokens.end(), encoded.begin(), encoded.end());
        }
        
        return tokens;
    }

    int encode_count(const std::string& text) const {
        auto tokens = encode(text);
        return static_cast<int>(tokens.size());
    }

    bool is_loaded() const {
        return is_loaded_;
    }
};

static void json_skip_ws(const std::string& s, size_t& i) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
}

static bool json_parse_string(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    out.clear();

    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') return true;
        if (c == '\\') {
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    // Keep Unicode escapes encoded so tokens round-trip safely.
                    out.append("\\u");
                    out.append(s.substr(i, 4));
                    i += 4;
                    break;
                }
                default:
                    return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

static bool json_skip_value(const std::string& s, size_t& i);

static bool json_skip_compound(const std::string& s, size_t& i, char open, char close) {
    if (i >= s.size() || s[i] != open) return false;
    ++i;
    json_skip_ws(s, i);
    if (i < s.size() && s[i] == close) {
        ++i;
        return true;
    }

    while (i < s.size()) {
        if (!json_skip_value(s, i)) return false;
        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            json_skip_ws(s, i);
            continue;
        }
        if (i < s.size() && s[i] == close) {
            ++i;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_skip_object(const std::string& s, size_t& i) {
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    json_skip_ws(s, i);
    if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
    }

    while (i < s.size()) {
        std::string key;
        if (!json_parse_string(s, i, key)) return false;
        json_skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        json_skip_ws(s, i);
        if (!json_skip_value(s, i)) return false;
        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            json_skip_ws(s, i);
            continue;
        }
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_skip_number(const std::string& s, size_t& i) {
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    bool had_digit = false;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        had_digit = true;
        ++i;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            had_digit = true;
            ++i;
        }
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            had_digit = true;
            ++i;
        }
    }
    return had_digit && i > start;
}

static bool json_skip_value(const std::string& s, size_t& i) {
    json_skip_ws(s, i);
    if (i >= s.size()) return false;
    if (s[i] == '"') {
        std::string tmp;
        return json_parse_string(s, i, tmp);
    }
    if (s[i] == '{') return json_skip_object(s, i);
    if (s[i] == '[') return json_skip_compound(s, i, '[', ']');
    if (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '-' || s[i] == '+') {
        return json_skip_number(s, i);
    }
    if (s.compare(i, 4, "true") == 0) {
        i += 4;
        return true;
    }
    if (s.compare(i, 5, "false") == 0) {
        i += 5;
        return true;
    }
    if (s.compare(i, 4, "null") == 0) {
        i += 4;
        return true;
    }
    return false;
}

static bool json_parse_integer(const std::string& s, size_t& i, int& out) {
    json_skip_ws(s, i);
    size_t start = i;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) ++i;
    if (i >= s.size() || !std::isdigit(static_cast<unsigned char>(s[i]))) {
        i = start;
        return false;
    }
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
        ++i;
    }
    try {
        long long v = std::stoll(s.substr(start, i - start));
        if (v < std::numeric_limits<int>::min() || v > std::numeric_limits<int>::max()) {
            i = start;
            return false;
        }
        out = static_cast<int>(v);
        return true;
    } catch (...) {
        i = start;
        return false;
    }
}

static bool json_parse_vocab_object(
    const std::string& s,
    size_t& i,
    std::vector<std::pair<std::string, int>>& out_entries
) {
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    json_skip_ws(s, i);
    if (i < s.size() && s[i] == '}') {
        ++i;
        return true;
    }

    while (i < s.size()) {
        std::string key;
        if (!json_parse_string(s, i, key)) return false;
        json_skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        json_skip_ws(s, i);

        int id = 0;
        if (!json_parse_integer(s, i, id)) return false;
        out_entries.emplace_back(std::move(key), id);

        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            json_skip_ws(s, i);
            continue;
        }
        if (i < s.size() && s[i] == '}') {
            ++i;
            return true;
        }
        return false;
    }
    return false;
}

static bool json_find_vocab_in_value(
    const std::string& s,
    size_t& i,
    std::vector<std::pair<std::string, int>>& entries
);

static bool json_find_vocab_in_object(
    const std::string& s,
    size_t& i,
    std::vector<std::pair<std::string, int>>& entries
) {
    if (i >= s.size() || s[i] != '{') return false;
    ++i;
    json_skip_ws(s, i);
    if (i < s.size() && s[i] == '}') {
        ++i;
        return false;
    }

    while (i < s.size()) {
        std::string key;
        if (!json_parse_string(s, i, key)) return false;
        json_skip_ws(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        json_skip_ws(s, i);

        if (key.find("vocab") == 0 && i < s.size() && s[i] == '{') {
            size_t vocab_pos = i;
            std::vector<std::pair<std::string, int>> parsed_entries;
            if (json_parse_vocab_object(s, vocab_pos, parsed_entries) && !parsed_entries.empty()) {
                i = vocab_pos;
                entries = std::move(parsed_entries);
                return true;
            }
            i = vocab_pos;
        }

        if (!json_find_vocab_in_value(s, i, entries)) {
            return false;
        }
        if (!entries.empty()) {
            return true;
        }

        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            json_skip_ws(s, i);
            continue;
        }
        if (i < s.size() && s[i] == '}') {
            ++i;
            return false;
        }
        return false;
    }

    return false;
}

static bool json_find_vocab_in_array(
    const std::string& s,
    size_t& i,
    std::vector<std::pair<std::string, int>>& entries
) {
    if (i >= s.size() || s[i] != '[') return false;
    ++i;
    json_skip_ws(s, i);
    if (i < s.size() && s[i] == ']') {
        ++i;
        return false;
    }

    while (i < s.size()) {
        if (!json_find_vocab_in_value(s, i, entries)) {
            return false;
        }
        if (!entries.empty()) {
            return true;
        }

        json_skip_ws(s, i);
        if (i < s.size() && s[i] == ',') {
            ++i;
            json_skip_ws(s, i);
            continue;
        }
        if (i < s.size() && s[i] == ']') {
            ++i;
            return false;
        }
        return false;
    }

    return false;
}

static bool json_find_vocab_in_value(
    const std::string& s,
    size_t& i,
    std::vector<std::pair<std::string, int>>& entries
) {
    json_skip_ws(s, i);
    if (i >= s.size()) return false;

    if (s[i] == '{') {
        if (json_find_vocab_in_object(s, i, entries)) return true;
        return true;
    }
    if (s[i] == '[') {
        if (json_find_vocab_in_array(s, i, entries)) return true;
        return true;
    }

    return json_skip_value(s, i);
}

static bool json_extract_vocab_fast(
    const std::string& json,
    std::vector<std::pair<std::string, int>>& entries
) {
    yyjson_doc* doc = yyjson_read(json.c_str(), json.size(), 0);
    if (!doc) return false;
    
    yyjson_val* root = yyjson_doc_get_root(doc);
    if (!root || !yyjson_is_obj(root)) {
        yyjson_doc_free(doc);
        return false;
    }
    
    bool found = false;
    
    std::function<void(yyjson_val*, bool)> search_obj = [&](yyjson_val* obj, bool is_root) {
        if (!obj || !yyjson_is_obj(obj)) return;
        
        yyjson_obj_iter iter;
        yyjson_obj_iter_init(obj, &iter);
        yyjson_val* item;
        while ((item = yyjson_obj_iter_next(&iter))) {
            const char* key = yyjson_get_str(item);
            yyjson_val* val = yyjson_obj_iter_get_val(item);
            if (!key || !val) continue;
            
            if (yyjson_is_obj(val)) {
                if (strcmp(key, "vocab") == 0) {
                    yyjson_obj_iter viter;
                    yyjson_obj_iter_init(val, &viter);
                    yyjson_val* vitem;
                    while ((vitem = yyjson_obj_iter_next(&viter))) {
                        const char* k = yyjson_get_str(vitem);
                        yyjson_val* v = yyjson_obj_iter_get_val(vitem);
                        if (k && v) {
                            if (!yyjson_is_int(v)) {
                                entries.clear();
                                return;
                            } else {
                                entries.emplace_back(std::string(k), (int)yyjson_get_int(v));
                            }
                        }
                    }
                    found = !entries.empty();
                    return;
                }
                search_obj(val, false);
                if (found) return;
            } else if (yyjson_is_arr(val)) {
                size_t arr_len = yyjson_arr_size(val);
                for (size_t i = 0; i < arr_len; ++i) {
                    yyjson_val* arr_item = yyjson_arr_get(val, i);
                    if (yyjson_is_obj(arr_item)) {
                        search_obj(arr_item, false);
                        if (found) return;
                    }
                }
            }
        }
        
        if (is_root && !found && entries.empty()) {
            yyjson_obj_iter_init(obj, &iter);
            while ((item = yyjson_obj_iter_next(&iter))) {
                const char* key = yyjson_get_str(item);
                yyjson_val* val = yyjson_obj_iter_get_val(item);
                if (key && yyjson_is_obj(val)) {
                    yyjson_obj_iter viter;
                    yyjson_obj_iter_init(val, &viter);
                    yyjson_val* vitem;
                    while ((vitem = yyjson_obj_iter_next(&viter))) {
                        const char* k = yyjson_get_str(vitem);
                        yyjson_val* v = yyjson_obj_iter_get_val(vitem);
                        if (k && v && yyjson_is_int(v)) {
                            entries.emplace_back(std::string(k), (int)yyjson_get_int(v));
                        }
                    }
                    if (!entries.empty()) {
                        found = true;
                        return;
                    }
                }
            }
        }
    };
    
    search_obj(root, true);
    
    yyjson_doc_free(doc);
    return found;
}

static std::unique_ptr<Tokenizer> g_tokenizer;
static std::mutex g_tokenizer_mutex;

bool init_tokenizer_from_file(const std::string& filepath) {
    std::lock_guard<std::mutex> lock(g_tokenizer_mutex);
    
    try {
        g_tokenizer = std::make_unique<Tokenizer>();
        if (g_tokenizer->load_from_file(filepath)) {
            return true;
        } else {
            g_tokenizer.reset();
            return false;
        }
    } catch (...) {
        g_tokenizer.reset();
        return false;
    }
}

bool init_tokenizer_from_json_content(const std::string& json_content) {
    std::lock_guard<std::mutex> lock(g_tokenizer_mutex);

    try {
        std::vector<std::pair<std::string, int>> entries;
        if (!json_extract_vocab_fast(json_content, entries) || entries.empty()) {
            g_tokenizer.reset();
            return false;
        }

        g_tokenizer = std::make_unique<Tokenizer>();
        if (g_tokenizer->load_from_json_vocab(entries)) {
            return true;
        }
        g_tokenizer.reset();
        return false;
    } catch (...) {
        g_tokenizer.reset();
        return false;
    }
}

int count_tokens_with_tokenizer(const std::string& text) {
    if (g_tokenizer && g_tokenizer->is_loaded()) {
        return g_tokenizer->encode_count(text);
    }
    
    return -1;
}

bool is_tokenizer_initialized() {
    return g_tokenizer != nullptr && g_tokenizer->is_loaded();
}
