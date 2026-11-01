// Offline check of the side/rear channel swap the component applies to the PCM it
// hands to foobar2000.
//
//   channel_swap_test
//
// Two things are checked, and neither needs a file or a render:
//
//   * which layouts the swap touches.  It has to be decided by the channel names
//     the layout table gives, because 5.1.2 and 5.1.4 reuse the slots 7.1 keeps
//     for the rear pair: a count-based rule would swap top channels there.
//   * the reorder itself: exactly Ls<->Lrs and Rs<->Rrs, every other slot
//     untouched, and the operation is its own inverse.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/joc_decode.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, const std::string& detail) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL %s: %s\n", what, detail.c_str());
}

// A block whose sample values are the channel index, so the reorder is readable.
std::vector<float> make_block(unsigned channels, std::size_t frames) {
    std::vector<float> block(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            block[frame * channels + channel] = static_cast<float>(channel + frame * 100);
        }
    }
    return block;
}

void expect_swappable(const char* layout, bool expected) {
    const joc_decode::SideRearChannels map = joc_decode::side_rear_channels(layout);
    check(map.complete() == expected, "side_rear_channels", layout);
}

void expect_layout_indices(const char* layout, int ls, int rs, int lrs, int rrs) {
    const joc_decode::SideRearChannels map = joc_decode::side_rear_channels(layout);
    check(map.left_side == ls, "left side index", layout);
    check(map.right_side == rs, "right side index", layout);
    check(map.left_rear == lrs, "left rear index", layout);
    check(map.right_rear == rrs, "right rear index", layout);
}

void expect_swap(const char* layout, unsigned channels, std::size_t frames) {
    const joc_decode::SideRearChannels map = joc_decode::side_rear_channels(layout);
    check(map.complete(), "layout carries both pairs", layout);
    if (!map.complete()) return;

    std::vector<float> block = make_block(channels, frames);
    joc_decode::swap_side_rear(block.data(), frames, channels, map);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < channels; ++channel) {
            int expected = static_cast<int>(channel) + static_cast<int>(frame) * 100;
            if (static_cast<int>(channel) == map.left_side) expected = map.left_rear + static_cast<int>(frame) * 100;
            else if (static_cast<int>(channel) == map.right_side) expected = map.right_rear + static_cast<int>(frame) * 100;
            else if (static_cast<int>(channel) == map.left_rear) expected = map.left_side + static_cast<int>(frame) * 100;
            else if (static_cast<int>(channel) == map.right_rear) expected = map.right_side + static_cast<int>(frame) * 100;
            const float actual = block[frame * channels + channel];
            check(actual == static_cast<float>(expected), "swapped sample", layout);
        }
    }

    // Swapping twice restores the original order.
    joc_decode::swap_side_rear(block.data(), frames, channels, map);
    const std::vector<float> original = make_block(channels, frames);
    check(block == original, "swap is its own inverse", layout);
}

// A layout without both pairs must come through untouched, including the 5.1.x
// ones whose last slots are top channels rather than a rear pair.
void expect_untouched(const char* layout, unsigned channels) {
    const joc_decode::SideRearChannels map = joc_decode::side_rear_channels(layout);
    check(!map.complete(), "layout has no side/rear pair", layout);
    std::vector<float> block = make_block(channels, 3);
    joc_decode::swap_side_rear(block.data(), 3, channels, map);
    check(block == make_block(channels, 3), "untouched", layout);
}

// A caller that passes fewer channels than the map needs is left alone rather than
// reading past the block: the decoder only enables the swap when the counts agree,
// but the guard is cheap.
void expect_guard() {
    const joc_decode::SideRearChannels map = joc_decode::side_rear_channels("7.1");
    std::vector<float> block = make_block(2, 4);
    joc_decode::swap_side_rear(block.data(), 4, 2, map);
    check(block == make_block(2, 4), "narrow block untouched", "7.1 in 2 channels");
    joc_decode::swap_side_rear(nullptr, 4, 8, map);
}

}  // namespace

int main() {
    // Layouts with a real side pair and a real rear pair.
    expect_swappable("7.1", true);
    expect_swappable("7.1.2", true);
    expect_swappable("7.1.4", true);
    expect_swappable("9.1.4", true);
    expect_swappable("9.1.6", true);

    // Stereo, 3.1, 4.0 and the 5.1 family have no rear pair to swap with.
    expect_swappable("2.0", false);
    expect_swappable("3.1", false);
    expect_swappable("4.0", false);
    expect_swappable("5.1", false);
    expect_swappable("5.1.2", false);
    expect_swappable("5.1.4", false);
    // Unknown layouts are no-ops too.
    expect_swappable("", false);
    expect_swappable("7.1.8", false);

    // In the table's order the side pair is 4/5 and the rear pair 6/7.
    expect_layout_indices("7.1", 4, 5, 6, 7);
    expect_layout_indices("7.1.2", 4, 5, 6, 7);
    expect_layout_indices("7.1.4", 4, 5, 6, 7);
    expect_layout_indices("9.1.4", 4, 5, 6, 7);
    expect_layout_indices("9.1.6", 4, 5, 6, 7);

    expect_swap("7.1", 8, 4);
    expect_swap("7.1.2", 10, 2);
    expect_swap("7.1.4", 12, 3);
    expect_swap("9.1.4", 14, 2);
    expect_swap("9.1.6", 16, 2);

    expect_untouched("5.1", 6);
    expect_untouched("5.1.2", 8);
    expect_untouched("5.1.4", 10);
    expect_untouched("4.0", 4);
    expect_guard();

    // The delivery path packs the swapped block to int24 after the reorder, so the
    // int24 output is checked as well: the bytes for the swapped block have to
    // equal the bytes of a hand-permuted copy, i.e. the packing keeps the order.
    {
        const joc_decode::SideRearChannels map = joc_decode::side_rear_channels("7.1");
        std::vector<float> block(8);
        for (unsigned channel = 0; channel < 8; ++channel) {
            block[channel] = static_cast<float>(channel + 1) / 32.0f;
        }
        std::vector<float> expected = block;
        std::swap(expected[4], expected[6]);
        std::swap(expected[5], expected[7]);

        joc_decode::swap_side_rear(block.data(), 1, 8, map);
        std::vector<std::uint8_t> packed(8 * 3);
        std::vector<std::uint8_t> reference(8 * 3);
        joc_decode::pack_int24(block.data(), 8, packed.data());
        joc_decode::pack_int24(expected.data(), 8, reference.data());
        check(packed == reference, "int24 carries the swapped order", "7.1");
    }

    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("ok: side/rear swap covers 7.1 and 9.1 layouts only\n");
    return 0;
}
