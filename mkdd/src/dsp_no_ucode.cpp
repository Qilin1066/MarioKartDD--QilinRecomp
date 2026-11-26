// A build without the player's DOL has no DSP microcode: the table qilin-dsp
// would generate, empty. (Booting any microcode then stops with its hash.)
#include "dsp_core.hpp"

namespace mkdd::dsp {
const Program* const g_programs = nullptr;
const size_t g_program_count = 0;
}  // namespace mkdd::dsp
