#include "Nodes.h"

#include "Reverb.h"

// In DAWg: the workspace's Nodes.cpp without the SDK's example node, which isn't part of DAWg.

namespace nodes
{

std::unique_ptr<dawg::Node> make (std::string_view type)
{
    if (type == dawg::reverb::reverbDescriptor.type)
        return std::make_unique<dawg::reverb::ReverbNode>();

    return nullptr;
}

} // namespace nodes
