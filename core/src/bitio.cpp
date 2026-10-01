// Translation unit that proves kolma/bitio.hpp compiles on its own and keeps
// the header in the build graph.
#include "kolma/bitio.hpp"

namespace kolma {
namespace {
// Referenced so the header's inline functions are instantiated here.
[[maybe_unused]] void bitio_self_check(Bytes& out) {
  BitWriter w(out);
  w.put(1, 1);
  w.flush();
}
}  // namespace
}  // namespace kolma