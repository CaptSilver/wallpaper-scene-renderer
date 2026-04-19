#include <regex>
#include <tuple>
#include <utility>
#include <vector>
#include <string>
#include <sstream>
#include <argparse/argparse.hpp>
#include <string_view>

constexpr std::string_view ARG_ASSETS      = "<assets>";
constexpr std::string_view ARG_SCENE       = "<scene>";
constexpr std::string_view OPT_VALID_LAYER = "--valid-layer";
constexpr std::string_view OPT_GRAPHVIZ    = "--graphviz";
constexpr std::string_view OPT_FPS         = "--fps";
constexpr std::string_view OPT_RESOLUTION  = "--resolution";
constexpr std::string_view OPT_CACHE_PATH  = "--cache-path";
constexpr std::string_view OPT_HDR         = "--hdr";
constexpr std::string_view OPT_USER_PROPS  = "--user-props";
constexpr std::string_view OPT_SET_PROP    = "--set";
constexpr std::string_view OPT_SCREENSHOT  = "--screenshot";
constexpr std::string_view OPT_SCREENSHOT_FRAMES = "--screenshot-frames";
// Interval capture: write a screenshot every N seconds for a bounded total
// time.  Output filename becomes `<base>_<ms>.ppm` so a sorted listing is
// chronological.  Mutually exclusive with single-shot (if both present,
// interval wins).
constexpr std::string_view OPT_SCREENSHOT_INTERVAL = "--screenshot-interval";
constexpr std::string_view OPT_SCREENSHOT_MAX_TIME = "--screenshot-max-time";
// Per-pass render-target dump — see VulkanRender::setPassDumpDir.  Writes one
// PPM per CustomShaderPass to the given directory.  Fires once on the next
// frame after scene load completes.
constexpr std::string_view OPT_DUMP_PASSES_DIR     = "--dump-passes-dir";
constexpr std::string_view OPT_DUMP_PASSES_DELAY   = "--dump-passes-delay";

struct Resolution {
	uint w;
	uint h;
};
std::ostream& operator<<(std::ostream& os, const Resolution& res) {
    return os << res.w << 'x' << res.h;
}

