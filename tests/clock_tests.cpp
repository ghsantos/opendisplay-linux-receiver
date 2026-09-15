#include "opendisplay/clock_sync.hpp"

#include <cassert>
#include <cstdio>

using od::ClockSync;

namespace {

void testEmpty() {
    ClockSync sync;
    assert(!sync.offset().has_value());
}

void testMinRttWins() {
    ClockSync sync;
    sync.addSample(100, 1000);
    sync.addSample(10, 900);    // lowest RTT — least queueing distortion
    sync.addSample(50, 950);
    assert(sync.offset().has_value());
    assert(*sync.offset() == 900);
}

void testBadSamplesDiscarded() {
    ClockSync sync;
    sync.addSample(-1, 500);
    sync.addSample(2000, 500);
    assert(!sync.offset().has_value());
    sync.addSample(5, 500);
    assert(*sync.offset() == 500);
}

void testWindowEvictsMin() {
    ClockSync sync;
    sync.addSample(1, 42);      // great sample that will be evicted
    for (int i = 0; i < 20; ++i) {
        sync.addSample(100, 1000 + i);
    }
    // The evicted minimum must not linger: best remaining is the first 100ms
    // sample left in the 15-deep window.
    const auto offset = sync.offset();
    assert(offset.has_value());
    assert(*offset != 42);
}

void testReset() {
    ClockSync sync;
    sync.addSample(10, 100);
    sync.reset();
    assert(!sync.offset().has_value());
}

}  // namespace

int main() {
    testEmpty();
    testMinRttWins();
    testBadSamplesDiscarded();
    testWindowEvictsMin();
    testReset();
    std::puts("clock tests passed");
    return 0;
}
