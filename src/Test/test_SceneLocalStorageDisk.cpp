#include <doctest.h>

#include <QByteArray>
#include <QFile>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QUrl>

#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "SceneBackend.hpp"
#include "Utils/Logging.h"
#include "test_scratch.hpp"

// Disk side of the SceneScript localStorage store: the debounced flush that
// replaces <cache>/wescene-renderer/localstorage_global.json (and the
// per-scene file next to it).  The JS-facing behaviour is covered by the
// localStorage cases in test_SceneScript.cpp against an in-memory stub; these
// cases drive the real file, because that is where a wallpaper's persisted
// state actually lives across a plasmashell restart.

namespace
{

namespace fs = std::filesystem;

// Points XDG_CACHE_HOME at a fresh directory for the duration of one case, so
// the object under test writes into scratch instead of the developer's cache.
struct CacheSandbox {
    std::string root;
    QByteArray  savedXdg;
    bool        hadXdg { false };

    explicit CacheSandbox(const char* tag) {
        // Resolve scratch BEFORE moving XDG_CACHE_HOME — ScratchBase() can
        // fall back to it.
        static int counter = 0;
        root = wallpaper::test::ScratchDir("ls_disk", (int)::getpid()) + "/" + tag + "_" +
               std::to_string(counter++);
        fs::remove_all(root);
        fs::create_directories(root);
        hadXdg   = qEnvironmentVariableIsSet("XDG_CACHE_HOME");
        savedXdg = qgetenv("XDG_CACHE_HOME");
        qputenv("XDG_CACHE_HOME", QByteArray::fromStdString(root));
    }
    ~CacheSandbox() {
        if (hadXdg)
            qputenv("XDG_CACHE_HOME", savedXdg);
        else
            qunsetenv("XDG_CACHE_HOME");
        std::error_code ec;
        fs::permissions(sceneDir(),
                        fs::perms::owner_all,
                        fs::perm_options::add,
                        ec); // so remove_all can descend
        fs::remove_all(root, ec);
    }

    // <cache>/wescene-renderer — where both scope files live.
    std::string sceneDir() const {
        return root + "/" + std::string(wallpaper::platform::kRendererCacheDir);
    }
    std::string globalFile() const {
        return sceneDir() + "/" + std::string(wallpaper::platform::kLocalStorageGlobalFile);
    }

