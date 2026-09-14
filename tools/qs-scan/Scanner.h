// qs-scan — the repository real-code gate.
//
// It walks a tree and reports work that must not ship: unfinished markers, fabricated
// implementations, swallowed failures, empty files, and directories that silently escaped the scan.
// See SYSTEM_PROMPT.md § Forbidden Patterns and § Anti-Evasion Rules.
#pragma once

#include <cstddef>
#include <filesystem>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace qscan {

enum class Severity { warn, error };

// `outside_tests` exists because SYSTEM_PROMPT.md allows explicitly named test doubles under
// tests/, while banning the same names everywhere else.
enum class Scope { everywhere, outside_tests };

struct Pattern {
    std::string id;
    Severity severity{Severity::error};
    Scope scope{Scope::everywhere};
    std::regex expression;
};

struct Hit {
    std::filesystem::path file;  // relative to the scan root
    std::size_t line{0};         // 1-based; 0 when the hit is not tied to a line
    std::string id;
    Severity severity{Severity::error};
    std::string text;
};

struct Options {
    std::filesystem::path root;
    // Paths (relative to the root) that must exist and be scanned. Guards against the gate being
    // satisfied by deleting or renaming the code it was meant to check.
    std::vector<std::filesystem::path> required;
    // Extra paths to skip, relative to the root. Recorded in the report, never silent.
    std::vector<std::filesystem::path> extra_ignores;
};

struct IgnoredEntry {
    std::filesystem::path path;
    std::string reason;
};

struct Report {
    std::vector<std::filesystem::path> scanned;
    std::vector<IgnoredEntry> ignored;
    std::vector<Hit> hits;
    // Structural problems: a required path is missing, a file cannot be read, a symlink was not
    // followed. Any of these fails the gate, because an unread tree is not a verified tree.
    std::vector<std::string> problems;
    std::size_t errors{0};
    std::size_t warnings{0};
};

// Loads patterns.txt. On failure returns false and fills `error` with a human-readable reason.
bool load_patterns(const std::filesystem::path& file,
                   std::vector<Pattern>& patterns,
                   std::string& error);

Report scan(const Options& options, const std::vector<Pattern>& patterns);

// Same as scan(), but with a per-file filter. Used by the self-tests to check that an ignored path
// is genuinely skipped rather than merely reported.
bool passes(const Report& report);

}  // namespace qscan
