// Offline check of the speaker-layout table the component delivers with.
//
//   layout_table_test
//
// The table in src/joc_decode.cpp is the one place that says which channels a
// layout has, what each of them is, and in which order they reach foobar2000.  It
// is checked from three sides:
//
//   * against the kernel's own frozen name table -- the names the renderer accepts
//     and the channel counts it reports, so a layout the component offers that the
//     kernel would reject (or a count that drifted apart) fails here;
//   * against the reference renderer's channel order, which is written out below
//     for every layout: the component's table lists the renderer's order, and the
//     delivered order is that order permuted into the mask's ascending bit order;
//   * against the host's contract: the mask has exactly one bit per channel, the
//     delivered channel i is the i-th smallest flag, and apply_delivery_order()
//     produces exactly that permutation.
//
// No rendering and no files are involved, so this runs everywhere the kernel
// archive builds.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/joc_decode.h"
#include "speaker/speaker_layout_lookup.h"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, const std::string& detail) {
    if (condition) return;
    ++g_failures;
    std::printf("FAIL %s: %s\n", what, detail.c_str());
}

unsigned popcount(unsigned value) {
    unsigned count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}

// What the reference renderer emits for each layout, in a space-separated form:
// the names in the order the renderer writes its channels (the reference table's
// `internal_to_standard` gathered names), the delivered order that follows from the
// mask's ascending bit order, and the mask the component must declare.  The mask is
// the WAVE mask of the layout name, i.e. the one ffmpeg uses for the same name, with
// 9.1.6's top-middle pair at the only WAVE positions left for it (TFC/TBC).
struct Expected {
    const char* name;
    const char* reference_order;
    const char* delivered_order;
    unsigned mask;
};

const Expected kExpected[] = {
    {"2.0", "L R", "L R", 0x3},
    {"3.1", "L R C LFE", "L R C LFE", 0xF},
    {"5.1", "L R C LFE Ls Rs", "L R C LFE Ls Rs", 0x3F},
    {"7.1", "L R C LFE Lrs Rrs Ls Rs", "L R C LFE Lrs Rrs Ls Rs", 0x63F},
    {"5.1.2", "L R C LFE Ls Rs Ltm Rtm", "L R C LFE Ls Rs Ltm Rtm", 0x560F},
    {"5.1.4", "L R C LFE Ls Rs Ltf Rtf Ltr Rtr", "L R C LFE Ls Rs Ltf Rtf Ltr Rtr", 0x2D60F},
    {"7.1.2", "L R C LFE Lrs Rrs Ls Rs Ltm Rtm", "L R C LFE Lrs Rrs Ls Rs Ltm Rtm", 0x563F},
    {"7.1.4", "L R C LFE Lrs Rrs Ls Rs Ltf Rtf Ltr Rtr",
     "L R C LFE Lrs Rrs Ls Rs Ltf Rtf Ltr Rtr", 0x2D63F},
    {"9.1.4", "L R C LFE Lrs Rrs Ls Rs Ltf Rtf Ltr Rtr Lw Rw",
     "L R C LFE Lrs Rrs Lw Rw Ls Rs Ltf Rtf Ltr Rtr", 0x2D6FF},
    {"9.1.6", "L R C LFE Lrs Rrs Ls Rs Ltf Rtf Ltr Rtr Ltm Rtm Lw Rw",
     "L R C LFE Lrs Rrs Lw Rw Ls Rs Ltf Ltm Rtf Ltr Rtm Rtr", 0x3F6FF},
};

// The channel names of a layout as the component will deliver them: the renderer's
// order permuted into the mask's ascending bit order.
std::vector<std::string> delivered_names(const joc_decode::LayoutSpec& spec,
                                         const joc_decode::LayoutDelivery& delivery) {
    std::vector<std::string> names;
    for (unsigned i = 0; i < delivery.channels; ++i) {
        names.push_back(spec.speakers[delivery.order[i]]);
    }
    return names;
}

std::string join(const std::vector<std::string>& words) {
    std::string text;
    for (const std::string& word : words) {
        if (!text.empty()) text += ' ';
        text += word;
    }
    return text;
}

