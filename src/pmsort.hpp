/**
 * @file pms.hpp
 * @brief pms: partition merge sort
 *
 * Require: C++17+
 *
 * Usage:
 *   pmsort(first, last);
 *   pmsort(first, last, comparator);
 *   pmsort(container);
 *   pmsort(container, comparator);
 */
#ifndef PMS_HPP
#define PMS_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#if __has_include(<version>)
#include <version>
#endif

namespace pms_detail {

inline constexpr std::size_t kInsertionBlock      = 32;   // Block size for insertion sort
inline constexpr std::size_t kMaxPartitions       = 256;  // Max partitions per split (power of 2)
inline constexpr std::size_t kSamplesPerPartition = 16;
inline constexpr int         kMaxDepth            = 48;   // Recursion limit, deeper ranges fall back to merge
inline constexpr std::size_t kMaxRuns             = 16;   // At most this many natural runs: merge them directly

// Element types handled by Sorter; other types use std::stable_sort.
// Move-only types must be nothrow-movable, so an exception cannot lose elements.
template <class T>
inline constexpr bool is_supported_v =
    std::is_default_constructible_v<T> &&
    std::is_move_constructible_v<T> &&
    std::is_move_assignable_v<T> &&
    (std::is_copy_assignable_v<T> ||
     (std::is_nothrow_move_constructible_v<T> && std::is_nothrow_move_assignable_v<T>));

// Returns true if the range is already sorted, or was strictly descending and got reversed.
// Random data exits after a few elements. Only strictly descending ranges are reversed,
// so equal elements never change order.
template <class It, class Less>
bool finishIfPresorted(It first, It last, const Less& less) {
    using Diff = typename std::iterator_traits<It>::difference_type;
    const Diff count = last - first;
    if (count < 2) return true;

    if (less(first[1], first[0])) {
        for (Diff i = 2; i < count; ++i)
            if (!less(first[i], first[i - 1])) return false;
        std::reverse(first, last);
        return true;
    }

    for (Diff i = 2; i < count; ++i)
        if (less(first[i], first[i - 1])) return false;
    return true;
}

template <class T, class Less>
class Sorter {
    static_assert(is_supported_v<T>, "pms_detail::Sorter: unsupported element type");

    // Ranges up to this size are sorted by insertion sort + merge
    static constexpr std::size_t kLeafSize =
        std::max<std::size_t>(kMaxPartitions * kSamplesPerPartition, (64 * 1024) / sizeof(T));

    // Copyable types keep splitters and samples by value (cache friendly, no indirection).
    // Move-only types keep pointers into the source range, which stay valid until the scatter step.
    static constexpr bool kCopyKeys = std::is_copy_assignable_v<T>;
    using Key = std::conditional_t<kCopyKeys, T, const T*>;

    static const T& keyRef(const Key& key) {
        if constexpr (kCopyKeys) return key; else return *key;
    }
    static void setKey(Key& dst, const T& value) {
        if constexpr (kCopyKeys) dst = value; else dst = &value;
    }

    // Splitter tree in Eytzinger layout; views memory owned by Sorter
    struct SplitterTree {
        Key* nodes  = nullptr;      // Tree layout, index 1 is the root
        Key* sorted = nullptr;      // Splitters in sorted order
        unsigned rangeCount = 2;    // Ranges between splitters (power of 2)
        unsigned depth      = 1;    // Tree levels: log2(rangeCount)

        // Converts a tree leaf into a partition id
        inline unsigned leafToId(unsigned leaf, const T& value, const Less& less) const {
            unsigned below = leaf - rangeCount;                                                    // Splitters smaller than value
            unsigned equal = (below < rangeCount - 1) & (!less(value, keyRef(sorted[below])));     // Equals a splitter?
            return 2 * below + equal;                                                              // Even id: range, odd id: equal to a splitter
        }

