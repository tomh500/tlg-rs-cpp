#include "slide.hpp"
#include "internal.hpp"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>

#if defined(_MSC_VER)
#define TLG_SLIDE_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define TLG_SLIDE_INLINE inline __attribute__((always_inline))
#else
#define TLG_SLIDE_INLINE inline
#endif

namespace tlg::detail {
namespace {

constexpr std::size_t hash3(std::uint8_t a, std::uint8_t b, std::uint8_t c) noexcept {
    const auto value = std::uint32_t(a) | (std::uint32_t(b) << 8) | (std::uint32_t(c) << 16);
    return (value * 0x9e3779b1u) >> 17;
}

TLG_SLIDE_INLINE std::size_t hash4(const std::uint8_t* bytes) noexcept {
    std::uint32_t value;
    std::memcpy(&value, bytes, sizeof(value));
    if constexpr (std::endian::native == std::endian::big) value = std::byteswap(value);
    return (value * 0x9e3779b1u) >> 17;
}

TLG_SLIDE_INLINE std::size_t common_prefix(const std::uint8_t* a, const std::uint8_t* b,
                          std::size_t limit) noexcept {
    std::size_t i = 0;
    // memcpy permits unaligned loads without violating C++ aliasing rules.
    // The bound is the match cap, including for overlapping copies.
    while (limit - i >= sizeof(std::uint64_t)) {
        std::uint64_t lhs, rhs;
        std::memcpy(&lhs, a + i, sizeof(lhs));
        std::memcpy(&rhs, b + i, sizeof(rhs));
        auto difference = lhs ^ rhs;
        if (difference != 0) {
            if constexpr (std::endian::native == std::endian::big)
                difference = std::byteswap(difference);
            return i + std::countr_zero(difference) / 8;
        }
        i += sizeof(std::uint64_t);
    }
    while (i < limit && a[i] == b[i]) ++i;
    return i;
}

// Costs only need the next 273 byte positions. A 512-leaf ring keeps exact
// shortest-path parsing linear in memory without retaining a full cost table.
// Each query merges all eight flag phases together; don't run eight trees.
class ParseMinimum {
public:
    static constexpr std::size_t leaves = 512;
    struct Node {
        std::array<std::size_t, 8> cost;
        std::array<std::size_t, 8> position{};
        Node() { cost.fill(std::numeric_limits<std::size_t>::max()); }
    };
    static constexpr std::size_t working_bytes = 2 * leaves * sizeof(Node);

    ParseMinimum() : tree_(2 * leaves) {}

    void put(std::size_t position, const std::array<std::size_t, 8>& costs) {
        auto i = leaves + (position & (leaves - 1));
        tree_[i].cost = costs;
        tree_[i].position.fill(position);
        while (i > 1) {
            i /= 2;
            tree_[i] = merge(tree_[2 * i], tree_[2 * i + 1]);
        }
    }

    [[nodiscard]] Node query(std::size_t first, std::size_t last) const {
        // The inclusive interval is shorter than the ring. Split a wrapped
        // interval once; every queried leaf is an already computed successor.
        const auto a = first & (leaves - 1);
        const auto b = last & (leaves - 1);
        return a <= b ? interval(a, b + 1)
                      : merge(interval(a, leaves), interval(0, b + 1));
    }

private:
    std::vector<Node> tree_;

    static Node merge(const Node& a, const Node& b) {
        Node result;
        for (std::size_t phase = 0; phase < 8; ++phase) {
            const auto choose_b = b.cost[phase] < a.cost[phase] ||
                (b.cost[phase] == a.cost[phase] && b.position[phase] > a.position[phase]);
            result.cost[phase] = choose_b ? b.cost[phase] : a.cost[phase];
            result.position[phase] = choose_b ? b.position[phase] : a.position[phase];
        }
        return result;
    }

