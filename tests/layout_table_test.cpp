// Offline check of the speaker-layout table the component delivers with.
//
//   layout_table_test
//
// The table in src/joc_decode.cpp is the one place that says which channels a
// layout has, what each of them is, and in which order they reach foobar2000.  It
// is checked from two sides:
//
//   * against the kernel's own frozen name table -- the names the renderer accepts
//     and the channel counts it reports, so a layout the component offers that the
//     kernel would reject (or a count that drifted apart) fails here;
//   * against the reference renderer's channel order, which is written out below
//     for every layout: the component hands the PCM over in exactly that order.
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

// What the reference renderer emits for each layout, as a space-separated list:
// the names in the order the renderer writes its channels.  The PCM handed to the
// host is that order unchanged (the chunk map is the host default for the count).
struct Expected {
    const char* name;
    const char* order;
};

const Expected kExpected[] = {
    {"2.0", "L R"},
    {"3.1", "L R C LFE"},
    {"4.0", "L R Lrs Rrs"},
    {"5.1", "L R C LFE Ls Rs"},
    {"7.1", "L R C LFE Ls Rs Lrs Rrs"},
    {"5.1.2", "L R C LFE Ls Rs Ltm Rtm"},
    {"5.1.4", "L R C LFE Ls Rs Ltf Rtf Ltr Rtr"},
    {"7.1.2", "L R C LFE Ls Rs Lrs Rrs Ltm Rtm"},
    {"7.1.4", "L R C LFE Ls Rs Lrs Rrs Ltf Rtf Ltr Rtr"},
    {"9.1.4", "L R C LFE Ls Rs Lrs Rrs Lw Rw Ltf Rtf Ltr Rtr"},
    {"9.1.6", "L R C LFE Ls Rs Lrs Rrs Lw Rw Ltf Rtf Ltm Rtm Ltr Rtr"},
};

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

    check(spec->channels == joc_decode::speaker_channels(name),
          "channel count matches speaker_channels()", name);
    check(spec->channels <= joc_decode::kMaxLayoutChannels, "channel count fits the table",
          name);

    // The kernel is the authority on which names exist and how wide they are.
    joc::speaker::LayoutInfo kernel;
    check(joc::speaker::layout_by_name(name.c_str(), &kernel), "kernel accepts the layout",
          name);
    check(kernel.channel_count == spec->channels, "kernel channel count",
          name + " kernel " + std::to_string(kernel.channel_count) + " table " +
              std::to_string(spec->channels));

    // The table lists the reference renderer's order, and the reference order is
    // what the expected list says.
    std::vector<std::string> channels;
    for (unsigned i = 0; i < spec->channels; ++i) {
        check(spec->speakers[i] != nullptr && spec->speakers[i][0] != '\0',
              "channel has a name", name + " channel " + std::to_string(i));
        channels.push_back(spec->speakers[i] != nullptr ? spec->speakers[i] : "");
    }
    check(join(channels) == expected.order, "channel order", name + " got " + join(channels));
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

    // Names the table must not claim: the component and the kernel treat every one
    // of them as an unknown layout.
    for (const char* rejected : {"3.0", "5.0", "6.1", "7.0", "22.2", "4.0 Surround",
                                 "4.0 Quad", ""}) {
        check(joc_decode::speaker_channels(rejected) == 0, "unsupported layout rejected",
              rejected);
    }

    if (g_failures == 0) {
        std::printf("layout_table_test: %zu layouts, 0 problem(s)\n", count);
        return 0;
    }
    std::printf("layout_table_test: %d problem(s)\n", g_failures);
    return 1;
}
