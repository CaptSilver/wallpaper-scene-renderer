#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "WPSoundParser.hpp"
#include "Utils/Logging.h"

namespace wallpaper
{

// One scene's published sound rows plus the WPSoundStream* aliases that back
// them, indexed in lockstep.
//
// The streams belong to audio::SoundManager; the pointers here are pure
// non-owning aliases, so the instant the manager frees a stream every alias
// naming it dangles — and the QML property tick keeps indexing this table by
// index at ~125 Hz, writing through the aliases (volume, play/stop/pause).
// Rows and aliases therefore live behind one mutex, and every dispatch keeps
// that mutex held across the WPSoundParser call.  That is what makes
// invalidate-then-free airtight: a reader either takes the lock before the
// invalidation (and the free waits behind it) or after (and sees nullptr).
template<class InfoT>
class SoundStreamTable {
public:
    // Rows and aliases go in together so a reader can never see one scene's
    // rows indexed against another scene's streams.  The rows define the
    // table length; a short alias list is padded with nullptr rather than
    // dropping rows QML may already be holding indices into.
    void publish(std::vector<InfoT> infos, std::vector<void*> streams) {
        if (infos.size() != streams.size()) {
            LOG_ERROR("sound stream table: %zu rows but %zu stream aliases — padding/truncating "
                      "to the row count; some sound controls will be inert",
                      infos.size(),
                      streams.size());
            streams.resize(infos.size(), nullptr);
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_infos   = std::move(infos);
        m_streams = std::move(streams);
    }

    // Fill with nullptr rather than clearing: the rows stay addressable, so
    // every index QML already holds keeps bounds-checking the same way and the
    // dispatches no-op through WPSoundParser's null guards instead of falling
    // out of range.
    void invalidateStreams() {
        std::lock_guard<std::mutex> lock(m_mutex);
        std::fill(m_streams.begin(), m_streams.end(), nullptr);
    }

    std::vector<InfoT> infos() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_infos;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_infos.size();
    }

    void* streamAt(int32_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return aliasAt(index);
    }

    void play(int32_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WPSoundParser::StreamPlay(aliasAt(index));
    }

    void stop(int32_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WPSoundParser::StreamStop(aliasAt(index));
    }

    void pause(int32_t index) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WPSoundParser::StreamPause(aliasAt(index));
    }

    bool isPlaying(int32_t index) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return WPSoundParser::StreamIsPlaying(aliasAt(index));
    }

    void setVolume(int32_t index, float volume) {
        std::lock_guard<std::mutex> lock(m_mutex);
        WPSoundParser::SetStreamVolume(aliasAt(index), volume);
    }

private:
    // Caller holds m_mutex.  Out-of-range reads as "no stream", which the
    // WPSoundParser wrappers already treat as a no-op.
    void* aliasAt(int32_t index) const {
        if (index < 0 || static_cast<std::size_t>(index) >= m_streams.size()) return nullptr;
        return m_streams[static_cast<std::size_t>(index)];
    }

    mutable std::mutex m_mutex;
    std::vector<InfoT> m_infos;
    std::vector<void*> m_streams;
};

// Drop the published aliases, then free the streams they named — in that
// order.  Reversing it reopens the window this exists to close.  Templated on
// the manager so the ordering is observable in a test without an audio device.
template<class ManagerT, class LayerInfoT, class VolumeInfoT>
void UnmountSoundStreams(ManagerT& sm, SoundStreamTable<LayerInfoT>& layers,
                         SoundStreamTable<VolumeInfoT>& volumes) {
    layers.invalidateStreams();
    volumes.invalidateStreams();
    sm.UnMountAll();
}

} // namespace wallpaper