    Node interval(std::size_t first, std::size_t last) const {
        Node result;
        first += leaves;
        last += leaves;
        while (first < last) {
            if ((first & 1) != 0) result = merge(result, tree_[first++]);
            if ((last & 1) != 0) result = merge(result, tree_[--last]);
            first /= 2;
            last /= 2;
        }
        return result;
    }
};

} // namespace

SlideEncoder::SlideEncoder(std::span<const std::uint8_t> dictionary, SlideParse parse,
                           std::size_t max_working_bytes, SlideMatch match)
    : heads_(keys), parse_(parse), max_working_bytes_(max_working_bytes), match_(match) {
    require(dictionary.empty() || dictionary.size() == window, ErrorCode::invalid_argument,
            "Slide dictionary must be empty or exactly 4096 bytes");
    require(parse == SlideParse::greedy || parse == SlideParse::optimal,
            ErrorCode::invalid_argument, "unknown Slide parse algorithm");
    require(match == SlideMatch::adaptive || match == SlideMatch::primary,
            ErrorCode::invalid_argument, "unknown Slide match algorithm");
    std::copy(dictionary.begin(), dictionary.end(), text_.begin());
    std::copy_n(text_.begin(), longest - 1, text_.begin() + window);
    if (!dictionary.empty()) {
        steps_ = window;
        rebuild();
    }
}

TLG_SLIDE_INLINE void SlideEncoder::push(std::uint8_t value) {
    if (long_heads_.empty()) push_byte<false>(value);
    else push_byte<true>(value);
}

template<bool LongKeys>
TLG_SLIDE_INLINE void SlideEncoder::push_byte(std::uint8_t value) {
    const auto slot = std::size_t(position_) & mask;
    text_[slot] = value;
    if (slot < longest - 1) text_[slot + window] = value;

    // A new byte completes the three-byte key beginning two positions ago.
    const auto start = position_ - 2;
    const auto q = std::size_t(start) & mask;
    const auto key = hash3(text_[q], text_[(q + 1) & mask], value);
    previous_[q] = heads_[key];
    heads_[key] = start;
    if constexpr (LongKeys) {
        if (steps_ != 0) {
            const auto long_start = position_ - 3;
            const auto long_slot = std::size_t(long_start) & mask;
            const auto long_key = hash4(text_.data() + long_slot);
            long_previous_[long_slot] = long_heads_[long_key];
            long_heads_[long_key] = long_start;
        }
    }
    ++position_;
    steps_ = std::min(steps_ + 1, window);
}

void SlideEncoder::rebuild() {
    std::fill(heads_.begin(), heads_.end(), 0);
    if (steps_ == 0) {
        if (!long_heads_.empty()) rebuild_long_keys();
        return;
    }
    // Reinsert oldest first: hash-chain walks then visit most recent matches
    // first. Absolute positions avoid ambiguous links after ring wraparound.
    const auto first = steps_ < window ? position_ - static_cast<std::uint32_t>(steps_) - 2
                                      : position_ - std::uint32_t(window);
    for (auto p = first; p <= position_ - 3; ++p) {
        const auto slot = std::size_t(p) & mask;
        const auto key = hash3(text_[slot], text_[(slot + 1) & mask], text_[(slot + 2) & mask]);
        previous_[slot] = heads_[key];
        heads_[key] = p;
    }
    if (!long_heads_.empty()) rebuild_long_keys();
}

void SlideEncoder::rebuild_long_keys() {
    std::fill(long_heads_.begin(), long_heads_.end(), 0);
    if (steps_ == 0) return;
    const auto first = steps_ < window ? position_ - static_cast<std::uint32_t>(steps_) - 2
                                      : position_ - std::uint32_t(window);
    for (auto p = first; p <= position_ - 4; ++p) {
        const auto slot = std::size_t(p) & mask;
        const auto key = hash4(text_.data() + slot);
        long_previous_[slot] = long_heads_[key];
        long_heads_[key] = p;
    }
}

void SlideEncoder::rebase() {
    // Keep all live absolute positions positive so zero remains the sentinel.
    const auto shift = (position_ - std::uint32_t(2 * window)) & ~std::uint32_t(mask);
    for (auto& p : heads_) p = p > shift ? p - shift : 0;
    for (auto& p : previous_) p = p > shift ? p - shift : 0;
    for (auto& p : long_heads_) p = p > shift ? p - shift : 0;
    for (auto& p : long_previous_) p = p > shift ? p - shift : 0;
    position_ -= shift;
}

TLG_SLIDE_INLINE std::size_t SlideEncoder::probe(std::span<const std::uint8_t> input,
                                std::size_t slot, std::size_t distance,
                                std::size_t cap, std::size_t best) const {
    if (cap <= best) return 0;
    if (best >= 3) {
        const auto predicted = best < distance ? text_[slot + best] : input[best - distance];
        if (predicted != input[best]) return 0;
    }

    const auto prefix = std::min(distance, cap);
    auto length = common_prefix(text_.data() + slot, input.data(), prefix);
    if (length == distance && length < cap) {
        // A decoder copies one byte at a time. After distance bytes it reads
        // bytes produced by this same token: an input prefix repeats itself.
        length += common_prefix(input.data(), input.data() + distance, cap - distance);
    }
    return length >= 3 && length > best ? length : 0;
}

TLG_SLIDE_INLINE std::pair<std::size_t, std::size_t>
SlideEncoder::find_match(std::span<const std::uint8_t> input) {
    if (input.size() < 3) return {0, 0};
    const auto cap = std::min(longest, input.size());
    std::size_t best = 0, best_slot = 0;

    // Their three-byte keys cross the cursor, so distances 1 and 2 are tested
    // directly. This makes long runs compress immediately after one literal.
    for (std::size_t distance = 1; distance <= 2; ++distance) {
        if (input[0] != input[distance]) continue;
        if (distance == 1 && input[0] != input[2]) continue;
        const auto slot = (std::size_t(position_) - distance) & mask;
        if (input[0] != text_[slot] || (distance == 2 && input[1] != text_[(slot + 1) & mask]))
            continue;
        const auto length = probe(input, slot, distance, cap, best);
        if (length != 0) { best = length; best_slot = slot; }
        if (best == cap) return {best_slot, best};
    }

    if (long_heads_.empty() || best < 3 || cap < 4) {
        auto p = heads_[hash3(input[0], input[1], input[2])];
        p = p >= position_ - static_cast<std::uint32_t>(window) ? p : 0;
        unsigned visited = 0;
        while (p != 0) {
            const auto distance = std::size_t(position_ - p);
            if (distance > window) break;
            const auto slot = std::size_t(p) & mask;
            const auto length = probe(input, slot, distance, cap, best);
            if (length != 0) { best = length; best_slot = slot; }
            if (best == cap) return {best_slot, best};
            // Low-entropy image residuals can put hundreds of positions in a
            // three-byte chain. Add a fourth-byte index only when it earns it;
            // random data and flat runs keep the cheaper single-index push.
            if (match_ == SlideMatch::adaptive && long_heads_.empty() && cap >= 4 &&
                ++visited == 64 && best < 18) {
                // Build both arrays before publishing the cache. A failed
                // allocation must not leave a half-installed index behind.
                std::vector<std::uint32_t> heads(keys), previous(window);
                long_heads_.swap(heads);
                long_previous_.swap(previous);
                rebuild_long_keys();
            }
            if (!long_heads_.empty() && best >= 3 && cap >= 4) break;
            p = previous_[slot];
        }
    }
    if (!long_heads_.empty() && best >= 3 && cap >= 4) {
        // A complete four-byte key cannot represent distance three, where the
        // fourth byte comes from this token. Test it before the ordered chain.
        const auto near_slot = (std::size_t(position_) - 3) & mask;
        const auto near_length = probe(input, near_slot, 3, cap, best);
        if (near_length != 0) { best = near_length; best_slot = near_slot; }
        if (best == cap) return {best_slot, best};
        auto p = long_heads_[hash4(input.data())];
        p = p >= position_ - static_cast<std::uint32_t>(window) ? p : 0;
        while (p != 0) {
            const auto distance = std::size_t(position_ - p);
            if (distance > window) break;
            const auto slot = std::size_t(p) & mask;
            const auto length = probe(input, slot, distance, cap, best);
            if (length != 0) { best = length; best_slot = slot; }
            if (best == cap) return {best_slot, best};
            p = long_previous_[slot];
        }
    }
    // The cold part of a zero-initialized dictionary has no hash nodes yet.
    // All its three-byte keys are zero, except keys crossing the ring boundary.
    // Avoid building 4096 redundant nodes after every incompressible raw plane.
    if (steps_ < window && input[0] == 0) {
        const auto cursor = std::size_t(position_) & mask;
        if (input[1] == 0 && steps_ <= window - 2) {
            const auto first = input[2] == 0 ? cursor : window - 2;
            for (auto slot = first; slot <= window - 2; ++slot) {
                const auto distance = window + cursor - slot;
                const auto length = probe(input, slot, distance, cap, best);
                if (length != 0) { best = length; best_slot = slot; }
                if (best == cap) return {best_slot, best};
            }
        }
        const auto slot = window - 1;
        const auto length = probe(input, slot, cursor + 1, cap, best);
        if (length != 0) { best = length; best_slot = slot; }
    }
    return {best_slot, best};
}

std::vector<std::uint8_t> SlideEncoder::encode(std::span<const std::uint8_t> input) {
    if (parse_ == SlideParse::optimal) return encode_optimal(input);
    std::vector<std::uint8_t> output;
    const auto flags = input.size() / 8 + (input.size() % 8 != 0);
    require(input.size() <= output.max_size() - flags, ErrorCode::resource_limit,
            "Slide input is too large");
    output.resize(input.size() + flags);
    std::size_t written = 0;

    std::array<std::uint8_t, 25> group{}; // 1 flag + 8 long-form tokens
    std::size_t used = 1;
    unsigned flag = 1;
    std::size_t i = 0;
    while (i < input.size()) {
        if (position_ >= (1u << 31)) rebase();
        const auto [slot, length] = find_match(input.subspan(i));
        if (length != 0) {
            group[0] |= std::uint8_t(flag);
            group[used++] = std::uint8_t(slot);
            group[used++] = std::uint8_t((slot >> 8) | ((length >= 18 ? 15 : length - 3) << 4));
            if (length >= 18) group[used++] = std::uint8_t(length - 18);
            if (long_heads_.empty()) {
                for (std::size_t n = 0; n < length; ++n) push_byte<false>(input[i++]);
            } else {
                for (std::size_t n = 0; n < length; ++n) push_byte<true>(input[i++]);
            }
        } else {
            group[used++] = input[i];
            push(input[i++]);
        }

        flag <<= 1;
        if (flag == 256) {
            std::memcpy(output.data() + written, group.data(), used);
            written += used;
            group[0] = 0;
            used = 1;
            flag = 1;
        }
    }
    if (flag != 1) {
        std::memcpy(output.data() + written, group.data(), used);
        written += used;
    }
    output.resize(written);
    return output;
}

std::vector<std::uint8_t> SlideEncoder::encode_optimal(std::span<const std::uint8_t> input) {
    // A token length needs nine bits, not sixteen. Keep the slot separately
    // to avoid struct padding: 2N + 9N bytes instead of the old 20N.
    // Before the backward pass, the first two trace bytes hold the match cap.
    using Trace = std::array<std::uint8_t, 9>;
    std::vector<std::uint8_t> output;
    std::vector<std::uint16_t> slots;
    std::vector<Trace> trace;
    const auto flags = input.size() / 8 + (input.size() % 8 != 0);
    require(input.size() <= output.max_size() - flags && input.size() <= slots.max_size() &&
            input.size() <= trace.max_size(),
            ErrorCode::resource_limit, "Slide optimal input is too large");
    if (input.empty()) return output;
    require(max_working_bytes_ >= ParseMinimum::working_bytes &&
            input.size() <= (max_working_bytes_ - ParseMinimum::working_bytes) /
                (sizeof(std::uint16_t) + sizeof(Trace)),
            ErrorCode::resource_limit, "Slide optimal scratch exceeds working memory limit");
    slots.resize(input.size());
    trace.resize(input.size());
    output.resize(input.size() + flags);
    ParseMinimum minimum;

    // Dictionary state depends on consumed bytes, not on how we tokenize them.
    // One longest match per position therefore supplies every legal prefix.
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (position_ >= (1u << 31)) rebase();
        const auto [slot, length] = find_match(input.subspan(i));
        slots[i] = static_cast<std::uint16_t>(slot);
        trace[i][0] = static_cast<std::uint8_t>(length);
        trace[i][1] = static_cast<std::uint8_t>(length >> 8);
        push(input[i]);
    }

