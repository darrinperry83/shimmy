/* The pinned Linux 6.12 sources predate Apple's partial counted_by support.
 * Keep the same layout as LLVM 19's build, where this optional attribute is
 * unavailable. Include first so compiler_attributes.h's guard keeps this choice.
 */
#include <linux/compiler_attributes.h>
#undef __counted_by
#define __counted_by(member)
