#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WRITERHISTORY_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_WRITERHISTORY_H_

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Libs::Graphics::WriterHistory {
inline constexpr size_t Capacity = 32768;
inline constexpr size_t DefaultMatches = 8;
inline constexpr size_t MaxMatches = 64;
enum class Kind : uint32_t { Packet, ComputeBuffer };
struct Record {
    uint64_t sequence = 0, address = 0, size = 0, detail = 0;
    uint64_t shader = 0, frame = 0, tick = 0, submit = 0;
    uint64_t native_buffer = 0, native_offset = 0, native_range = 0;
    uint32_t slot = 0;
    std::array<uint32_t, 3> groups {};
    std::array<char, 48> source {};
    Kind kind = Kind::Packet;
    bool context_available = false, native_available = false;
    bool indirect = false, table = false, source_truncated = false;
};
struct Stats {
    size_t capacity = 0, retained = 0;
    uint64_t last_sequence = 0, first_retained_sequence = 0;
    uint64_t dropped = 0, rejected = 0;
};
struct Query {
    Stats stats;
    bool enabled = false, valid_range = false, truncated = false;
    uint64_t cutoff_sequence = 0, matches = 0;
    std::vector<Record> records; // newest first, at most MaxMatches
};
inline bool ValidRange(uint64_t address, uint64_t size) {
    return address != 0 && size != 0 && size <= std::numeric_limits<uint64_t>::max() - address;
}
inline bool Overlaps(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
    return ValidRange(a, as) && ValidRange(b, bs) && a < b + bs && b < a + as;
}
// Pure CPU bounded ring. Its owner serializes Add/Read; records are observations of
// declared writes, not proof of executed stores or GPU completion.
class History {
public:
    explicit History(size_t capacity = Capacity) : m_records(std::max<size_t>(capacity, 1)) {}
    bool Add(Record record) {
        if (!ValidRange(record.address, record.size) || m_sequence == UINT64_MAX) {
            ++m_rejected;
            return false;
        }
        record.sequence = ++m_sequence;
        m_records[m_next] = record;
        m_next = (m_next + 1) % m_records.size();
        m_retained = std::min(m_retained + 1, m_records.size());
        return true;
    }
    Stats Snapshot() const {
        Stats s;
        s.capacity = m_records.size(); s.retained = m_retained;
        s.last_sequence = m_sequence; s.dropped = m_sequence - m_retained;
        s.rejected = m_rejected;
        if (m_retained) s.first_retained_sequence =
            m_records[(m_next + m_records.size() - m_retained) % m_records.size()].sequence;
        return s;
    }
    Query Read(uint64_t address, uint64_t size, uint64_t cutoff,
               size_t max_matches = DefaultMatches) const {
        Query q;
        q.enabled = true; q.stats = Snapshot(); q.cutoff_sequence = cutoff;
        q.valid_range = ValidRange(address, size);
        if (!q.valid_range) return q;
        const auto limit = std::min(max_matches, MaxMatches);
        q.records.reserve(std::min(limit, m_retained));
        for (size_t n = 0; n < m_retained; ++n) {
            const auto& r = m_records[(m_next + m_records.size() - 1 - n) % m_records.size()];
            if (r.sequence > cutoff || !Overlaps(address, size, r.address, r.size)) continue;
            ++q.matches;
            if (q.records.size() < limit) q.records.push_back(r);
        }
        q.truncated = q.matches > q.records.size();
        return q;
    }
private:
    std::vector<Record> m_records;
    size_t m_next = 0, m_retained = 0;
    uint64_t m_sequence = 0, m_rejected = 0;
};
} // namespace Libs::Graphics::WriterHistory
#endif
