// qs-scan — the repository real-code gate.
//
// Exit codes: 0 clean, 1 violations or structural problems, 2 usage or setup error.
#include "Scanner.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct CommandLine {
    std::filesystem::path root;
    std::filesystem::path patterns;
    std::vector<std::filesystem::path> required;
    std::vector<std::filesystem::path> ignores;
    bool list_ignored = false;
    bool quiet = false;
};

void print_usage(std::ostream& out) {
    out << "usage: qs-scan [--root DIR] [--patterns FILE] [--require PATH]... [--ignore PATH]...\n"
           "               [--list] [--quiet]\n"
           "\n"
           "Scans a source tree for work that must not ship and exits non-zero if it finds any.\n"
           "\n"
           "  --root DIR       tree to scan (default: current directory)\n"
           "  --patterns FILE  pattern table (default: <root>/tools/qs-scan/patterns.txt)\n"
           "  --require PATH   path that must exist and be scanned; repeatable\n"
           "  --ignore PATH    extra path to skip, reported in the output; repeatable\n"
           "  --list           print every skipped path with its reason\n"
           "  --quiet          print only the summary line\n";
}

bool take_value(int argc, char** argv, int& index, std::filesystem::path& target) {
    if (index + 1 >= argc) {
        std::cerr << "qs-scan: " << argv[index] << " needs a value\n";
        return false;
    }
    target = argv[++index];
    return true;
}

bool parse(int argc, char** argv, CommandLine& out) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--help" || argument == "-h") {
            print_usage(std::cout);
            std::exit(0);
        }
        if (argument == "--root") {
            if (!take_value(argc, argv, index, out.root)) {
                return false;
            }
        } else if (argument == "--patterns") {
            if (!take_value(argc, argv, index, out.patterns)) {
                return false;
            }
        } else if (argument == "--require") {
            std::filesystem::path value;
            if (!take_value(argc, argv, index, value)) {
                return false;
            }
            out.required.push_back(value);
        } else if (argument == "--ignore") {
            std::filesystem::path value;
            if (!take_value(argc, argv, index, value)) {
                return false;
            }
            out.ignores.push_back(value);
        } else if (argument == "--list") {
            out.list_ignored = true;
        } else if (argument == "--quiet") {
            out.quiet = true;
        } else {
            std::cerr << "qs-scan: unknown argument: " << argument << "\n";
            print_usage(std::cerr);
            return false;
        }
    }
    return true;
}

const char* severity_label(qscan::Severity severity) {
    return severity == qscan::Severity::error ? "error" : "warn";
}

void report_hit(const qscan::Hit& hit, std::ostream& out) {
    out << hit.file.generic_string();
    if (hit.line != 0) {
        out << ':' << hit.line;
    }
    out << ": " << severity_label(hit.severity) << ": " << hit.id;
    if (!hit.text.empty()) {
        out << ": " << hit.text;
    }
    out << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    CommandLine command;
    if (!parse(argc, argv, command)) {
        return 2;
    }

    if (command.root.empty()) {
        command.root = std::filesystem::current_path();
    }
    std::error_code error;
    command.root = std::filesystem::weakly_canonical(command.root, error);
    if (error || !std::filesystem::is_directory(command.root, error)) {
        std::cerr << "qs-scan: root is not a readable directory\n";
        return 2;
    }

    if (command.patterns.empty()) {
        command.patterns = command.root / "tools/qs-scan/patterns.txt";
    }

    std::vector<qscan::Pattern> patterns;
    std::string pattern_error;
    if (!qscan::load_patterns(command.patterns, patterns, pattern_error)) {
        std::cerr << "qs-scan: " << pattern_error << '\n';
        return 2;
    }

    qscan::Options options;
    options.root = command.root;
    options.required = command.required;
    options.extra_ignores = command.ignores;

    const qscan::Report report = qscan::scan(options, patterns);

    for (const auto& problem : report.problems) {
        std::cout << "problem: " << problem << '\n';
    }
    for (const auto& hit : report.hits) {
        report_hit(hit, std::cout);
    }
    if (command.list_ignored) {
        for (const auto& entry : report.ignored) {
            std::cout << "skipped: " << entry.path.generic_string() << ": " << entry.reason << '\n';
        }
    }
    if (!command.quiet || !qscan::passes(report)) {
        std::cout << "qs-scan: scanned " << report.scanned.size() << " files, " << report.errors
                  << " errors, " << report.warnings << " warnings, " << report.ignored.size()
                  << " skipped, " << report.problems.size() << " problems\n";
    }

    return qscan::passes(report) ? 0 : 1;
}