        inline bool inPartition(unsigned id, const T& value, const Less& less) const {
            unsigned index = id >> 1;
            if (id & 1) return !less(value, keyRef(sorted[index])) && !less(keyRef(sorted[index]), value);
            return (index == 0 || less(keyRef(sorted[index - 1]), value)) &&
                   (index == rangeCount - 1 || less(value, keyRef(sorted[index])));
        }

        inline unsigned classify(const T& value, const Less& less) const {
            unsigned node = 1;
            for (unsigned level = 0; level < depth; ++level)
                node = 2 * node + (less(keyRef(nodes[node]), value) ? 1u : 0u);
            return leafToId(node, value, less);
        }

        // Four independent walks so the CPU can overlap them
        inline void classifyFour(const T* src, unsigned* ids, const Less& less) const {
            unsigned n0 = 1, n1 = 1, n2 = 1, n3 = 1;

            for (unsigned level = 0; level < depth; ++level) {
                n0 = 2 * n0 + (less(keyRef(nodes[n0]), src[0]) ? 1u : 0u);
                n1 = 2 * n1 + (less(keyRef(nodes[n1]), src[1]) ? 1u : 0u);
                n2 = 2 * n2 + (less(keyRef(nodes[n2]), src[2]) ? 1u : 0u);
                n3 = 2 * n3 + (less(keyRef(nodes[n3]), src[3]) ? 1u : 0u);
            }

            ids[0] = leafToId(n0, src[0], less);
            ids[1] = leafToId(n1, src[1], less);
            ids[2] = leafToId(n2, src[2], less);
            ids[3] = leafToId(n3, src[3], less);
        }
    };

    T* data_;
    std::size_t size_;
    Less less_;

    std::unique_ptr<T[]> scratch_;                    // Second buffer (size_ elements)
    std::unique_ptr<std::uint16_t[]> partitionIds_;   // Partition id of each element (only when size_ > kLeafSize)
    std::unique_ptr<Key[]> splitterStore_;              // Tree, splitters and samples, shared by all levels
    SplitterTree splitterTree_;
    Key* samples_ = nullptr;
    std::vector<std::size_t> runBounds_;              // Run boundaries: 0, end of run 1, end of run 2, ...
    std::size_t partitionCounts_[2 * kMaxPartitions];
    std::size_t writePos_[2 * kMaxPartitions];

    // Number of partitions for a range of `count` elements (power of 2, at most kMaxPartitions)
    static std::size_t choosePartitionCount(std::size_t count) {
        const std::size_t halfLeaf = kLeafSize / 2;
        const std::size_t wanted = (count + halfLeaf - 1) / halfLeaf;
        std::size_t partitions = 2;
        while (partitions < wanted && partitions < kMaxPartitions) partitions <<= 1;
        return partitions;
    }

    static void buildTree(SplitterTree& tree, unsigned node, int lo, int hi) {
        if (lo > hi) return;

        int mid = (lo + hi) / 2;
        tree.nodes[node] = tree.sorted[mid];

        buildTree(tree, 2 * node, lo, mid - 1);
        buildTree(tree, 2 * node + 1, mid + 1, hi);
    }

    // Deterministic stratified sampling: split [0, count) into sampleCount equal strips and take
    // one sample per strip at a golden-ratio (Weyl) offset. Reads are sequential and there is
    // no RNG, so the samples depend only on count and sampleCount.
    void takeSamples(const T* src, std::size_t count, std::size_t sampleCount) {
        const std::uint64_t stride = count / sampleCount;
        const std::uint64_t span   = std::min<std::uint64_t>(stride, 0xFFFFFFFFull);
        std::uint64_t weyl = 0;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            weyl += 0x9E3779B97F4A7C15ull;
            const std::uint64_t offset = ((weyl >> 32) * span) >> 32;   // In [0, span)
            setKey(samples_[i], src[i * stride + offset]);
        }
    }