    void writeFile(const std::string& path, const std::string& content) const {
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream f(path, std::ios::trunc);
        f << content;
    }
};

QJsonObject readJsonObject(const std::string& path) {
    QFile f(QString::fromStdString(path));
    if (! f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

// Restores a directory's write bit however the case exits.
struct PermRestore {
    std::string dir;
    ~PermRestore() {
        std::error_code ec;
        fs::permissions(dir, fs::perms::owner_all, fs::perm_options::add, ec);
    }
};

// Captures WallpaperLog output so a case can assert a failure was surfaced
// rather than swallowed.
struct LogCapture {
    static inline std::string text;
    LogCapture() {
        text = {};
        wallpaper_log_test::setSink([](int, const char* msg) {
            text += msg;
            text += '\n';
        });
    }
    ~LogCapture() { wallpaper_log_test::setSink(nullptr); }
    static bool mentions(const char* needle) { return text.find(needle) != std::string::npos; }
};

constexpr int kGlobal = 0;

} // namespace

TEST_SUITE("SceneScript localStorage disk store") {
    TEST_CASE("flush writes the global scope to disk") {
        CacheSandbox sb("write");
        auto         obj = std::make_unique<scenebackend::SceneObject>();

        obj->lsSet(kGlobal, "hits", QJSValue(7));
        obj->flushLocalStorageForTesting();

        CHECK_FALSE(obj->localStorageDirtyForTesting(true));
        CHECK(readJsonObject(sb.globalFile()).value("hits").toInt() == 7);
        // The temp file must not survive a successful write.
        CHECK_FALSE(fs::exists(sb.globalFile() + ".tmp"));
    }

    TEST_CASE("a replace swaps the file instead of truncating it under a reader") {
        CacheSandbox sb("replace");
        sb.writeFile(sb.globalFile(), R"({"kept":"previous"})");
        struct stat before {};
        REQUIRE(::stat(sb.globalFile().c_str(), &before) == 0);

        // A reader that opened the scope before the write still has to see a
        // whole document — the replace is a rename, not a truncate in place.
        const int fd = ::open(sb.globalFile().c_str(), O_RDONLY);
        REQUIRE(fd >= 0);

        auto obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsSet(kGlobal, "kept", QJSValue("current"));
        obj->flushLocalStorageForTesting();

        char       buf[128] = {};
        const auto n        = ::read(fd, buf, sizeof(buf) - 1);
        ::close(fd);
        CHECK(std::string(buf, n > 0 ? (size_t)n : 0) == R"({"kept":"previous"})");

        struct stat after {};
        REQUIRE(::stat(sb.globalFile().c_str(), &after) == 0);
        CHECK(after.st_ino != before.st_ino);
        CHECK(readJsonObject(sb.globalFile()).value("kept").toString() == "current");
    }

    TEST_CASE("a write that cannot land keeps the scope dirty and spares the old file") {
        if (::geteuid() == 0) return; // root ignores the directory mode
        CacheSandbox sb("readonly");
        sb.writeFile(sb.globalFile(), R"({"kept":"previous"})");

        PermRestore restore { sb.sceneDir() };
        LogCapture  log;
        {
            auto obj = std::make_unique<scenebackend::SceneObject>();
            obj->lsSet(kGlobal, "fresh", QJSValue(1)); // loads the existing file first

            std::error_code ec;
            fs::permissions(sb.sceneDir(), fs::perms::owner_write, fs::perm_options::remove, ec);
            REQUIRE_FALSE(ec);

            obj->flushLocalStorageForTesting();

            // Nothing landed, so the pending write must survive for the next
            // debounce tick instead of being dropped on the floor.
            CHECK(obj->localStorageDirtyForTesting(true));
            CHECK(LogCapture::mentions("localStorage"));
        }
        // The previously persisted keys are still readable — the live file was
        // never removed to make room for a write that failed.
        CHECK(readJsonObject(sb.globalFile()).value("kept").toString() == "previous");
    }

    TEST_CASE("a rename that cannot replace the target keeps the scope dirty") {
        if (::geteuid() == 0) return;
        CacheSandbox sb("renamefail");
        // A non-empty directory in the target's place: the temp file writes
        // fine, the replace cannot happen.
        fs::create_directories(sb.globalFile());
        sb.writeFile(sb.globalFile() + "/blocker", "x");

        LogCapture log;
        auto       obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsSet(kGlobal, "fresh", QJSValue(1));
        obj->flushLocalStorageForTesting();

        CHECK(obj->localStorageDirtyForTesting(true));
        CHECK(LogCapture::mentions("localStorage"));
        // A temp file left behind would leak one per retry.
        CHECK_FALSE(fs::exists(sb.globalFile() + ".tmp"));
    }

    TEST_CASE("flush keeps keys another instance wrote after this one loaded") {
        CacheSandbox sb("merge");
        sb.writeFile(sb.globalFile(), R"({"a":1})");

        auto obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsSet(kGlobal, "c", QJSValue(3)); // loads {"a":1}

        // A second wallpaper instance (other monitor) persists its own key.
        sb.writeFile(sb.globalFile(), R"({"a":1,"b":2})");

        obj->flushLocalStorageForTesting();

        const QJsonObject onDisk = readJsonObject(sb.globalFile());
        CHECK(onDisk.value("a").toInt() == 1);
        CHECK(onDisk.value("b").toInt() == 2);
        CHECK(onDisk.value("c").toInt() == 3);
    }

    TEST_CASE("a sibling's newer value is not overwritten by an untouched stale copy") {
        CacheSandbox sb("stale");
        sb.writeFile(sb.globalFile(), R"({"shared":"old"})");

        auto obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsSet(kGlobal, "mine", QJSValue(1)); // loads shared="old"

        sb.writeFile(sb.globalFile(), R"({"shared":"new"})");
        obj->flushLocalStorageForTesting();

        const QJsonObject onDisk = readJsonObject(sb.globalFile());
        CHECK(onDisk.value("shared").toString() == "new");
        CHECK(onDisk.value("mine").toInt() == 1);
    }

    TEST_CASE("a removed key stays removed through the merge") {
        CacheSandbox sb("remove");
        sb.writeFile(sb.globalFile(), R"({"a":1,"b":2})");

        auto obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsRemove(kGlobal, "a");

        sb.writeFile(sb.globalFile(), R"({"a":1,"b":2,"z":9})");
        obj->flushLocalStorageForTesting();

        const QJsonObject onDisk = readJsonObject(sb.globalFile());
        CHECK_FALSE(onDisk.contains("a"));
        CHECK(onDisk.value("b").toInt() == 2);
        CHECK(onDisk.value("z").toInt() == 9);
    }

    TEST_CASE("clear wipes the scope on disk instead of merging it back") {
        CacheSandbox sb("clear");
        sb.writeFile(sb.globalFile(), R"({"a":1,"b":2})");

        auto obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsClear(kGlobal);
        obj->lsSet(kGlobal, "after", QJSValue(5));
        obj->flushLocalStorageForTesting();

        const QJsonObject onDisk = readJsonObject(sb.globalFile());
        CHECK(onDisk.size() == 1);
        CHECK(onDisk.value("after").toInt() == 5);
    }

    TEST_CASE("a corrupt scope file is reported and overwritten, not silently empty") {
        CacheSandbox sb("corrupt");
        sb.writeFile(sb.globalFile(), "{not json at all");

        LogCapture log;
        auto       obj = std::make_unique<scenebackend::SceneObject>();
        obj->lsSet(kGlobal, "k", QJSValue(1));
        CHECK(LogCapture::mentions("localStorage"));

        obj->flushLocalStorageForTesting();
        CHECK_FALSE(obj->localStorageDirtyForTesting(true));
        CHECK(readJsonObject(sb.globalFile()).value("k").toInt() == 1);
    }

    TEST_CASE("the per-scene scope round-trips through its own file") {
        CacheSandbox sb("scene");
        auto         obj = std::make_unique<scenebackend::SceneObject>();
        obj->setSource(QUrl::fromLocalFile("/tmp/wek-nonexistent/3662790108/scene.pkg"));

        obj->lsSet(1, "icons", QJSValue(false));
        obj->flushLocalStorageForTesting();

        CHECK_FALSE(obj->localStorageDirtyForTesting(false));
        const std::string scenePath = sb.sceneDir() + "/3662790108/" +
                                      std::string(wallpaper::platform::kLocalStorageSceneFile);
        CHECK(readJsonObject(scenePath).contains("icons"));
    }
}