    std::array<std::size_t, 8> costs{};
    minimum.put(input.size(), costs);
    for (auto i = input.size(); i-- != 0;) {
        const auto literal = minimum.query(i + 1, i + 1);
        const auto cap = std::size_t{trace[i][0]} | (std::size_t{trace[i][1]} << 8);
        trace[i].fill(0);
        const auto short_match = cap >= 3 ? minimum.query(i + 3, i + std::min<std::size_t>(17, cap))
                                           : ParseMinimum::Node{};
        const auto long_match = cap >= 18 ? minimum.query(i + 18, i + cap)
                                         : ParseMinimum::Node{};
        for (std::size_t phase = 0; phase < 8; ++phase) {
            const auto next = (phase + 1) & 7;
            const auto flag_cost = std::size_t(phase == 0);
            costs[phase] = literal.cost[next] + 1 + flag_cost;
            auto length = std::size_t{1};
            const auto consider = [&](const ParseMinimum::Node& match, std::size_t token_cost) {
                const auto candidate = match.cost[next] + token_cost + flag_cost;
                const auto candidate_length = match.position[next] - i;
                if (candidate < costs[phase] || (candidate == costs[phase] && candidate_length > length)) {
                    costs[phase] = candidate;
                    length = candidate_length;
                }
            };
            // 贪心只顾眼前最长，明天可能就亏了；flag byte 也要算，不能逃单。
            if (cap >= 3) consider(short_match, 2);
            if (cap >= 18) consider(long_match, 3);
            trace[i][phase] |= static_cast<std::uint8_t>(length << phase);
            trace[i][phase + 1] |= static_cast<std::uint8_t>(length >> (8 - phase));
        }
        minimum.put(i, costs);
    }