    // splitterTree_ is only used before recursing, so all levels can share it
    void buildSplitters(const T* src, std::size_t count) {
        SplitterTree& tree = splitterTree_;
        const std::size_t partitions  = choosePartitionCount(count);
        const std::size_t sampleCount = partitions * kSamplesPerPartition;

        takeSamples(src, count, sampleCount);
        if constexpr (kCopyKeys)
            std::sort(samples_, samples_ + sampleCount, less_);
        else
            std::sort(samples_, samples_ + sampleCount,
                      [this](const Key& a, const Key& b) { return less_(*a, *b); });

        unsigned splitterCount = 0;
        for (std::size_t i = 0; i + 1 < partitions; ++i) {
            const Key& candidate = samples_[(i + 1) * sampleCount / partitions];

            if (splitterCount == 0 || less_(keyRef(tree.sorted[splitterCount - 1]), keyRef(candidate)))
                tree.sorted[splitterCount++] = candidate;  // Skip duplicate splitters
        }

        unsigned rangeCount = 2;
        while (rangeCount < splitterCount + 1) rangeCount <<= 1;

        for (unsigned i = splitterCount; i < rangeCount; ++i)
            tree.sorted[i] = tree.sorted[splitterCount - 1];  // Pad to a full tree

        tree.rangeCount = rangeCount;
        tree.depth = 0;

        while ((1u << tree.depth) < rangeCount) ++tree.depth;

        buildTree(tree, 1, 0, (int)rangeCount - 2);
    }

    // Assigns a partition id to every element and counts elements per partition
    void classifyAll(const T* src, std::size_t count, std::uint16_t* ids, std::size_t* counts) const {
        const SplitterTree& tree = splitterTree_;
        std::size_t i = 0;
        unsigned ids4[4], prevId = ~0u;

        for (; i + 4 <= count; i += 4) {
            // Fast path: the next 4 elements stay in the previous partition
            if (prevId != ~0u &&
                tree.inPartition(prevId, src[i], less_)     && tree.inPartition(prevId, src[i + 1], less_) &&
                tree.inPartition(prevId, src[i + 2], less_) && tree.inPartition(prevId, src[i + 3], less_)) {
                for (int j = 0; j < 4; ++j) ids[i + j] = (std::uint16_t)prevId;
                counts[prevId] += 4;
                continue;
            }
            tree.classifyFour(src + i, ids4, less_);
            for (int j = 0; j < 4; ++j) { ids[i + j] = (std::uint16_t)ids4[j]; ++counts[ids4[j]]; }
            prevId = ids4[3];
        }
        for (; i < count; ++i) {
            unsigned id = tree.classify(src[i], less_);
            ids[i] = (std::uint16_t)id; ++counts[id];
        }
    }

    // Stable insertion sort of data[lo, hi)
    void insertion(T* data, std::size_t lo, std::size_t hi) {
        for (std::size_t i = lo + 1; i < hi; ++i) {
            T key = std::move(data[i]);
            std::size_t j = i;

            while (j > lo && less_(key, data[j - 1])) {
                data[j] = std::move(data[j - 1]);
                --j;
            }

            data[j] = std::move(key);
        }
    }

    // Merges [leftPos, leftEnd) and [rightPos, rightEnd) from src into dst starting at outPos.
    // Ties take the left side (stable).
    void mergeRange(
        T* src, T* dst,
        std::size_t leftPos, std::size_t leftEnd,
        std::size_t rightPos, std::size_t rightEnd,
        std::size_t outPos
    ) {
        while (leftPos < leftEnd && rightPos < rightEnd) {
            bool takeRight = less_(src[rightPos], src[leftPos]);
            dst[outPos++] = std::move(takeRight ? src[rightPos] : src[leftPos]);
            rightPos += takeRight;
            leftPos += !takeRight;
        }

        while (leftPos < leftEnd) dst[outPos++] = std::move(src[leftPos++]);
        while (rightPos < rightEnd) dst[outPos++] = std::move(src[rightPos++]);
    }

