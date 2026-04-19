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
#include "arg.hpp"

int main(int argc, char** argv) {
    argparse::ArgumentParser program("scene-viewer");
    setAndParseArg(program, argc, argv);

#if (QT_VERSION < QT_VERSION_CHECK(6, 0, 0))
    QCoreApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
#endif
    QGuiApplication app(argc, argv);

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

    // --user-props + --set key=value overrides, applied before scripts start.
    std::string user_props = BuildUserPropsJson(program);
    if (!user_props.empty()) {
        std::cout << "user props: " << user_props << std::endl;
        sv->setProperty("userProperties", QString::fromStdString(user_props));
    }

    sv->setAcceptMouse(true);
    sv->setAcceptHover(true);

    // --dump-passes-dir: schedule a one-shot per-pass RT dump N seconds
    // after startup.  Used to pinpoint where text / colour corruption
    // first enters the effect chain on wallpapers like 2866203962.
    {
        std::string dump_dir = program.get<std::string>(OPT_DUMP_PASSES_DIR);
        if (! dump_dir.empty()) {
            double delay_s = program.get<double>(OPT_DUMP_PASSES_DELAY);
            if (delay_s < 0.0) delay_s = 2.0;
            QTimer::singleShot((int)(delay_s * 1000.0), sv,
                [sv, dump_dir]() {
                    std::cout << "dump-passes: requesting dump to " << dump_dir << std::endl;
                    QMetaObject::invokeMethod(sv, "requestPassDump",
                        Q_ARG(QString, QString::fromStdString(dump_dir)));
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
                    QMetaObject::invokeMethod(sv, "simulateHoverAt",
                                              Q_ARG(double, cx), Q_ARG(double, cy));
                });
                hover_timer->start(500);
            }
        }
        auto click_spec = program.present<std::string>("--click");
        if (click_spec) {
            double cx = 0, cy = 0;
            if (std::sscanf(click_spec->c_str(), "%lf,%lf", &cx, &cy) == 2) {
                QTimer::singleShot(1500, sv, [sv, cx, cy]() {
                    QMetaObject::invokeMethod(sv, "simulateClickAt",
                                              Q_ARG(double, cx), Q_ARG(double, cy));
                });
            }
        }
        std::string screenshot_path = program.get<std::string>(OPT_SCREENSHOT);
        double interval_s = program.get<double>(OPT_SCREENSHOT_INTERVAL);
        double max_time_s = program.get<double>(OPT_SCREENSHOT_MAX_TIME);

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
            if (base.size() > 4 &&
                base.compare(base.size() - 4, 4, ".ppm") == 0) {
                base = base.substr(0, base.size() - 4);
            }

            auto* start = new qint64{ 0 };
            auto* shot_timer = new QTimer(&app);
            shot_timer->setSingleShot(false);
            QObject::connect(shot_timer, &QTimer::timeout,
                [sv, base, ext, interval_s, max_time_s, start, shot_timer, &app]() {
                    qint64 now = QDateTime::currentMSecsSinceEpoch();
                    if (*start == 0) *start = now;
                    double elapsed_ms = (double)(now - *start);
                    char name_buf[32];
                    std::snprintf(name_buf, sizeof(name_buf), "_%06d",
                                  (int)elapsed_ms);
                    std::string path = base + name_buf + ext;
                    std::cout << "interval screenshot @ " << (elapsed_ms / 1000.0)
                              << "s -> " << path << std::endl;
                    QMetaObject::invokeMethod(sv, "requestScreenshot",
                        Q_ARG(QString, QString::fromStdString(path)));
                    if (elapsed_ms / 1000.0 >= max_time_s) {
                        shot_timer->stop();
                        // give the last capture time to flush before quitting
                        QTimer::singleShot(500, &app, [&app]() { app.quit(); });
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
            std::cout << "screenshot: will request in " << delay_ms
                      << "ms to " << screenshot_path << std::endl;
            QTimer::singleShot(delay_ms, sv, [sv, screenshot_path, &app]() {
                std::cout << "screenshot: firing requestScreenshot now" << std::endl;
                QMetaObject::invokeMethod(sv, "requestScreenshot",
                                          Q_ARG(QString, QString::fromStdString(screenshot_path)));
                auto* poll = new QTimer(&app);
                QObject::connect(poll, &QTimer::timeout, [sv, poll, &app]() {
                    bool done = false;
                    QMetaObject::invokeMethod(sv, "screenshotDone",
                                              Qt::DirectConnection,
                                              Q_RETURN_ARG(bool, done));
                    if (done) {
                        poll->stop();
                        app.quit();
                    }
                });
                poll->start(20);
                // hard cap: exit even if the backend never flips the flag
                QTimer::singleShot(5000, &app, [&app]() { app.quit(); });
            });
        }
    }

    view.show();
    return QGuiApplication::exec();
}