    std::size_t written = 0, flag_position = 0, phase = 0;
    for (std::size_t i = 0; i < input.size();) {
        if (phase == 0) {
            flag_position = written++;
            output[flag_position] = 0;
        }
        const auto length = ((std::size_t{trace[i][phase]} |
            (std::size_t{trace[i][phase + 1]} << 8)) >> phase) & 511;
        if (length == 1) output[written++] = input[i];
        else {
            const auto slot = slots[i];
            output[flag_position] |= static_cast<std::uint8_t>(1u << phase);
            output[written++] = static_cast<std::uint8_t>(slot);
            output[written++] = static_cast<std::uint8_t>((slot >> 8) |
                ((length >= 18 ? 15 : length - 3) << 4));
            if (length >= 18) output[written++] = static_cast<std::uint8_t>(length - 18);
        }
        i += length;
        phase = (phase + 1) & 7;
    }
    output.resize(written);
    return output;
}

void SlideEncoder::store() {
    std::copy_n(text_.begin(), window, saved_text_.begin());
    saved_cursor_ = std::size_t(position_) & mask;
    saved_steps_ = steps_;
    has_snapshot_ = true;
}

void SlideEncoder::restore() {
    require(has_snapshot_, ErrorCode::invalid_argument, "Slide restore requires a stored dictionary");
    std::copy(saved_text_.begin(), saved_text_.end(), text_.begin());
    std::copy_n(text_.begin(), longest - 1, text_.begin() + window);
    if (saved_steps_ == 0 && match_ == SlideMatch::adaptive) {
        // Empty rollback can expire the entire index by age instead of clearing
        // 32768 buckets. Every old node is now more than one ring behind us.
        // 常见的随机 raw plane 不值得把这张大表反复擦成白板。
        if (position_ >= (1u << 31)) rebase();
        position_ = ((position_ + static_cast<std::uint32_t>(mask)) &
                     ~static_cast<std::uint32_t>(mask)) + static_cast<std::uint32_t>(window);
        steps_ = 0;
        return;
    }
    position_ = std::uint32_t(2 * window + saved_cursor_);
    steps_ = saved_steps_;
    rebuild();
}

