#include "SceneBackend.hpp"
#include <QtGlobal>
#include <QGuiApplication>
#include <QtQml>
#include <QtQuick/QQuickView>
#include <QTimer>
#include <QMetaObject>
#include <QDateTime>
#include <iostream>
#include <string>
#include <cstdio>
#include <csignal>
#include <atomic>
#include <filesystem>
#include <cstdlib>
#include <sys/statfs.h>
#include <linux/magic.h>
#include "arg.hpp"

// Returns true iff `p` is on a RAM-backed filesystem (tmpfs / ramfs).  Pass
// dumps default to /tmp on many systems, which IS tmpfs on Bazzite — 128 HDR
// RTs × 29 MB each is 3.7 GB of staging *plus* the PPM output, all competing
// for the same RAM pool.  Warning the user here lets them redirect to a
// disk-backed dir before we OOM.
static bool path_is_ramfs(const std::string& p) {
    struct statfs sfs;
    if (statfs(p.c_str(), &sfs) != 0) return false;
    // linux/magic.h: TMPFS_MAGIC=0x01021994, RAMFS_MAGIC=0x858458f6
    return sfs.f_type == TMPFS_MAGIC || sfs.f_type == RAMFS_MAGIC;
}

// Clean-exit signal handler: sets an atomic flag that a QTimer polls from
// the Qt event loop.  Calling QMetaObject::invokeMethod from inside a
// signal handler is not async-signal-safe, so we stay minimal here.
// Hooked for SIGINT + SIGTERM so `timeout --signal=INT` flushes the profile.
static std::atomic<bool> g_quit_requested { false };
static void              qmlviewer_sigint_handler(int) {
    if (g_quit_requested.exchange(true)) {
        // Second signal: force-exit in case the event loop is wedged.
        std::_Exit(130);
    }
}

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

    std::signal(SIGINT, qmlviewer_sigint_handler);
    std::signal(SIGTERM, qmlviewer_sigint_handler);

    // Poll the signal-flag every 100ms.  On a first flag flip we call quit();
    // wallpapers with very busy render threads (e.g. 3body) can otherwise
    // delay processing of a queued-connection quit past the timeout's grace.
    auto* quit_poll = new QTimer(&app);
    QObject::connect(quit_poll, &QTimer::timeout, [&app]() {
        if (g_quit_requested.load()) app.quit();
    });
    quit_poll->start(100);

    qmlRegisterType<scenebackend::SceneObject>("scenetest", 1, 0, "SceneViewer");
    QQuickView view;
    {
        auto [w_width, w_height] = program.get<Resolution>(OPT_RESOLUTION);
        view.setWidth(w_width);
        view.setHeight(w_height);
        view.setResizeMode(QQuickView::SizeRootObjectToView);
        view.setSource(QUrl("qrc:///pkg/main.qml"));
    }

    QObject*                   obj = view.rootObject();
    scenebackend::SceneObject* sv  = obj->findChild<scenebackend::SceneObject*>();
    if (program.get<bool>(OPT_VALID_LAYER)) sv->enableVulkanValid();
    if (program.get<bool>(OPT_GRAPHVIZ)) sv->enableGenGraphviz();
    sv->setProperty("assets", QUrl::fromLocalFile(program.get<std::string>(ARG_ASSETS).c_str()));
    sv->setProperty("source", QUrl::fromLocalFile(program.get<std::string>(ARG_SCENE).c_str()));
    sv->setProperty("fps", program.get<int32_t>(OPT_FPS));
    sv->setProperty("hdrOutput", program.get<bool>(OPT_HDR));
    // Pin the Vulkan render target to the exact -R physical pixel size.
    // Set before the first updatePaintNode (show() below triggers it) so
    // initVulkan picks these up instead of the racy item-size × dpr path.
    {
        auto [rw, rh] = program.get<Resolution>(OPT_RESOLUTION);
        sv->setProperty("renderPixelWidth", (int)rw);
        sv->setProperty("renderPixelHeight", (int)rh);
    }

    // --user-props + --set key=value overrides, applied before scripts start.
    std::string user_props = BuildUserPropsJson(program);
    if (! user_props.empty()) {
        std::cout << "user props: " << user_props << std::endl;
        sv->setProperty("userProperties", QString::fromStdString(user_props));
    }

    sv->setAcceptMouse(true);
    sv->setAcceptHover(true);

    // --dump-passes-dir: schedule a one-shot per-pass RT dump N seconds
    // after startup.  Used to pinpoint where text / colour corruption
    // first enters the effect chain on wallpapers like 2866203962.
    //
    // Default path when the flag is absent but --dump-passes-delay was set:
    // $HOME/.cache/wekde/pass-dump.  Disk-backed on typical distros (avoids
    // tmpfs RAM competition with the VMA staging pool).  We erase the dir
    // contents before arming so consecutive runs don't pile up stale PPMs.
    {
        std::string dump_dir = program.get<std::string>(OPT_DUMP_PASSES_DIR);

        // Infer whether the user asked for a dump even without naming a dir —
        // i.e., they passed a non-default --dump-passes-delay.  argparse has
        // no is_used() here, so we treat a non-default delay as intent.
        const double delay_raw = program.get<double>(OPT_DUMP_PASSES_DELAY);
        const bool   delay_set = delay_raw != 2.0; // 2.0 is the argparse default

        if (dump_dir.empty() && delay_set) {
            const char* home = std::getenv("HOME");
            if (home && home[0]) {
                dump_dir = std::string(home) + "/.cache/wekde/pass-dump";
                std::cout << "dump-passes: no --dump-passes-dir set, using default " << dump_dir
                          << std::endl;
            }
        }

        if (! dump_dir.empty()) {
            std::error_code ec;
            // Create parent directory.  If the dir exists, erase stale files
            // first so /tmp doesn't keep growing across consecutive invocations.
            std::filesystem::create_directories(dump_dir, ec);
            if (std::filesystem::exists(dump_dir)) {
                size_t erased = 0;
                for (auto& ent : std::filesystem::directory_iterator(dump_dir, ec)) {
                    if (ent.is_regular_file()) {
                        std::filesystem::remove(ent.path(), ec);
                        if (! ec) erased++;
                    }
                }
                if (erased > 0) {
                    std::cout << "dump-passes: erased " << erased
                              << " stale file(s) from " << dump_dir << std::endl;
                }
            }

            // tmpfs warning — pass dumping can be several GB of RAM between
            // staging buffers and PPM output, which will OOM on tmpfs.
            if (path_is_ramfs(dump_dir)) {
                std::cerr << "dump-passes: WARNING " << dump_dir
                          << " is on a RAM-backed filesystem (tmpfs/ramfs). "
                          << "Several GB of PPMs will compete with VMA staging for RAM; "
                          << "redirect to a disk-backed path if the dump OOMs." << std::endl;
            }

            double delay_s = delay_raw;
            if (delay_s < 0.0) delay_s = 2.0;
            QTimer::singleShot((int)(delay_s * 1000.0), sv, [sv, dump_dir]() {
                std::cout << "dump-passes: requesting dump to " << dump_dir << std::endl;
                QMetaObject::invokeMethod(
                    sv, "requestPassDump", Q_ARG(QString, QString::fromStdString(dump_dir)));
            });
        }
    }

    // --js-eval "src"   inject arbitrary JS into the SceneScript engine
    // once after --js-eval-delay seconds.  Useful to force state machine
    // transitions that are normally gated on user input (e.g. 3body's
    // `shared.rst=1` would need a cursor click on the warning button).
    {
        std::string js_src = program.get<std::string>("--js-eval");
        if (! js_src.empty()) {
            double delay_s = program.get<double>("--js-eval-delay");
            if (delay_s < 0.0) delay_s = 2.0;
            QTimer::singleShot((int)(delay_s * 1000.0), sv, [sv, js_src]() {
                QString rv;
                QMetaObject::invokeMethod(sv,
                                          "debugEvalJs",
                                          Qt::DirectConnection,
                                          Q_RETURN_ARG(QString, rv),
                                          Q_ARG(QString, QString::fromStdString(js_src)));
                std::cout << "js-eval: " << js_src << " -> " << rv.toStdString() << std::endl;
            });
        }
    }

    // --cursor X,Y   repeat cursor hover at that widget-space position each
    //                second (keeps hover-driven UI visible for screenshots).
    // --click  X,Y   fire a single click event 1s after startup.
    // --screenshot PATH  same PPM dump as the GLFW viewer, for parity.
    {
        auto cursor_spec = program.present<std::string>("--cursor");
        if (cursor_spec) {
            double cx = 0, cy = 0;
            if (std::sscanf(cursor_spec->c_str(), "%lf,%lf", &cx, &cy) == 2) {
                auto* hover_timer = new QTimer(&app);
                QObject::connect(hover_timer, &QTimer::timeout, [sv, cx, cy]() {
                    QMetaObject::invokeMethod(
                        sv, "simulateHoverAt", Q_ARG(double, cx), Q_ARG(double, cy));
                });
                hover_timer->start(500);
            }
        }
        double click_delay_s = program.get<double>("--click-delay");
        if (click_delay_s < 0.0) click_delay_s = 1.5;
        int  click_delay_ms = (int)(click_delay_s * 1000.0);
        auto click_spec     = program.present<std::string>("--click");
        if (click_spec) {
            double cx = 0, cy = 0;
            if (std::sscanf(click_spec->c_str(), "%lf,%lf", &cx, &cy) == 2) {
                QTimer::singleShot(click_delay_ms, sv, [sv, cx, cy]() {
                    QMetaObject::invokeMethod(
                        sv, "simulateClickAt", Q_ARG(double, cx), Q_ARG(double, cy));
                });
            }
        }
        auto drag_spec = program.present<std::string>("--drag");
        if (drag_spec) {
            double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (std::sscanf(drag_spec->c_str(), "%lf,%lf:%lf,%lf", &x1, &y1, &x2, &y2) == 4) {
                QTimer::singleShot(click_delay_ms, sv, [sv, x1, y1, x2, y2]() {
                    QMetaObject::invokeMethod(sv,
                                              "simulateDragAt",
                                              Q_ARG(double, x1),
                                              Q_ARG(double, y1),
                                              Q_ARG(double, x2),
                                              Q_ARG(double, y2));
                });
            } else {
                std::cerr << "--drag: expected 'x1,y1:x2,y2', got: " << *drag_spec << std::endl;
            }
        }
        std::string screenshot_path = program.get<std::string>(OPT_SCREENSHOT);
        double      interval_s      = program.get<double>(OPT_SCREENSHOT_INTERVAL);
        double      max_time_s      = program.get<double>(OPT_SCREENSHOT_MAX_TIME);

        if (! screenshot_path.empty() && interval_s > 0.0) {
            // Interval (time-lapse) mode.  Fire a screenshot every
            // interval_s seconds; each one writes to `<base>_<ms>.ppm`
            // where <ms> is wall-clock elapsed since start (zero-padded
            // 6 digits so `ls` sorts chronologically).  Exit when
            // max_time_s has elapsed.  Default max_time to 10 intervals
            // if unset so a user typing just --screenshot-interval gets
            // a bounded run.
            if (max_time_s <= 0.0) max_time_s = interval_s * 10.0;

            // Find a sensible base name: strip any trailing .ppm so the
            // suffixed filenames still end with .ppm.
            std::string base = screenshot_path;
            std::string ext  = ".ppm";
            if (base.size() > 4 && base.compare(base.size() - 4, 4, ".ppm") == 0) {
                base = base.substr(0, base.size() - 4);
            }

            auto* start      = new qint64 { 0 };
            auto* shot_timer = new QTimer(&app);
            shot_timer->setSingleShot(false);
            QObject::connect(
                shot_timer,
                &QTimer::timeout,
                [sv, base, ext, interval_s, max_time_s, start, shot_timer, &app]() {
                    qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (*start == 0) *start = now;
                    double elapsed_ms = (double)(now - *start);
                    char   name_buf[32];
                    std::snprintf(name_buf, sizeof(name_buf), "_%06d", (int)elapsed_ms);
                    std::string path = base + name_buf + ext;
                    std::cout << "interval screenshot @ " << (elapsed_ms / 1000.0) << "s -> "
                              << path << std::endl;
                    QMetaObject::invokeMethod(
                        sv, "requestScreenshot", Q_ARG(QString, QString::fromStdString(path)));
                    if (elapsed_ms / 1000.0 >= max_time_s) {
                        shot_timer->stop();
                        // give the last capture time to flush before quitting
                        QTimer::singleShot(500, &app, [&app]() {
                            app.quit();
                        });
                    }
                });
            shot_timer->start((int)(interval_s * 1000.0));

            // Small initial delay so the scene has some frames under its belt.
            QTimer::singleShot(300, sv, [shot_timer]() {
                // fire the first capture immediately, then keep cadence
                QMetaObject::invokeMethod(shot_timer, "timeout");
            });
        } else if (! screenshot_path.empty()) {
            int32_t frames_to_wait = program.get<int32_t>(OPT_SCREENSHOT_FRAMES);
            int32_t fps_val        = program.get<int32_t>(OPT_FPS);
            if (fps_val < 5) fps_val = 60;
            if (frames_to_wait < 1) frames_to_wait = 30;
            int delay_ms = (int)(frames_to_wait * 1000.0 / fps_val) + 300;
            std::cout << "screenshot: will request in " << delay_ms << "ms to " << screenshot_path
                      << std::endl;
            QTimer::singleShot(delay_ms, sv, [sv, screenshot_path, &app]() {
                std::cout << "screenshot: firing requestScreenshot now" << std::endl;
                QMetaObject::invokeMethod(sv,
                                          "requestScreenshot",
                                          Q_ARG(QString, QString::fromStdString(screenshot_path)));
                auto* poll = new QTimer(&app);
                QObject::connect(poll, &QTimer::timeout, [sv, poll, &app]() {
                    bool done = false;
                    QMetaObject::invokeMethod(
                        sv, "screenshotDone", Qt::DirectConnection, Q_RETURN_ARG(bool, done));
                    if (done) {
                        poll->stop();
                        app.quit();
                    }
                });
                poll->start(20);
                // Hard cap: exit even if the backend never flips the flag.
                // Bumped to 30s so heavy scenes (~80 effects, ~200 shaders —
                // SAO, Cyberpunk Lucy, NieR 2B) have time to finish parsing
                // before main unwinds.  Otherwise the parser thread is still
                // mid-Parse when atexit handlers fire and tear down glslang's
                // static keyword unordered_set, producing a teardown UAF in
                // tokenizeIdentifier.  See glslang-thread-safety.md.
                QTimer::singleShot(30000, &app, [&app]() {
                    app.quit();
                });
            });
        }
    }

    view.show();
    return QGuiApplication::exec();
}