    void merge(T* src, T* dst, std::size_t lo, std::size_t mid, std::size_t hi) {
        mergeRange(src, dst, lo, mid, mid, hi, lo);
    }

    // Two independent merges interleaved to overlap their latency
    void mergeTwo(
        T* src, T* dst,
        std::size_t loA, std::size_t midA, std::size_t hiA,
        std::size_t loB, std::size_t midB, std::size_t hiB
    ) {
        std::size_t leftA = loA, rightA = midA, outA = loA;
        std::size_t leftB = loB, rightB = midB, outB = loB;

        while (leftA < midA && rightA < hiA && leftB < midB && rightB < hiB) {
            bool takeRightA = less_(src[rightA], src[leftA]);
            dst[outA++] = std::move(takeRightA ? src[rightA] : src[leftA]);
            rightA += takeRightA;
            leftA += !takeRightA;

            bool takeRightB = less_(src[rightB], src[leftB]);
            dst[outB++] = std::move(takeRightB ? src[rightB] : src[leftB]);
            rightB += takeRightB;
            leftB += !takeRightB;
        }

        mergeRange(src, dst, leftA, midA, rightA, hiA, outA);
        mergeRange(src, dst, leftB, midB, rightB, hiB, outB);
    }

    // Random data: insertion sort on blocks, then bottom-up merge between two buffers
    void blockMergeSort(T* data, T* scratch, std::size_t count, T* result) {
        if (count == 0) return;

        for (std::size_t lo = 0; lo < count; lo += kInsertionBlock)
            insertion(data, lo, std::min(lo + kInsertionBlock, count));

        T* src = data;
        T* dst = scratch;

        for (std::size_t width = kInsertionBlock; width < count; width *= 2) {
            for (std::size_t lo = 0; lo < count; lo += 4 * width) {
                std::size_t midA = std::min(lo + width, count);
                std::size_t hiA  = std::min(lo + 2 * width, count);

                std::size_t loB  = lo + 2 * width;
                std::size_t midB = std::min(loB + width, count);
                std::size_t hiB  = std::min(loB + 2 * width, count);

                bool needMergeA = midA < hiA && less_(src[midA], src[midA - 1]);
                bool needMergeB = loB < count && midB < hiB && less_(src[midB], src[midB - 1]);

                if (needMergeA && needMergeB) {
                    mergeTwo(src, dst, lo, midA, hiA, loB, midB, hiB);
                    continue;
                }

                if (needMergeA)
                    merge(src, dst, lo, midA, hiA);
                else
                    std::move(src + lo, src + hiA, dst + lo);  // Already in order

                if (loB < count) {
                    if (needMergeB)
                        merge(src, dst, loB, midB, hiB);
                    else
                        std::move(src + loB, src + hiB, dst + loB);
                }
            }

            std::swap(src, dst);
        }

        if (src != result)
            std::move(src, src + count, result);
    }

