#pragma once

#include "../../../../engine/src/loopreverb/dawg_sdk/Node.h"

// The clean-room tests name the node API as dawg::Node, dawg::ProcessSpec and so on, as in their workspace. In DAWg
// that copy lives in dawg::reverb (engine/src/loopreverb/README.md), so this test-only header makes its names
// visible in dawg again. It is never included by DAWg's own code, where it would clash with DAWg's node API.

namespace dawg
{
using namespace reverb; // NOLINT(google-build-using-namespace): see above
} // namespace dawg