void setAndParseArg(argparse::ArgumentParser& arg, int argc, char** argv) {
    arg.add_argument(ARG_ASSETS).help("assets folder").nargs(1);
    arg.add_argument(ARG_SCENE).help("scene file").nargs(1);

    arg.add_argument("-f", OPT_FPS)
        .help("fps")
        .default_value<int32_t>(15)
        .nargs(1)
        .scan<'i', int32_t>();

    arg.add_argument("-V", OPT_VALID_LAYER)
        .help("enable vulkan valid layer")
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .append();

    arg.add_argument("-G", OPT_GRAPHVIZ)
        .help("generate graphviz of render graph, output to 'graph.dot'")
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .append();

    arg.add_argument("-C", OPT_CACHE_PATH)
        .help("generate graphviz of render graph, output to 'graph.dot'")
        .default_value(std::string())
        .nargs(1)
        .append();

    arg.add_argument("-H", OPT_HDR)
        .help("enable HDR content pipeline (RGBA16F render targets + tonemapping)")
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .append();

    arg.add_argument("-R", OPT_RESOLUTION)
        .help("Set the resolution, eg. 1920x1080")
        .default_value(Resolution{1280, 720})
        .implicit_value(true)
        .nargs(1)
        .append()
		.action([](const std::string& value) {
			const std::regex re_res(R"(([0-9]+)x([0-9]+))");
			std::smatch match;
			uint width = 1280, height = 720;
			if(std::regex_match(value, match, re_res)) {
				const std::string w_str = match[1].str(), h_str = match[2].str();
				std::from_chars(w_str.c_str(), w_str.c_str() + w_str.length(), width);
				std::from_chars(h_str.c_str(), h_str.c_str() + h_str.length(), height);
			}
			return Resolution{width, height};
		});

    arg.add_argument("-U", OPT_USER_PROPS)
        .help("user properties override JSON, e.g. '{\"lucyrebecca\":\"6\"}'")
        .default_value(std::string())
        .nargs(1)
        .append();

    // Repeatable --set key=value. Each pair becomes one entry in the merged
    // user-properties JSON. String vs bool vs number is sniffed from the value:
    //   --set hdrOutput=true        -> bool
    //   --set lucyrebecca=6         -> number
    //   --set accent="1 0.5 0"      -> string (WE color)
    arg.add_argument(OPT_SET_PROP)
        .help("override a user property (repeatable): --set key=value")
        .default_value<std::vector<std::string>>({})
        .append();

    arg.add_argument("--cursor")
        .help("simulate hover at widget-space x,y (repeats every 0.5s to keep hover UI visible)")
        .nargs(1);

    arg.add_argument("--click")
        .help("simulate a click at widget-space x,y 1.5s after startup")
        .nargs(1);

    arg.add_argument("-S", OPT_SCREENSHOT)
        .help("capture a PPM screenshot to the given path, then exit")
        .default_value(std::string())
        .nargs(1)
        .append();

    arg.add_argument(OPT_SCREENSHOT_FRAMES)
        .help("number of frames to render before capturing (default 30)")
        .default_value<int32_t>(30)
        .nargs(1)
        .scan<'i', int32_t>();

    arg.add_argument(OPT_SCREENSHOT_INTERVAL)
        .help("time-lapse: seconds between captures (requires --screenshot)")
        .default_value<double>(0.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument(OPT_SCREENSHOT_MAX_TIME)
        .help("time-lapse: total seconds to capture (requires --screenshot-interval)")
        .default_value<double>(0.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument(OPT_DUMP_PASSES_DIR)
        .help("debug: dump each CustomShaderPass output RT to PPM files in DIR")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument(OPT_DUMP_PASSES_DELAY)
        .help("debug: seconds to wait before the pass dump fires (default 2.0)")
        .default_value<double>(2.0)
        .nargs(1)
        .scan<'g', double>();

    try {
        arg.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << arg;
        std::exit(1);
    }
}

// Assemble the final user-properties JSON from --user-props <json>
// (verbatim if present) plus each repeated --set key=value.
// Returns {} when neither flag is provided. No external JSON dep — hand-built,
// since the values we emit are tiny and already well-formed by construction.
inline std::string BuildUserPropsJson(const argparse::ArgumentParser& arg) {
    std::string                    base = arg.get<std::string>(OPT_USER_PROPS);
    std::vector<std::string>       sets = arg.get<std::vector<std::string>>(OPT_SET_PROP);
    if (base.empty() && sets.empty()) return {};

    // Start from --user-props object content (strip outer braces if present
    // and reuse its body), so flags compose.
    std::ostringstream out;
    out << "{";
    bool first = true;

    if (!base.empty()) {
        // Trust the user JSON but strip outer braces if present.
        std::string inner = base;
        auto trim = [](std::string& s) {
            while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(0,1);
            while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        };
        trim(inner);
        if (!inner.empty() && inner.front() == '{' && inner.back() == '}') {
            inner = inner.substr(1, inner.size() - 2);
            trim(inner);
        }
        if (!inner.empty()) {
            out << inner;
            first = false;
        }
    }

    auto is_number = [](const std::string& s) {
        if (s.empty()) return false;
        char* end = nullptr;
        std::strtod(s.c_str(), &end);
        return end != s.c_str() && *end == '\0';
    };
    auto json_escape = [](const std::string& s) {
        std::string r;
        r.reserve(s.size() + 2);
        for (char c : s) {
            switch (c) {
                case '"':  r += "\\\""; break;
                case '\\': r += "\\\\"; break;
                case '\n': r += "\\n"; break;
                case '\t': r += "\\t"; break;
                default:   r += c;
            }
        }
        return r;
    };

    for (const auto& kv : sets) {
        auto eq = kv.find('=');
        if (eq == std::string::npos) {
            std::cerr << "skipping malformed --set (expected key=value): " << kv << std::endl;
            continue;
        }
        std::string k = kv.substr(0, eq);
        std::string v = kv.substr(eq + 1);

        std::string lit;
        if (v == "true" || v == "false") {
            lit = v;
        } else if (is_number(v)) {
            lit = v;
        } else {
            lit = "\"" + json_escape(v) + "\"";
        }
        if (!first) out << ",";
        out << "\"" << json_escape(k) << "\":" << lit;
        first = false;
    }
    out << "}";
    return out.str();
}
