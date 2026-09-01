#include <doctest.h>

#include "SoundStreamTable.hpp"
#include "WPSoundParser.hpp"
#include "wpscene/WPSoundObject.h"
#include "Audio/SoundManager.h"
#include "Fs/VFS.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using wallpaper::SoundStreamTable;
using wallpaper::UnmountSoundStreams;
using wallpaper::WPSoundParser;
using wallpaper::audio::SoundManager;
using wallpaper::fs::VFS;
using wallpaper::wpscene::WPSoundObject;

namespace
{

// The table is generic over the row type; production instantiates it with
// SoundVolumeScriptInfo / SoundLayerControlInfo.  These stand-ins keep the TU
// free of the whole SceneWallpaper public header.
struct LayerRow {
    std::string name;
};
struct VolumeRow {
    int32_t index { 0 };
};

// Mountable sound object.  Nothing here is ever opened — WPSoundParser::Parse
// only validates and mounts; the file is not touched until the audio thread
// polls NextPcmData, which no test does.
WPSoundObject MountableObj(const char* name) {
    WPSoundObject obj;
    obj.name         = name;
    obj.volume       = 0.8f;
    obj.playbackmode = "loop";
    obj.sound        = { "audio/track1.ogg" };
    return obj;
}

// Stands in for audio::SoundManager so the ORDER of the two halves of
// UnmountSoundStreams is observable: it snapshots the alias at the instant the
// streams would be freed.
struct OrderProbe {
    const SoundStreamTable<LayerRow>* layers { nullptr };
    void*                             aliasAtUnmount { nullptr };
    bool                              called { false };

    void UnMountAll() {
        called         = true;
        aliasAtUnmount = layers->streamAt(0);
    }
};

} // namespace

