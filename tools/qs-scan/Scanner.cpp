#include "Scanner.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <system_error>

namespace qscan {
namespace {

constexpr std::size_t sniff_bytes = 8192;
constexpr std::size_t reported_text_limit = 160;

// Files that legitimately contain the patterns themselves. Excluded by exact path, never by
// directory, so tooling and CI configuration stay covered.
constexpr std::array<std::string_view, 3> ignored_files{
    "SYSTEM_PROMPT.md",
    "AGENTS.md",
    "tools/qs-scan/patterns.txt",
};

// Directories that hold no authored source. Build trees are recognised separately, by a CMake cache,
// so a build directory under any other name cannot escape the scan.
constexpr std::array<std::string_view, 4> ignored_directories{
    ".git", ".hg", ".svn", "tests/fixtures",
};

constexpr std::array<std::string_view, 14> binary_extensions{
    ".png", ".jpg", ".jpeg", ".ico", ".gif", ".ttf", ".otf", ".woff", ".woff2",
    ".qsb", ".qm",  ".pdf", ".gz",  ".zip",
};

bool begins_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool matches_entry(std::string_view candidate, std::string_view entry) {
    if (candidate == entry) {
        return true;
    }
    return begins_with(candidate, entry) && candidate.size() > entry.size() &&
           candidate[entry.size()] == '/';
}

std::string join(const std::filesystem::path& path, std::string_view reason) {
    return path.generic_string() + ": " + std::string(reason);
}

bool is_binary(const std::string& content, const std::filesystem::path& path) {
    const std::string extension = path.extension().string();
    for (const auto suffix : binary_extensions) {
        if (extension == suffix) {
            return true;
        }
    }
    const std::size_t limit = std::min(content.size(), sniff_bytes);
    const auto begin = content.begin();
    const auto end = begin + static_cast<std::ptrdiff_t>(limit);
    return std::find(begin, end, '\0') != end;
}

bool has_visible_content(const std::string& content) {
    return std::any_of(content.begin(), content.end(), [](unsigned char character) {
        return std::isspace(character) == 0;
    });
}

bool is_under_tests(const std::filesystem::path& relative) {
    const auto first = relative.begin();
    return first != relative.end() && first->string() == "tests";
}

std::string shorten(std::string text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) {
        --end;
    }
    text = text.substr(begin, end - begin);
    if (text.size() > reported_text_limit) {
        text = text.substr(0, reported_text_limit) + "...";
    }
    return text;
}

class Runner {
public:
    Runner(const Options& options, const std::vector<Pattern>& patterns, Report& report)
        : options_(options), patterns_(patterns), report_(report) {}

    void run() {
        for (const auto& required : options_.required) {
            std::error_code error;
            const std::filesystem::path absolute = options_.root / required;
            if (!std::filesystem::exists(absolute, error)) {
                report_.problems.push_back(join(required, "required path is missing or was not scanned"));
                continue;
            }
            if (ignore_reason(required).has_value()) {
                report_.problems.push_back(join(required, "required path is excluded from the scan"));
            }
        }
        visit_directory({}, options_.root);
    }

private:
    // Returns the reason a path is skipped, so every exclusion ends up in the report instead of
    // disappearing silently.
    std::optional<std::string> ignore_reason(const std::filesystem::path& relative) const {
        const std::string key = relative.generic_string();
        for (const auto entry : ignored_files) {
            if (key == entry) {
                return "rule text that necessarily quotes the patterns";
            }
        }
        for (const auto& extra : options_.extra_ignores) {
            if (matches_entry(key, extra.generic_string())) {
                return "excluded on the command line";
            }
        }
        for (const auto entry : ignored_directories) {
            if (matches_entry(key, entry)) {
                return "not authored source";
            }
        }
        for (const auto& component : relative) {
            const std::string name = component.string();
            if (begins_with(name, "build") || begins_with(name, ".build") || name == "CMakeFiles") {
                return "build output";
            }
        }
        return std::nullopt;
    }

    void note_ignored(const std::filesystem::path& relative, std::string reason) {
        report_.ignored.push_back({relative, std::move(reason)});
    }

    void visit_directory(const std::filesystem::path& relative, const std::filesystem::path& absolute) {
        std::error_code error;
        std::filesystem::directory_iterator iterator(absolute, error);
        if (error) {
            report_.problems.push_back(
                {join(relative.empty() ? std::filesystem::path{"."} : relative,
                      "cannot list directory: " + error.message())});
            return;
        }

        std::vector<std::filesystem::directory_entry> entries;
        for (const auto& entry : iterator) {
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(),
                  [](const auto& left, const auto& right) { return left.path() < right.path(); });

        for (const auto& entry : entries) {
            const std::filesystem::path child_relative =
                relative.empty() ? entry.path().filename() : relative / entry.path().filename();
            const auto status = entry.symlink_status(error);
            if (error) {
                report_.problems.push_back(join(child_relative, "cannot inspect: " + error.message()));
                error.clear();
                continue;
            }
            if (std::filesystem::is_symlink(status)) {
                report_.problems.push_back(
                    join(child_relative, "symlink is not followed, so its target is not verified"));
                continue;
            }
            if (std::filesystem::is_directory(status)) {
                if (const auto reason = ignore_reason(child_relative)) {
                    note_ignored(child_relative, *reason);
                    continue;
                }
                if (std::filesystem::exists(entry.path() / "CMakeCache.txt", error)) {
                    note_ignored(child_relative, "build output (contains a CMake cache)");
                    continue;
                }
                visit_directory(child_relative, entry.path());
                continue;
            }
            if (std::filesystem::is_regular_file(status)) {
                visit_file(child_relative, entry.path());
            }
        }
    }

