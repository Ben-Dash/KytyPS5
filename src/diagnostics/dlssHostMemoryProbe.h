#ifndef KYTY_DIAGNOSTICS_DLSSHOSTMEMORYPROBE_H_
#define KYTY_DIAGNOSTICS_DLSSHOSTMEMORYPROBE_H_

namespace Diagnostics {

// Explicit no-ELF diagnostic. Memory and Graphics must already be initialized.
// Maps one guest page through the public direct-memory API and releases it.
[[nodiscard]] bool RunDlssHostMemoryProbe();

} // namespace Diagnostics

#endif
