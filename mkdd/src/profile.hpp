// A sampling profiler for development (profile.cpp).
#pragma once

namespace mkdd {

// With MKDD_PROFILE=<file> set, sample the instruction address every
// millisecond of CPU time and write the counts to <file> every five seconds.
// Without it, nothing.
void start_profiler();

}  // namespace mkdd