void check_layout(const Expected& expected) {
    const std::string name = expected.name;
    const joc_decode::LayoutSpec* spec = nullptr;
    std::size_t count = 0;
    const joc_decode::LayoutSpec* table = joc_decode::layout_table(&count);
    for (std::size_t i = 0; i < count; ++i) {
        if (name == table[i].name) spec = &table[i];
    }
    check(spec != nullptr, "layout is in the table", name);
    if (spec == nullptr) return;

    joc_decode::LayoutDelivery delivery;
    check(joc_decode::layout_delivery(name, &delivery), "layout_delivery", name);
    check(delivery.channels == spec->channels, "channel count", name);
    check(popcount(delivery.mask) == delivery.channels, "mask has one bit per channel",
          name + " mask=" + std::to_string(delivery.mask));
    check(delivery.mask == expected.mask,
          "mask", name + " expected " + std::to_string(expected.mask) + " got " +
                        std::to_string(delivery.mask));

    // The kernel is the authority on which names exist and how wide they are.
    joc::speaker::LayoutInfo kernel;
    check(joc::speaker::layout_by_name(name.c_str(), &kernel), "kernel accepts the layout",
          name);
    check(kernel.channel_count == spec->channels, "kernel channel count",
          name + " kernel " + std::to_string(kernel.channel_count) + " table " +
              std::to_string(spec->channels));

    // The table lists the reference renderer's order, and the reference order is
    // what the expected list says.
    std::vector<std::string> reference;
    for (unsigned i = 0; i < spec->channels; ++i) reference.push_back(spec->speakers[i]);
    check(join(reference) == expected.reference_order,
          "reference channel order", name + " got " + join(reference));

    // Delivered order: a permutation of [0, channels), the flag at position i is
    // strictly below the flag at position i + 1, and the names come out as listed.
    std::vector<bool> seen(delivery.channels, false);
    for (unsigned i = 0; i < delivery.channels; ++i) {
        check(delivery.order[i] < delivery.channels, "order index in range", name);
        if (delivery.order[i] < delivery.channels) {
            check(!seen[delivery.order[i]], "order is a permutation", name);
            seen[delivery.order[i]] = true;
        }
        if (i != 0) {
            const unsigned previous = spec->flags[delivery.order[i - 1]];
            const unsigned current = spec->flags[delivery.order[i]];
            check(previous < current, "delivered order follows the mask", name);
        }
    }
    const std::string delivered = join(delivered_names(*spec, delivery));
    check(delivered == expected.delivered_order,
          "delivered order", name + " got " + delivered);
    check(delivery.reordered == (delivered != expected.reference_order), "reordered flag",
          name);

    // apply_delivery_order has to do exactly what order[] says, on a block whose
    // samples are the channel index.
    const std::size_t frames = 2;
    std::vector<float> source(frames * delivery.channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < delivery.channels; ++channel) {
            source[frame * delivery.channels + channel] = static_cast<float>(channel);
        }
    }
    std::vector<float> target(source.size(), -1.0f);
    joc_decode::apply_delivery_order(delivery, source.data(), frames, target.data());
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (unsigned channel = 0; channel < delivery.channels; ++channel) {
            const float value = target[frame * delivery.channels + channel];
            check(value == static_cast<float>(delivery.order[channel]),
                  "apply_delivery_order", name);
        }
    }
}

}  // namespace

int main() {
    // The component's table and the kernel's frozen table have to be the same list
    // of layouts, in the same order: the preferences page shows one, the renderer
    // accepts the other.
    std::size_t count = 0;
    const joc_decode::LayoutSpec* table = joc_decode::layout_table(&count);
    check(count == static_cast<std::size_t>(joc::speaker::kLayoutCount),
          "layout count matches the kernel",
          std::to_string(count) + " vs " + std::to_string(joc::speaker::kLayoutCount));
    const std::size_t expected_count = sizeof(kExpected) / sizeof(kExpected[0]);
    check(count == expected_count, "layout count matches the expected list",
          std::to_string(count) + " vs " + std::to_string(expected_count));
    for (std::size_t i = 0; i < count && i < expected_count; ++i) {
        check(std::strcmp(table[i].name, kExpected[i].name) == 0, "layout order",
              std::string(table[i].name) + " vs " + kExpected[i].name);
        joc::speaker::LayoutInfo kernel;
        if (joc::speaker::layout_at(static_cast<int>(i), &kernel)) {
            check(std::strcmp(table[i].name, kernel.name) == 0, "layout name at index",
                  std::string(table[i].name) + " vs " + kernel.name);
        }
    }

    for (const Expected& expected : kExpected) check_layout(expected);

    // Names the table must not claim: the page used to offer these, and the kernel
    // rejects every one of them.
    for (const char* rejected : {"3.0", "4.0", "5.0", "6.1", "7.0", "22.2", ""}) {
        check(joc_decode::speaker_channels(rejected) == 0, "unsupported layout rejected",
              rejected);
        joc_decode::LayoutDelivery delivery;
        check(!joc_decode::layout_delivery(rejected, &delivery), "unsupported layout has no "
                                                                "delivery",
              rejected);
    }

    if (g_failures == 0) {
        std::printf("layout_table_test: %zu layouts, 0 problem(s)\n", count);
        return 0;
    }
    std::printf("layout_table_test: %d problem(s)\n", g_failures);
    return 1;
}
