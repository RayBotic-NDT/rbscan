// rbscan command line tool.
//
//   rbscan info <scan> [--json] [--attributes]
//   rbscan validate <scan> [--json] [--no-values] [--time-tolerance <s>]
//   rbscan resolve-geometry <scan> [--acquisition <name>] [--source <node>]
//          [--detector <node>] [--world <frame>] [--source-frame <frame>]
//          [--detector-frame <frame>] [--no-matrix] [--overwrite]
//   rbscan rebuild <scan>
//   rbscan schemas [<id>]

#include <cstdio>
#include <cstdlib>
#include <raybotic/scan/rbscan.hpp>
#include <string>
#include <vector>

namespace rs = raybotic::scan;

namespace {

constexpr const char* usage = R"(usage: rbscan <command> [options]

commands:
  info <scan> [--json] [--attributes]
      overview: nodes, series, samples, time span, rate, size on disk
  validate <scan> [--json] [--no-values] [--time-tolerance <s>]
      checks the scan against its schemas and links; exit code 1 on errors
  resolve-geometry <scan> [--acquisition <name>] [--source <node>] [--detector <node>]
                   [--world <frame>] [--source-frame <frame>] [--detector-frame <frame>]
                   [--no-matrix] [--overwrite]
      writes acquisitions/<name>/projections from frames, detector and source
  rebuild <scan>
      relinks all segments into the master and marks the scan complete
  schemas [<id>]
      lists the registered schemas, or prints one as JSON
)";

struct Args {
    std::vector<std::string> positional;
    std::vector<std::string> flags;
    std::vector<std::pair<std::string, std::string>> values;

    bool has(const std::string& flag) const {
        for (const auto& f : flags) {
            if (f == flag) return true;
        }
        return false;
    }
    std::string value(const std::string& key, const std::string& fallback = {}) const {
        for (const auto& [k, v] : values) {
            if (k == key) return v;
        }
        return fallback;
    }
};

Args parse(int argc, char** argv, const std::vector<std::string>& with_value) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg.rfind("--", 0) != 0) {
            a.positional.push_back(arg);
            continue;
        }
        bool takes_value = false;
        for (const auto& k : with_value) takes_value = takes_value || k == arg;
        if (takes_value) {
            if (i + 1 >= argc) throw rs::Error(arg + " needs a value");
            a.values.emplace_back(arg, argv[++i]);
        } else {
            a.flags.push_back(arg);
        }
    }
    return a;
}

const std::string& scan_arg(const Args& a) {
    if (a.positional.empty()) throw rs::Error("the scan directory is missing");
    return a.positional.front();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h") {
        std::fputs(usage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    try {
        if (command == "info") {
            const Args a = parse(argc, argv, {});
            const auto summary = rs::summarize(scan_arg(a));
            std::fputs((a.has("--json") ? rs::to_json(summary) + "\n"
                                        : rs::to_text(summary, a.has("--attributes")))
                           .c_str(),
                       stdout);
            return 0;
        }
        if (command == "validate") {
            const Args a = parse(argc, argv, {"--time-tolerance"});
            rs::ValidateOptions o;
            o.check_values = !a.has("--no-values");
            if (auto t = a.value("--time-tolerance"); !t.empty())
                o.time_tolerance_s = std::stod(t);
            const auto report = rs::validate(scan_arg(a), o);
            std::fputs(
                (a.has("--json") ? rs::to_json(report) + "\n" : rs::to_text(report))
                    .c_str(),
                stdout);
            return report.ok() ? 0 : 1;
        }
        if (command == "resolve-geometry") {
            const Args a = parse(argc, argv,
                                 {"--acquisition", "--source", "--detector", "--world",
                                  "--source-frame", "--detector-frame"});
            rs::GeometryOptions o;
            o.acquisition = a.value("--acquisition", o.acquisition);
            o.source = a.value("--source");
            o.detector = a.value("--detector");
            o.world_frame = a.value("--world", o.world_frame);
            o.source_frame = a.value("--source-frame");
            o.detector_frame = a.value("--detector-frame");
            o.projection_matrix = !a.has("--no-matrix");
            o.overwrite = a.has("--overwrite");
            const auto r = rs::resolve_geometry(scan_arg(a), o);
            std::printf(
                "%s/projections: %llu projections, %llu interpolated, %llu invalid "
                "(outside the recorded frames), %llu robot unreachable, %llu emission "
                "unstable\n",
                r.acquisition.c_str(), static_cast<unsigned long long>(r.projections),
                static_cast<unsigned long long>(r.interpolated),
                static_cast<unsigned long long>(r.extrapolated),
                static_cast<unsigned long long>(r.robot_unreachable),
                static_cast<unsigned long long>(r.emission_unstable));
            return 0;
        }
        if (command == "rebuild") {
            const Args a = parse(argc, argv, {});
            rs::ScanWriter::rebuild(scan_arg(a));
            std::printf("relinked %s\n", scan_arg(a).c_str());
            return 0;
        }
        if (command == "schemas") {
            const Args a = parse(argc, argv, {});
            if (a.positional.empty()) {
                for (const auto& id : rs::SchemaRegistry::global().ids()) {
                    const auto& s = rs::SchemaRegistry::global().get(id);
                    std::printf("%-28s %-13s %s\n", id.c_str(), s.category.c_str(),
                                s.abstract ? "(abstract)" : "");
                }
            } else {
                std::printf(
                    "%s\n",
                    rs::SchemaRegistry::global().get(a.positional[0]).to_json().c_str());
            }
            return 0;
        }
        std::fprintf(stderr, "unknown command '%s'\n\n%s", command.c_str(), usage);
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