TEST_SUITE("SoundStreamTable") {
    TEST_CASE("publish stores rows and aliases together") {
        SoundStreamTable<LayerRow> t;
        t.publish({ { "a" }, { "b" } },
                  { reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20) });

        CHECK(t.size() == 2u);
        CHECK(t.streamAt(0) == reinterpret_cast<void*>(0x10));
        CHECK(t.streamAt(1) == reinterpret_cast<void*>(0x20));
        CHECK(t.streamAt(-1) == nullptr);
        CHECK(t.streamAt(2) == nullptr);
        REQUIRE(t.infos().size() == 2u);
        CHECK(t.infos()[1].name == "b");
    }

    TEST_CASE("invalidateStreams nulls the aliases and keeps the rows") {
        SoundStreamTable<LayerRow> t;
        t.publish({ { "a" }, { "b" } },
                  { reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20) });

        t.invalidateStreams();

        CHECK(t.size() == 2u);
        CHECK(t.streamAt(0) == nullptr);
        CHECK(t.streamAt(1) == nullptr);
        CHECK(t.infos().size() == 2u);
    }

    TEST_CASE("mismatched publish keeps every row and pads the aliases") {
        // The rows are what QML indexes, so they define the length; a short
        // alias list must not silently shorten the table and shift indices.
        SoundStreamTable<LayerRow> t;
        t.publish({ { "a" }, { "b" }, { "c" } }, { reinterpret_cast<void*>(0x10) });

        CHECK(t.size() == 3u);
        CHECK(t.streamAt(0) == reinterpret_cast<void*>(0x10));
        CHECK(t.streamAt(1) == nullptr);
        CHECK(t.streamAt(2) == nullptr);

        // Extra aliases have no row to name them and are dropped.
        t.publish({ { "a" } }, { reinterpret_cast<void*>(0x10), reinterpret_cast<void*>(0x20) });
        CHECK(t.size() == 1u);
        CHECK(t.streamAt(1) == nullptr);
    }

    TEST_CASE("an invalidated table stops reporting a still-live stream as playing") {
        // No free here — the stream stays alive for the whole case, so this
        // pins the no-op-through-nullptr contract on its own.
        SoundManager sm;
        VFS          vfs;
        auto         obj = MountableObj("music");
        void*        raw = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(raw != nullptr);

        SoundStreamTable<LayerRow> t;
        t.publish({ { "music" } }, { raw });
        CHECK(t.isPlaying(0)); // not startsilent -> Playing

        t.invalidateStreams();
        CHECK_FALSE(t.isPlaying(0));

        t.play(0);
        t.stop(0);
        t.pause(0);
        t.setVolume(0, 0.5f);
        CHECK_FALSE(t.isPlaying(0)); // dispatches went nowhere, as intended
    }

    TEST_CASE("UnmountSoundStreams drops every alias before the streams are freed") {
        // The reload step: loadScene frees the previous scene's streams while
        // the QML property tick is still indexing the published tables.
        SoundManager sm;
        VFS          vfs;
        auto         layerObj  = MountableObj("music");
        auto         volumeObj = MountableObj("ambience");
        void*        layerRaw  = WPSoundParser::Parse(layerObj, vfs, sm);
        void*        volumeRaw = WPSoundParser::Parse(volumeObj, vfs, sm);
        REQUIRE(layerRaw != nullptr);
        REQUIRE(volumeRaw != nullptr);
        REQUIRE(sm.MountedChannelCount() == 2u);

        SoundStreamTable<LayerRow> layers;
        layers.publish({ { "music" } }, { layerRaw });
        SoundStreamTable<VolumeRow> volumes;
        volumes.publish({ { 0 } }, { volumeRaw });

        UnmountSoundStreams(sm, layers, volumes);

        CHECK(sm.MountedChannelCount() == 0u); // the streams really are gone
        CHECK(layers.streamAt(0) == nullptr);
        CHECK(volumes.streamAt(0) == nullptr);
        CHECK(layers.size() == 1u); // rows survive, so indices stay in range
        CHECK(volumes.size() == 1u);
    }

    TEST_CASE("post-unmount dispatches do not reach the freed streams") {
        // Every call the QML property tick can make during a reload, against a
        // table whose streams were just destroyed.  Only stays memory-safe
        // because the aliases were nulled first; if that ever regresses this
        // case is a use-after-free write and the sanitizer gate reports it.
        SoundManager sm;
        VFS          vfs;
        auto         obj = MountableObj("music");
        void*        raw = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(raw != nullptr);

        SoundStreamTable<LayerRow>  layers;
        SoundStreamTable<VolumeRow> volumes;
        layers.publish({ { "music" } }, { raw });
        volumes.publish({ { 0 } }, { raw });

        UnmountSoundStreams(sm, layers, volumes);
        REQUIRE(sm.MountedChannelCount() == 0u);

        layers.play(0);
        layers.stop(0);
        layers.pause(0);
        layers.setVolume(0, 0.25f);
        volumes.setVolume(0, 0.5f);
        CHECK_FALSE(layers.isPlaying(0));
    }

    TEST_CASE("the free happens after the aliases are dropped, not before") {
        // Catches a reordering to `sm.UnMountAll(); layers.invalidateStreams();`,
        // which every other case here would still pass.
        SoundStreamTable<LayerRow>  layers;
        SoundStreamTable<VolumeRow> volumes;
        layers.publish({ { "music" } }, { reinterpret_cast<void*>(0x10) });
        volumes.publish({ { 0 } }, { reinterpret_cast<void*>(0x20) });

        OrderProbe probe;
        probe.layers = &layers;

        UnmountSoundStreams(probe, layers, volumes);

        CHECK(probe.called);
        CHECK(probe.aliasAtUnmount == nullptr);
    }

    TEST_CASE("republish after unmount re-arms the table") {
        SoundManager sm;
        VFS          vfs;
        auto         obj = MountableObj("music");
        void*        raw = WPSoundParser::Parse(obj, vfs, sm);
        REQUIRE(raw != nullptr);

        SoundStreamTable<LayerRow>  layers;
        SoundStreamTable<VolumeRow> volumes;
        layers.publish({ { "music" } }, { raw });

        UnmountSoundStreams(sm, layers, volumes);

        auto  freshObj = MountableObj("music2");
        void* fresh    = WPSoundParser::Parse(freshObj, vfs, sm);
        REQUIRE(fresh != nullptr);
        layers.publish({ { "music2" } }, { fresh });

        CHECK(layers.streamAt(0) == fresh);
        CHECK(layers.isPlaying(0));
        CHECK(layers.infos()[0].name == "music2");
    }

    TEST_CASE("out-of-range indices are no-ops on a populated table") {
        SoundStreamTable<LayerRow> t;
        t.publish({ { "music" } }, { reinterpret_cast<void*>(0x10) });

        t.setVolume(5, 1.0f);
        t.play(-1);
        t.stop(99);
        t.pause(-7);
        CHECK_FALSE(t.isPlaying(7));
        CHECK(t.size() == 1u);
        CHECK(t.streamAt(0) == reinterpret_cast<void*>(0x10));
    }

    TEST_CASE("an empty table absorbs every dispatch") {
        SoundStreamTable<VolumeRow> t;
        t.setVolume(0, 1.0f);
        t.play(0);
        CHECK_FALSE(t.isPlaying(0));
        CHECK(t.size() == 0u);
        CHECK(t.streamAt(0) == nullptr);
        CHECK(t.infos().empty());
    }

} // SoundStreamTable
