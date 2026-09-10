#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>
#include <memory>
#include <span>
#include <limits>

#include "Core/NoCopyMove.hpp"
#include "Core/Literals.hpp"

namespace wallpaper
{
class SceneIndexArray : NoCopy {
    constexpr static size_t Unit_Byte_Size { sizeof(uint32_t) };

public:
    // Storage is always uint32_t-backed, but a 16-bit array packs two indices
    // per slot.  The GPU has to be told which, so the tag rides along with the
    // data instead of being inferred at the bind site.
    enum class IndexWidth
    {
        U16,
        U32
    };

    SceneIndexArray(usize indexCount);
    SceneIndexArray(std::span<const uint32_t> data, IndexWidth width = IndexWidth::U16);

    IndexWidth Width() const noexcept { return m_width; }

    // Number of actual indices, as opposed to storage slots.
    usize IndexElemCount() const noexcept { return ElemCountFor(DataCount()); }
    usize RenderIndexElemCount() const noexcept { return ElemCountFor(RenderDataCount()); }

    // Indices actually issued to a triangle-list draw.  A 16-bit array can
    // carry a trailing index that completes no triangle (the odd-triangle
    // padding slot), so round down.
    usize DrawIndexCount() const noexcept { return (IndexElemCount() / 3) * 3; }
    usize RenderDrawIndexCount() const noexcept { return (RenderIndexElemCount() / 3) * 3; }

    SceneIndexArray(SceneIndexArray&&) noexcept;
    ~SceneIndexArray() = default;

    void Assign(usize index, std::span<const uint32_t> data) { AssignSpan(index, data); }
    void AssignHalf(usize index, std::span<const uint16_t> data) { AssignSpan(index, data); }

    // Get
    const uint32_t* Data() const { return m_pData.get(); }
    usize           DataCount() const { return m_size; }
    usize           DataSizeOf() const { return m_size * Unit_Byte_Size; }

    usize RenderDataCount() const noexcept {
        return m_render_size > m_size ? m_size : m_render_size;
    }
    void SetRenderDataCount(usize val) noexcept { m_render_size = val; }

    usize CapacityCount() const { return m_capacity; }
    usize CapacitySizeof() const { return m_capacity * Unit_Byte_Size; }

    uint32_t ID() const { return m_id; }
    void     SetID(uint32_t id) { m_id = id; }

private:
    // Not named `slots` — Qt defines that as a keyword macro, and this header
    // reaches the QML bridge through Scene.h.
    usize ElemCountFor(usize slot_count) const noexcept {
        return m_width == IndexWidth::U32 ? slot_count : slot_count * 2;
    }

    bool IncreaseCheckSet(size_t size);

    template<typename T>
    void AssignSpan(usize index, std::span<const T> data) {
        using in_value_type = T;
        if (! IncreaseCheckSet((index + data.size()) * sizeof(in_value_type))) return;
        std::copy(data.begin(), data.end(), ((in_value_type*)m_pData.get()) + index);
    }

    std::unique_ptr<uint32_t[]> m_pData;
    usize                       m_size { 0 };
    usize                       m_capacity { 0 };

    usize m_render_size { std::numeric_limits<usize>::max() };

    IndexWidth m_width { IndexWidth::U16 };

    uint32_t m_id;
};
} // namespace wallpaper
