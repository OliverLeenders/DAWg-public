#pragma once

#include "dawg_sdk/Node.h"

#include <memory>
#include <string_view>

// The nodes the render tool and tests know, by type.

namespace nodes
{

/**
    Creates a node.

    @param type  "dawg.loopReverb" (the node to build) or "example.gain".
    @return      The node, or nullptr for an unknown type.
*/
[[nodiscard]] std::unique_ptr<dawg::Node> make (std::string_view type);

} // namespace nodes
