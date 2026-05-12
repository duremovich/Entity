#pragma once

#include <cstdint>
#include <vector>

#include <entt/entt.hpp>

namespace entity {

// One connection in an effect graph. `srcNode == entt::null` means
// "from the layer's source texture" (the synthetic Layer Source input
// in the graph editor). `dstNode == entt::null` means "to the layer's
// final post-effects output" (the synthetic To Screen output).
//
// `srcSocket` / `dstSocket` index into the effect kind's SocketSchema
// list. v1 ships only one input socket (TextureInput, index 0) and one
// output socket (TextureOutput, index 0); parameter sockets arrive in
// a later phase.
struct EffectConnection {
    entt::entity   srcNode{entt::null};
    entt::entity   dstNode{entt::null};
    std::uint8_t   srcSocket{0};
    std::uint8_t   dstSocket{0};
};

// EffectChain lives on a layer entity (Clip or Generative). It owns
// the ordered list of effect entities plus the explicit graph
// topology. When `connections` is empty the chain is interpreted as a
// linear stack in `nodes` order. When `connections` is non-empty the
// topology drives evaluation order (topological sort) and `nodes` is
// the declaration order for the stack-view UI.
//
// `outputNode` is the effect entity whose output feeds PASS 2; it
// defaults to the last node in `nodes` for linear stacks and is set
// explicitly by the graph editor when the user picks a non-trailing
// node as the chain's output.
struct EffectChain {
    std::vector<entt::entity>     nodes;
    std::vector<EffectConnection> connections;
    entt::entity                  outputNode{entt::null};
};

} // namespace entity