SlideDecoder::SlideDecoder(std::span<const std::uint8_t> dictionary, SlideDecode algorithm)
    : algorithm_(algorithm) {
    require(dictionary.empty() || dictionary.size() == text_.size(), ErrorCode::invalid_argument,
            "Slide dictionary must be empty or exactly 4096 bytes");
    require(algorithm == SlideDecode::bulk || algorithm == SlideDecode::scalar,
            ErrorCode::invalid_argument, "unknown Slide decode algorithm");
    std::copy(dictionary.begin(), dictionary.end(), text_.begin());
}

void validate_slide(std::span<const std::uint8_t> input, std::size_t exact_output) {
    ByteReader reader(input);
    std::size_t written = 0;
    while (written < exact_output) {
        const auto flags = reader.u8();
        if (flags == 0 && exact_output - written >= 8) {
            reader.take(8);
            written += 8;
            continue;
        }
        for (unsigned bit = 0; bit < 8 && written < exact_output; ++bit) {
            std::size_t length = 1;
            if ((flags & (1u << bit)) != 0) {
                reader.u8();
                const auto high = reader.u8();
                length = std::size_t(high >> 4) + 3;
                if ((high >> 4) == 15) length = 18 + reader.u8();
                require(length <= exact_output - written, ErrorCode::corrupt_data,
                        "Slide match exceeds the expected output size", reader.position());
            } else {
                reader.u8();
            }
            written += length;
        }
    }
    require(reader.empty(), ErrorCode::corrupt_data,
            "Slide stream has bytes beyond its expected output", reader.position());
}

std::vector<std::uint8_t> SlideDecoder::decode(std::span<const std::uint8_t> input,
                                             std::size_t exact_output) {
    validate_slide(input, exact_output);
    return decode_prevalidated(input, exact_output);
}

