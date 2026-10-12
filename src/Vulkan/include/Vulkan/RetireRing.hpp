#pragma once
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace wallpaper::vulkan
{

// Images and framebuffers a frame stopped using, kept until no submitted
// command buffer can still name them.  Frame F waits at its head for the
// fence of frame F - frames_in_flight, so an entry stamped R is safe to
// destroy once R + frames_in_flight <= F.  The caller destroys what collect
// returns; drain hands everything back for a WaitIdle-covered teardown.
template<typename T>
class RetireRing {
public:
    void push(T item, std::uint64_t stamp) { m_entries.push_back({ std::move(item), stamp }); }

    std::vector<T> collect(std::uint64_t frame, std::uint64_t frames_in_flight) {
        std::vector<T> out;
        std::erase_if(m_entries, [&](Entry& e) {
            if (e.stamp + frames_in_flight > frame) return false;
            out.push_back(std::move(e.item));
            return true;
        });
        return out;
    }
    std::vector<T> drain() {
        std::vector<T> out;
        out.reserve(m_entries.size());
        for (Entry& e : m_entries) out.push_back(std::move(e.item));
        m_entries.clear();
        return out;
    }
    std::size_t size() const { return m_entries.size(); }

private:
    struct Entry {
        T             item;
        std::uint64_t stamp;
    };
    std::vector<Entry> m_entries;
};

} // namespace wallpaper::vulkan
