#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <regex>
#include <tuple>
#include <utility>
#include <vector>
#include <string>
#include <sstream>
#include <argparse/argparse.hpp>
#include <string_view>

constexpr std::string_view ARG_ASSETS              = "<assets>";
constexpr std::string_view ARG_SCENE               = "<scene>";
constexpr std::string_view OPT_VALID_LAYER         = "--valid-layer";
constexpr std::string_view OPT_GRAPHVIZ            = "--graphviz";
constexpr std::string_view OPT_FPS                 = "--fps";
constexpr std::string_view OPT_REFRESH_MHZ         = "--refresh-mhz";
constexpr std::string_view OPT_PRESENT_MODE        = "--present-mode";
constexpr std::string_view OPT_RESOLUTION          = "--resolution";
constexpr std::string_view OPT_CACHE_PATH          = "--cache-path";
constexpr std::string_view OPT_HDR                 = "--hdr";
constexpr std::string_view OPT_POSTPROCESSING      = "--postprocessing";
constexpr std::string_view OPT_USER_PROPS          = "--user-props";
constexpr std::string_view OPT_SET_PROP            = "--set";
constexpr std::string_view OPT_SET_RUNTIME         = "--set-runtime";
constexpr std::string_view OPT_SET_RUNTIME_DELAY   = "--set-runtime-delay";
constexpr std::string_view OPT_SET_RUNTIME_AT      = "--set-runtime-at";
constexpr std::string_view OPT_SHOT_AFTER_SET      = "--shot-after-set";
constexpr std::string_view OPT_SCREENSHOT          = "--screenshot";
constexpr std::string_view OPT_SCREENSHOT_FRAMES   = "--screenshot-frames";
constexpr std::string_view OPT_SCREENSHOT_AT_FRAME = "--screenshot-at-frame";
constexpr std::string_view OPT_DETERMINISTIC       = "--deterministic";
// Interval capture: write a screenshot every N seconds for a bounded total
// time.  Output filename becomes `<base>_<ms>.ppm` so a sorted listing is
// chronological.  Mutually exclusive with single-shot (if both present,
// interval wins).
constexpr std::string_view OPT_SCREENSHOT_INTERVAL = "--screenshot-interval";
constexpr std::string_view OPT_SCREENSHOT_MAX_TIME = "--screenshot-max-time";
// Per-pass render-target dump — see VulkanRender::setPassDumpDir.  Writes one
// PPM per CustomShaderPass to the given directory.  Fires once on the next
// frame after scene load completes.
constexpr std::string_view OPT_DUMP_PASSES_DIR   = "--dump-passes-dir";
constexpr std::string_view OPT_DUMP_PASSES_DELAY = "--dump-passes-delay";

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
        .default_value<int32_t>(60)
        .nargs(1)
        .scan<'i', int32_t>();

    // Display rate in millihertz (59.94Hz -> 59940).  0 = auto-detect from the
    // screen.  An explicit value lets a 144Hz desk exercise the 59.94Hz snap
    // path, which is the case the fractional plumbing exists for.
    arg.add_argument(OPT_REFRESH_MHZ)
        .help("display refresh in millihertz (0 = detect from screen)")
        .default_value<int32_t>(0)
        .nargs(1)
        .scan<'i', int32_t>();

    // 0=Auto 1=Fifo 2=FifoRelaxed 3=Mailbox 4=Immediate, matching
    // PresentModePolicy.  Only the GLFW viewer owns a swapchain to apply it to.
    arg.add_argument(OPT_PRESENT_MODE)
        .help("swapchain present mode (0=Auto 1=Fifo 2=FifoRelaxed 3=Mailbox 4=Immediate)")
        .default_value<int32_t>(0)
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
        .help("directory for the SPIR-V shader cache (defaults to in-memory; "
              "persisting it across runs avoids re-compiling shaders)")
        .default_value(std::string())
        .nargs(1)
        .append();

    arg.add_argument("-H", OPT_HDR)
        .help("enable HDR content pipeline (RGBA16F render targets + tonemapping)")
        .default_value(false)
        .implicit_value(true)
        .nargs(0)
        .append();

    arg.add_argument(OPT_POSTPROCESSING)
        .help("override scene postprocessing variant: 'low' / 'medium' / 'ultra' / "
              "'displayhdr'.  'ultra' enables the HDR mip-chain bloom pipeline "
              "on hdr+bloom scenes that don't declare a postprocessing field")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("-R", OPT_RESOLUTION)
        .help("Set the resolution, eg. 1920x1080")
        .default_value(Resolution { 1280, 720 })
        .implicit_value(true)
        .nargs(1)
        .append()
        .action([](const std::string& value) {
            const std::regex re_res(R"(([0-9]+)x([0-9]+))");
            std::smatch      match;
            uint             width = 1280, height = 720;
            if (std::regex_match(value, match, re_res)) {
                const std::string w_str = match[1].str(), h_str = match[2].str();
                std::from_chars(w_str.c_str(), w_str.c_str() + w_str.length(), width);
                std::from_chars(h_str.c_str(), h_str.c_str() + h_str.length(), height);
            }
            return Resolution { width, height };
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

    // Repeatable --set-runtime key=value: same typing as --set, but applied
    // AFTER the scene has loaded (via a QTimer in qmlviewer.cpp), so it
    // exercises MainHandler::applyUserPropsRuntime's instant-toggle path
    // instead of the parse-time defaults --set/--user-props feed.
    arg.add_argument(OPT_SET_RUNTIME)
        .help("(sceneviewer-script only) change a user property AFTER load "
              "(repeatable): --set-runtime key=value")
        .default_value<std::vector<std::string>>({})
        .append();

    arg.add_argument(OPT_SET_RUNTIME_DELAY)
        .help("(sceneviewer-script only) seconds to wait before applying "
              "--set-runtime (default 2.0)")
        .default_value<double>(2.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument(OPT_SET_RUNTIME_AT)
        .help("(sceneviewer-script only) change a user property at a given time after launch "
              "(repeatable): --set-runtime-at SEC:key=value (SEC from 0 to 86400)")
        .default_value<std::vector<std::string>>({})
        .append();
    arg.add_argument(OPT_SHOT_AFTER_SET)
        .help("(sceneviewer-script only) with --set-runtime-at: capture the 8 frames after each "
              "change and the 60th into DIR as <key>=<value>_f<frame>.ppm")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("--cursor")
        .help("simulate hover at widget-space x,y (repeats every 0.5s to keep hover UI visible)")
        .nargs(1);

    arg.add_argument("--click")
        .help("simulate a click at widget-space x,y after --click-delay seconds")
        .nargs(1);

    arg.add_argument("--drag")
        .help("simulate press→move→release: 'x1,y1:x2,y2' (widget-space, after --click-delay)")
        .nargs(1);

    arg.add_argument("--click-delay")
        .help("seconds to wait before firing --click / --drag (default 1.5)")
        .default_value<double>(1.5)
        .nargs(1)
        .scan<'g', double>();

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

    arg.add_argument(OPT_SCREENSHOT_AT_FRAME)
        .help("capture EXACTLY at this monotonic frame index (race-free, decided "
              "on the render thread) instead of the wall-clock --screenshot-frames "
              "sleep; pair with --deterministic for byte-reproducible captures. "
              "0 (default) keeps the wall-clock behaviour.")
        .default_value<int32_t>(0)
        .nargs(1)
        .scan<'i', int32_t>();

    arg.add_argument(OPT_DETERMINISTIC)
        .help("deterministic render mode: fixed per-frame dt + seeded particle "
              "RNG, so a headless capture is reproducible run-to-run on this "
              "machine (the render oracle's warm==cold prerequisite)")
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

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

    arg.add_argument("--hide-pattern")
        .help("debug: hide any scene object whose name contains any of the "
              "comma-separated substrings (e.g. 'hair' or 'hair c1,hair top')")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("--js-eval")
        .help("(sceneviewer-script only) run arbitrary JS in the SceneScript "
              "QJSEngine once at tick 1 — e.g. 'shared.rst=1' to force a 3body "
              "universe reset, or any state machine transition normally gated "
              "on user input. Silent no-op in the GLFW sceneviewer (no QJSEngine).")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("--js-eval-delay")
        .help("(sceneviewer-script only) seconds to wait before running --js-eval (default 2.0)")
        .default_value<double>(2.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument("--media-thumbnail")
        .help("(sceneviewer-script only) comma-separated album-art images fed to "
              "mediaThumbnailChanged one at a time, as if MPRIS reported a track "
              "change; 'none' sends a track with no art. Fires from --media-delay "
              "on, every --media-interval seconds.")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("--media-properties")
        .help("(sceneviewer-script only) 'title|artist|album' sent to mediaPropertiesChanged "
              "at --media-delay, as if a music player reported the track")
        .default_value(std::string())
        .nargs(1);

    arg.add_argument("--media-delay")
        .help("(sceneviewer-script only) seconds before the first --media-thumbnail entry or the "
              "--media-properties event (default 2)")
        .default_value<double>(2.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument("--media-interval")
        .help("(sceneviewer-script only) seconds between --media-thumbnail entries (default 3)")
        .default_value<double>(3.0)
        .nargs(1)
        .scan<'g', double>();

    arg.add_argument("--list-env-vars")
        .help("print the WEKDE_* diagnostic environment variables and exit")
        .default_value(false)
        .implicit_value(true)
        .nargs(0);

    try {
        arg.parse_args(argc, argv);
    } catch (const std::runtime_error& err) {
        std::cerr << err.what() << std::endl;
        std::cerr << arg;
        std::exit(1);
    }

    if (arg.get<bool>("--list-env-vars")) {
        std::cout
            << "WEKDE_* diagnostic environment variables:\n"
            << "  WEKDE_PIPELINE_DIAG=1            per-pass Vulkan render-graph dump on reload\n"
            << "  WEKDE_TIME_DIAG=1                per-frame CPU timing of major stages\n"
            << "  WEKDE_SCRIPT_DIAG=1              verbose SceneScript JS engine logging\n"
            << "  WEKDE_DEBUG_PARTICLE=1           particle-system per-emitter trace\n"
            << "  WEKDE_TEXT_DUMP_DIR=<dir>        dump rasterized text glyph atlas to <dir>\n"
            << "  WEKDE_PASS_DUMP_MEM_MB=<n>       cap --dump-passes-dir memory at <n> MB\n"
            << "  WEKDE_SKIP_PARTICLE_CHILD_SUBSTR=<s>  skip particles whose name contains <s>\n"
            << "  WEKDE_DISABLE_OVERBRIGHT_HDR=1   disable overbright clamp in HDR "
               "post-processing\n";
        std::exit(0);
    }
}

// Merge `base` (raw JSON object body, braces optional) with `sets` (repeatable
// "key=value", auto-typed as bool/number/string) into one JSON object string.
// Pure -- no argparse dependency, so it's directly unit-testable. No external
// JSON dep either — hand-built, since the values we emit are tiny and already
// well-formed by construction.
inline std::string MergeUserPropsJson(const std::string&              base,
                                      const std::vector<std::string>& sets) {
    // Start from --user-props object content (strip outer braces if present
    // and reuse its body), so flags compose.
    std::ostringstream out;
    out << "{";
    bool first = true;

    if (! base.empty()) {
        // Trust the user JSON but strip outer braces if present.
        std::string inner = base;
        auto        trim  = [](std::string& s) {
            while (! s.empty() && std::isspace((unsigned char)s.front())) s.erase(0, 1);
            while (! s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
        };
        trim(inner);
        if (! inner.empty() && inner.front() == '{' && inner.back() == '}') {
            inner = inner.substr(1, inner.size() - 2);
            trim(inner);
        }
        if (! inner.empty()) {
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
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\t': r += "\\t"; break;
            default: r += c;
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
        if (! first) out << ",";
        out << "\"" << json_escape(k) << "\":" << lit;
        first = false;
    }
    out << "}";
    return out.str();
}

// Assemble the final user-properties JSON from --user-props <json>
// (verbatim if present) plus each repeated --set key=value.
// Returns {} when neither flag is provided.
inline std::string BuildUserPropsJson(const argparse::ArgumentParser& arg) {
    std::string              base = arg.get<std::string>(OPT_USER_PROPS);
    std::vector<std::string> sets = arg.get<std::vector<std::string>>(OPT_SET_PROP);
    if (base.empty() && sets.empty()) return {};
    return MergeUserPropsJson(base, sets);
}

// Delta-only JSON for a post-load property change: applyUserPropsRuntime only
// checks props.contains(propName) for each binding, so the changed key(s) alone
// are sufficient -- no need to re-merge the full initial --user-props/--set state.
inline std::string BuildRuntimeUserPropsJson(const std::vector<std::string>& sets) {
    if (sets.empty()) return {};
    return MergeUserPropsJson({}, sets);
}

struct RuntimeSetAt {
    double      delay_s;
    std::string set; // key=value, typed like --set
};

inline std::optional<RuntimeSetAt> ParseRuntimeSetAt(const std::string& spec) {
    auto colon = spec.find(':');
    if (colon == std::string::npos || colon + 1 >= spec.size()) return std::nullopt;
    char*      end   = nullptr;
    const auto delay = std::strtod(spec.c_str(), &end);
    if (end != spec.c_str() + colon) return std::nullopt;
    // The caller turns this into a millisecond int for QTimer, so nan, inf and
    // huge values would overflow it; negatives are refused rather than guessed at.
    if (! std::isfinite(delay) || delay < 0.0 || delay > 86400.0) return std::nullopt;
    return RuntimeSetAt { delay, spec.substr(colon + 1) };
}

// The change lands on the render thread one to three frames after the main
// thread sets the property, so arm a burst and read the first frame that
// differs; the last entry is the steady state well after it.
inline std::vector<uint64_t> CaptureBurstFrames(uint64_t base) {
    std::vector<uint64_t> frames;
    for (uint64_t i = 1; i <= 8; ++i) frames.push_back(base + i);
    frames.push_back(base + 60);
    return frames;
}

inline std::string ShotAfterSetPath(const std::string& dir, const std::string& set,
                                    uint64_t frame) {
    std::string name = set;
    for (char& c : name)
        if (c == '/' || c == '\\') c = '_';
    char suffix[32];
    std::snprintf(suffix, sizeof(suffix), "_f%06llu.ppm", (unsigned long long)frame);
    return dir + "/" + name + suffix;
}
