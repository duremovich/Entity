#pragma once

#include "entity/components/EffectChain.hpp"

#include <entt/entt.hpp>
#include <vector>

namespace entity::effects {

class EffectKindRegistry;

// One executable node of a resolved effect chain. `inputs` has one entry
// per texture-input socket of the node's kind:
//   >= 0  index of the producing step in EffectExecutionPlan::steps
//   -1    the layer's source texture (synthetic Layer Source)
//   -2    unconnected (renderer binds nothing; the pass reads black)
struct EffectExecutionStep {
    entt::entity     node{entt::null};
    std::vector<int> inputs;
};

// A fully resolved, enabled-only, topologically ordered execution plan
// for one layer's EffectChain. The single source of truth for evaluation
// order — the snapshot bake emits it onto the wire and the stack-view UI
// orders rows by it, so the two can never disagree.
struct EffectExecutionPlan {
    std::vector<EffectExecutionStep> steps;
    // Index into `steps` whose output feeds PASS 2. -1 = empty plan.
    int outputIndex{-1};
    // True when the explicit topology was rejected (cycle, or more than
    // kMaxLiveIntermediates simultaneously-live intermediates) and the
    // plan fell back to linear stack order. Callers log; the plan is
    // always valid either way.
    bool linearFallback{false};
};

// Bound on simultaneously-live intermediate RTs a graph may need (the
// scratch pool acquires one compose target per live intermediate). A
// linear chain needs 2; a diamond 3. Enforced here — wider graphs fall
// back to linear so the show thread can never blow the compose pool.
inline constexpr int kMaxEffectGraphLiveIntermediates = 4;

// Build the execution plan for `chain`.
//
// connections empty → linear stack: enabled nodes in `chain.nodes` order,
// each consuming the previous step (first consumes the layer source).
// connections non-empty → Kahn's topological sort over enabled nodes;
// disabled nodes are bypass-rewired (consumers re-point to the disabled
// node's first texture input's producer, chasing through runs of disabled
// nodes); `chain.outputNode` picks the final step (default: last).
//
// `kindRegistry` resolves each node's texture-input count (nullptr or an
// unknown kind defaults to 1 input). Never fails: cycles and over-wide
// graphs degrade to the linear plan with `linearFallback` set.
EffectExecutionPlan buildEffectExecutionPlan(const entt::registry& registry,
                                             const EffectChain&    chain,
                                             const EffectKindRegistry* kindRegistry);

// True if adding src→dst would create a cycle in the chain's explicit
// topology (used by ConnectEffectCommand to refuse the edit up front;
// the bake's linearFallback is defense-in-depth only). Sentinel-endpoint
// edges (entt::null) never cycle.
bool topologyWouldCycle(const EffectChain& chain,
                        entt::entity src, entt::entity dst);

// Materialize the implicit linear-stack topology as explicit connections
// (layer-source → n0 → n1 → … → output). The first graph-editor gesture
// on a never-edited chain calls this so editing starts from the exact
// topology the user was already seeing.
std::vector<EffectConnection> materializeLinearTopology(const EffectChain& chain);

} // namespace entity::effects