    // Merges data[lo, mid) and data[mid, hi) in place; buf holds the left half. Uses galloping.
    void mergeInPlace(T* data, T* buf, std::size_t lo, std::size_t mid, std::size_t hi) {
        if (!less_(data[mid], data[mid - 1])) return;                       // Already in order
        lo = std::upper_bound(data + lo, data + mid, data[mid], less_) - data;      // Skip left prefix already in place
        hi = std::lower_bound(data + mid, data + hi, data[mid - 1], less_) - data;  // Skip right suffix already in place

        const std::size_t leftCount = mid - lo;
        std::move(data + lo, data + mid, buf);

        std::size_t leftPos = 0, rightPos = mid, outPos = lo;
        std::size_t leftWins = 0, rightWins = 0;
        constexpr std::size_t kGallopThreshold = 7;

        while (leftPos < leftCount && rightPos < hi) {
            while (leftPos < leftCount && rightPos < hi) {                  // Element by element
                if (less_(data[rightPos], buf[leftPos])) {
                    data[outPos++] = std::move(data[rightPos++]);
                    ++rightWins; leftWins = 0;
                } else {
                    data[outPos++] = std::move(buf[leftPos++]);
                    ++leftWins; rightWins = 0;
                }
                if (leftWins >= kGallopThreshold || rightWins >= kGallopThreshold) break;
            }
            if (leftPos >= leftCount || rightPos >= hi) break;

            // Galloping: move whole groups (stable: left <= data[rightPos] goes first)
            std::size_t leftRunEnd =
                std::upper_bound(buf + leftPos, buf + leftCount, data[rightPos], less_) - buf;
            std::move(buf + leftPos, buf + leftRunEnd, data + outPos);
            outPos += leftRunEnd - leftPos;
            leftPos = leftRunEnd;
            if (leftPos >= leftCount) break;

            std::size_t rightRunEnd =
                std::lower_bound(data + rightPos, data + hi, buf[leftPos], less_) - data;
            std::move(data + rightPos, data + rightRunEnd, data + outPos);
            outPos += rightRunEnd - rightPos;
            rightPos = rightRunEnd;
            leftWins = rightWins = 0;
        }
        while (leftPos < leftCount) data[outPos++] = std::move(buf[leftPos++]);  // Rest of the right side is in place
    }

    // True if the whole range has at most kMaxRuns natural runs (non-decreasing or strictly
    // decreasing). Exits early, so random data costs about 2 * kMaxRuns comparisons.
    bool fewRuns() const {
        std::size_t runs = 0;
        for (std::size_t start = 0; start < size_;) {
            std::size_t end = start + 1;
            if (end < size_) {
                if (less_(data_[end], data_[end - 1])) { while (++end < size_ && less_(data_[end], data_[end - 1])) {} }
                else                                   { while (++end < size_ && !less_(data_[end], data_[end - 1])) {} }
            }
            if (++runs > kMaxRuns) return false;
            start = end;
        }
        return true;
    }

    // Leaf range: merge natural runs for structured data, merge sort blocks for random data.
    // Always O(count log count), so it is also the fallback used by partitionSort.
    void sortLeaf(T* data, T* scratch, std::size_t count, T* result) {
        if (count == 0) return;

        // Many short runs means random-looking data
        {
            const std::size_t limit = count / 16;
            std::size_t descents = 0, ascents = 0;
            for (std::size_t i = 1; i < count; ++i) {
                if (less_(data[i], data[i - 1])) ++descents; else ++ascents;
                if (descents > limit && ascents > limit) {
                    blockMergeSort(data, scratch, count, result);
                    return;
                }
            }
        }

        mergeRuns(data, scratch, count, result);
    }

    // Splits data[0, count) into natural runs and merges them pairwise; the result goes to `result`
    void mergeRuns(T* data, T* scratch, std::size_t count, T* result) {
        // Strictly descending runs are reversed (stable); short runs are extended by insertion sort
        runBounds_.assign(1, 0);
        for (std::size_t start = 0; start < count;) {
            std::size_t end = start + 1;
            if (end < count) {
                if (less_(data[end], data[end - 1])) {
                    while (++end < count && less_(data[end], data[end - 1])) {}
                    std::reverse(data + start, data + end);
                } else {
                    while (++end < count && !less_(data[end], data[end - 1])) {}
                }
            }
            if (end - start < kInsertionBlock) {
                end = std::min(start + kInsertionBlock, count);
                insertion(data, start, end);
            }
            runBounds_.push_back(end);
            start = end;
        }

        while (runBounds_.size() > 2) {                                     // One merge level per pass
            std::size_t kept = 1;
            for (std::size_t i = 0; i + 1 < runBounds_.size(); i += 2) {
                if (i + 2 < runBounds_.size()) {
                    mergeInPlace(data, scratch, runBounds_[i], runBounds_[i + 1], runBounds_[i + 2]);
                    runBounds_[kept++] = runBounds_[i + 2];
                } else {
                    runBounds_[kept++] = runBounds_[i + 1];
                }
            }
            runBounds_.resize(kept);
        }

        if (result != data) std::move(data, data + count, result);
    }