    void visit_file(const std::filesystem::path& relative, const std::filesystem::path& absolute) {
        if (const auto reason = ignore_reason(relative)) {
            note_ignored(relative, *reason);
            return;
        }

        std::error_code error;
        const auto size = std::filesystem::file_size(absolute, error);
        if (error) {
            report_.problems.push_back(join(relative, "cannot read size: " + error.message()));
            return;
        }
        if (size == 0) {
            report_.scanned.push_back(relative);
            report_.hits.push_back({relative, 0, "empty-file", Severity::error, "file is empty"});
            ++report_.errors;
            return;
        }

        std::ifstream stream(absolute, std::ios::binary);
        if (!stream) {
            report_.problems.push_back(join(relative, "cannot open for reading"));
            return;
        }
        const std::string content((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
        if (stream.bad()) {
            report_.problems.push_back(join(relative, "read failed part way through"));
            return;
        }

        if (is_binary(content, absolute)) {
            note_ignored(relative, "binary content");
            return;
        }

        report_.scanned.push_back(relative);

        if (!has_visible_content(content)) {
            report_.hits.push_back(
                {relative, 0, "empty-file", Severity::error, "file holds no visible content"});
            ++report_.errors;
            return;
        }

        const bool under_tests = is_under_tests(relative);
        std::istringstream lines(content);
        std::string line;
        std::size_t number = 1;
        for (; std::getline(lines, line); ++number) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            std::vector<std::string> reported;
            for (const auto& pattern : patterns_) {
                if (pattern.scope == Scope::outside_tests && under_tests) {
                    continue;
                }
                if (std::find(reported.begin(), reported.end(), pattern.id) != reported.end()) {
                    continue;
                }
                if (std::regex_search(line, pattern.expression)) {
                    reported.push_back(pattern.id);
                    report_.hits.push_back({relative, number, pattern.id, pattern.severity, shorten(line)});
                    if (pattern.severity == Severity::error) {
                        ++report_.errors;
                    } else {
                        ++report_.warnings;
                    }
                }
            }
        }
    }

    const Options& options_;
    const std::vector<Pattern>& patterns_;
    Report& report_;
};

}  // namespace

bool load_patterns(const std::filesystem::path& file,
                   std::vector<Pattern>& patterns,
                   std::string& error) {
    std::ifstream stream(file);
    if (!stream) {
        error = "cannot open pattern table: " + file.string();
        return false;
    }

    std::string line;
    std::size_t number = 0;
    while (std::getline(stream, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }

        std::vector<std::string> fields;
        std::istringstream split(line);
        std::string field;
        while (std::getline(split, field, '\t')) {
            fields.push_back(field);
        }
        if (fields.size() != 4) {
            error = file.string() + ":" + std::to_string(number) +
                    ": expected four tab separated fields (id, severity, scope, expression)";
            return false;
        }

        Pattern pattern;
        pattern.id = fields[0];
        if (fields[1] == "error") {
            pattern.severity = Severity::error;
        } else if (fields[1] == "warn") {
            pattern.severity = Severity::warn;
        } else {
            error = file.string() + ":" + std::to_string(number) +
                    ": severity must be 'error' or 'warn'";
            return false;
        }
        if (fields[2] == "all") {
            pattern.scope = Scope::everywhere;
        } else if (fields[2] == "outside-tests") {
            pattern.scope = Scope::outside_tests;
        } else {
            error = file.string() + ":" + std::to_string(number) +
                    ": scope must be 'all' or 'outside-tests'";
            return false;
        }
        try {
            pattern.expression = std::regex(fields[3], std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error& failure) {
            error = file.string() + ":" + std::to_string(number) +
                    ": invalid expression: " + failure.what();
            return false;
        }
        patterns.push_back(std::move(pattern));
    }

    if (patterns.empty()) {
        error = file.string() + ": no patterns were loaded";
        return false;
    }
    return true;
}

Report scan(const Options& options, const std::vector<Pattern>& patterns) {
    Report report;
    Runner runner(options, patterns, report);
    runner.run();
    return report;
}

bool passes(const Report& report) {
    return report.errors == 0 && report.problems.empty();
}

}  // namespace qscan
