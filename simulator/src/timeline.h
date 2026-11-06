// The timeline of the simulated sign, after Tracy's zones view: which RTEMS task runs when (from the context
// switches), and the interrupts, PIT ticks, panel frames, serial bytes, remote keys and stops as ticks, on a time
// axis in simulated seconds. Wheel zooms around the mouse, dragging pans, Follow keeps the newest events in view.
// The events come from the Board's trace (Machine::tracing, take_trace), recorded only while the window is open.
#pragma once

#include "machine.h"

#include <string>
#include <vector>

namespace sim {

class Timeline {
public:
    void draw(Machine& m, const char* title, bool* open = nullptr);

private:
    std::vector<Board::TraceEvent> ev_;                  // in time order
    std::vector<std::string> names_;
    double t0_ = 0, t1_ = 40e6;                          // the visible range, in instructions (40 M = 1 s)
    bool follow_ = true, record_ = true;
    void take(Machine& m);
};

}  // namespace sim