    // Stable out-of-place partitioning; the two buffers swap roles at each level
    void partitionSort(T* src, T* dst, std::size_t lo, std::size_t hi, int depth) {
        const std::size_t count = hi - lo;

        if (count <= kLeafSize || depth >= kMaxDepth) {
            sortLeaf(src + lo, dst + lo, count, data_ + lo);
            return;
        }

        buildSplitters(src + lo, count);

        const std::size_t idCount = 2 * splitterTree_.rangeCount;
        std::fill_n(partitionCounts_, idCount, std::size_t{0});

        std::uint16_t* ids = partitionIds_.get() + lo;
        classifyAll(src + lo, count, ids, partitionCounts_);

        // Guard: with good samples the largest range partition is about count / partitions (at
        // least 4 partitions). If it exceeds count / 2 the samples were misleading, so sort this
        // range by merging. Equal partitions (odd ids) are never split further, so they are not counted.
        {
            std::size_t largest = 0;
            for (std::size_t id = 0; id < idCount; id += 2)
                largest = std::max(largest, partitionCounts_[id]);
            if (largest > count / 2) {
                sortLeaf(src + lo, dst + lo, count, data_ + lo);
                return;
            }
        }

        std::size_t starts[2 * kMaxPartitions + 1];  // Only this array lives across recursion (4 KB per level)
        std::size_t offset = 0;
        for (std::size_t id = 0; id < idCount; ++id) {
            starts[id] = writePos_[id] = offset;
            offset += partitionCounts_[id];
        }
        starts[idCount] = offset;

        T* in = src + lo;
        T* out = dst + lo;

        for (std::size_t i = 0; i < count; ++i)
            out[writePos_[ids[i]]++] = std::move(in[i]);  // Keeps the original order (stable)

        for (std::size_t id = 0; id < idCount; ++id) {
            if (starts[id + 1] == starts[id]) continue;

            const std::size_t partLo = lo + starts[id];
            const std::size_t partHi = lo + starts[id + 1];

            if (id & 1) {  // All elements equal: done
                if (dst != data_)
                    std::move(dst + partLo, dst + partHi, data_ + partLo);
            } else {
                partitionSort(dst, src, partLo, partHi, depth + 1);
            }
        }
    }

public:
    Sorter(T* data, std::size_t size, Less less)
        : data_(data), size_(size), less_(std::move(less)), scratch_(new T[size]) {}

    Sorter(const Sorter&) = delete;
    Sorter& operator=(const Sorter&) = delete;

    void run() {
        if (size_ <= 1) return;

        // Few natural runs: merge them directly and skip partitioning
        if (fewRuns()) {
            mergeRuns(data_, scratch_.get(), size_, data_);
            return;
        }

        if (size_ > kLeafSize) {  // Partitioning memory is only needed for large ranges
            const std::size_t partitions = choosePartitionCount(size_);
            partitionIds_.reset(new std::uint16_t[size_]);
            // tree nodes[partitions] + sorted splitters[partitions] + samples[partitions * kSamplesPerPartition]
            splitterStore_.reset(new Key[partitions * (2 + kSamplesPerPartition)]);
            splitterTree_.nodes  = splitterStore_.get();
            splitterTree_.sorted = splitterTree_.nodes + partitions;
            samples_             = splitterTree_.sorted + partitions;
        }
        partitionSort(data_, scratch_.get(), 0, size_, 0);
    }
};