std::vector<std::uint8_t> SlideDecoder::decode_prevalidated(std::span<const std::uint8_t> input,
                                                          std::size_t exact_output) {
    if (algorithm_ == SlideDecode::scalar) return decode_scalar(input, exact_output);
    std::vector<std::uint8_t> output;
    require(exact_output <= output.max_size(), ErrorCode::resource_limit, "Slide output is too large");
    output.resize(exact_output);
    // Framing and all lengths were checked before allocation or state changes.
    // Output itself is the evolving dictionary; commit only the final ring tail.
    const auto initial_cursor = cursor_;
    auto* out = output.data();
    std::size_t at = 0, written = 0;
    while (written < exact_output) {
        const auto flags = input[at++];
        if (flags == 0 && exact_output - written >= 8) {
            std::memcpy(out + written, input.data() + at, 8);
            at += 8;
            written += 8;
            continue;
        }
        for (unsigned bit = 0; bit < 8 && written < exact_output; ++bit) {
            if ((flags & (1u << bit)) == 0) {
                out[written++] = input[at++];
                continue;
            }
            const auto low = input[at++];
            const auto high = input[at++];
            const auto slot = std::size_t(low | ((high & 15u) << 8));
            auto remaining = std::size_t(high >> 4) + 3;
            if ((high >> 4) == 15) remaining = 18 + input[at++];
            auto distance = ((initial_cursor + written) - slot) & 4095;
            if (distance == 0) distance = text_.size();

            if (written < distance) {
                const auto old = std::min(remaining, distance - written);
                const auto first = std::min(old, text_.size() - slot);
                std::memcpy(out + written, text_.data() + slot, first);
                if (old > first) std::memcpy(out + written + first, text_.data(), old - first);
                written += old;
                remaining -= old;
            }
            if (remaining == 0) continue;
            const auto source = written - distance;
            if (distance == 1) {
                std::memset(out + written, out[source], remaining);
                written += remaining;
                continue;
            }
            // Seed one period, then double the known prefix. Every memcpy is
            // disjoint and exact-sized. 偷读八个字节也许快，越界了就不酷了。
            auto count = std::min(distance, remaining);
            std::memcpy(out + written, out + source, count);
            written += count;
            remaining -= count;
            while (remaining != 0) {
                count = std::min(written - source, remaining);
                std::memcpy(out + written, out + source, count);
                written += count;
                remaining -= count;
            }
        }
    }
    const auto tail = std::min(exact_output, text_.size());
    if (tail != 0) {
        const auto slot = (initial_cursor + exact_output - tail) & 4095;
        const auto first = std::min(tail, text_.size() - slot);
        std::memcpy(text_.data() + slot, out + exact_output - tail, first);
        if (tail > first) std::memcpy(text_.data(), out + exact_output - tail + first, tail - first);
    }
    cursor_ = (initial_cursor + exact_output) & 4095;
    return output;
}

std::vector<std::uint8_t> SlideDecoder::decode_scalar(std::span<const std::uint8_t> input,
                                                    std::size_t exact_output) {
    ByteReader reader(input);
    std::vector<std::uint8_t> output;
    require(exact_output <= output.max_size(), ErrorCode::resource_limit, "Slide output is too large");
    output.resize(exact_output);
    std::size_t written = 0;
    while (written < exact_output) {
        const auto flags = reader.u8();
        for (unsigned bit = 0; bit < 8 && written < exact_output; ++bit) {
            if ((flags & (1u << bit)) != 0) {
                const auto low = reader.u8();
                const auto high = reader.u8();
                auto slot = std::size_t(low | ((high & 15u) << 8));
                auto length = std::size_t(high >> 4) + 3;
                if ((high >> 4) == 15) length = 18 + reader.u8();
                require(length <= exact_output - written, ErrorCode::corrupt_data,
                        "Slide match exceeds the expected output size", reader.position());
                for (std::size_t n = 0; n < length; ++n) {
                    const auto value = text_[slot];
                    output[written++] = value;
                    text_[cursor_] = value;
                    cursor_ = (cursor_ + 1) & 4095;
                    slot = (slot + 1) & 4095;
                }
            } else {
                const auto value = reader.u8();
                output[written++] = value;
                text_[cursor_] = value;
                cursor_ = (cursor_ + 1) & 4095;
            }
        }
    }
    require(reader.empty(), ErrorCode::corrupt_data,
            "Slide stream has bytes beyond its expected output", reader.position());
    return output;
}

} // namespace tlg::detail

#undef TLG_SLIDE_INLINE