// True if It points into contiguous memory: raw pointers, std::contiguous_iterator (C++20),
// or vector / string iterators (C++17). vector<bool> is excluded: its iterator is a proxy.
template <class It, class V = typename std::iterator_traits<It>::value_type>
inline constexpr bool is_contiguous_v =
    std::is_pointer_v<It>
#if defined(__cpp_lib_concepts)
    || std::contiguous_iterator<It>
#endif
    || (!std::is_same_v<V, bool> &&
        (std::is_same_v<It, typename std::vector<V>::iterator> ||
         std::is_same_v<It, std::string::iterator>));

// Detects iterator types, so the container overload is skipped for pmsort(first, last)
template <class T, class = void>
struct is_iterator : std::false_type {};
template <class T>
struct is_iterator<T, std::void_t<typename std::iterator_traits<T>::iterator_category>>
    : std::true_type {};
template <class T>
inline constexpr bool is_iterator_v = is_iterator<T>::value;

// bool needs no sorting algorithm: count the true values and write the result back
template <class It, class Less>
void sortBools(It first, It last, const Less& less) {
    const bool falseFirst = less(false, true);
    if (!falseFirst && !less(true, false)) return;  // Both values are equivalent

    const auto trues  = std::count(first, last, true);
    const auto falses = (last - first) - trues;
    if (falseFirst) {
        std::fill(first, first + falses, false);
        std::fill(first + falses, last, true);
    } else {
        std::fill(first, first + trues, true);
        std::fill(first + trues, last, false);
    }
}

} // namespace pms_detail

/**
 * Stable sort of [first, last) by `less`.
 *
 * Requirements:
 *  - Random-access iterators; `less` is copyable and callable as const.
 *  - Element type is default-constructible, move-constructible and move-assignable, and either
 *    copy-assignable or nothrow-movable (so move-only types work). Other types fall back to
 *    std::stable_sort.
 *  - bool is sorted by counting: O(n), no buffer.
 *
 * Contiguous iterators sort in place. Others (e.g. deque) go through a temporary buffer.
 */
template <class It,
          class Less = std::less<typename std::iterator_traits<It>::value_type>>
void pmsort(It first, It last, Less less = Less{}) {
    using Traits = std::iterator_traits<It>;
    using V = typename Traits::value_type;

    static_assert(std::is_base_of_v<std::random_access_iterator_tag,
                  typename Traits::iterator_category>,
                  "pmsort requires random access iterators");
    static_assert(!std::is_const_v<std::remove_reference_t<typename Traits::reference>>,
                  "pmsort cannot sort a const range");

    const auto distance = last - first;
    if (distance < 2) return;
    [[maybe_unused]] const auto count = static_cast<std::size_t>(distance);

    if constexpr (std::is_same_v<V, bool>) {
        pms_detail::sortBools(first, last, less);
    } else {
        // O(n) check without allocation, works for any random-access iterator
        if (pms_detail::finishIfPresorted(first, last, less)) return;

        if constexpr (!pms_detail::is_supported_v<V>) {
            std::stable_sort(first, last, less);
        } else if constexpr (pms_detail::is_contiguous_v<It>) {
            pms_detail::Sorter<V, Less>(std::addressof(*first), count, std::move(less)).run();
        } else {
            std::unique_ptr<V[]> buffer(new V[count]);
            std::move(first, last, buffer.get());
            pms_detail::Sorter<V, Less>(buffer.get(), count, std::move(less)).run();
            std::move(buffer.get(), buffer.get() + count, first);
        }
    }
}

// pmsort(c) / pmsort(c, comp) for any container with begin() / end()
template <class C, class Less = std::less<>,
          class = std::void_t<decltype(std::begin(std::declval<C&>()))>,
          class = std::enable_if_t<!pms_detail::is_iterator_v<Less>>>
void pmsort(C& c, Less less = Less{}) {
    pmsort(std::begin(c), std::end(c), less);
}

#endif // PMS_HPP
