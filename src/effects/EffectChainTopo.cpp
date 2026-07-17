#include "entity/effects/EffectChainTopo.hpp"

#include "entity/components/Effect.hpp"
#include "entity/effects/EffectKindRegistry.hpp"

#include <algorithm>
#include <unordered_map>

namespace entity::effects {

namespace {

int textureInputCountFor(const entt::registry& registry,
                         entt::entity node,
                         const EffectKindRegistry* kindRegistry) {
    if (!kindRegistry) return 1;
    const auto* fx = registry.try_get<Effect>(node);
    if (!fx) return 1;
    const EffectKind* kind = kindRegistry->find(fx->kindId);
    if (!kind) return 1;
    return std::clamp(kind->textureInputCount(), 0, 4);
}

bool isEnabled(const entt::registry& registry, entt::entity node) {
    const auto* fx = registry.try_get<Effect>(node);
    return fx && fx->enabled;
}

// Enabled nodes of chain.nodes in declaration order, each with linear
// prev-feeds-next inputs. The shared fallback and the connections-empty
// fast path.
EffectExecutionPlan linearPlan(const entt::registry& registry,
                               const EffectChain& chain,
                               const EffectKindRegistry* kindRegistry) {
    EffectExecutionPlan plan;
    for (auto node : chain.nodes) {
        if (!registry.valid(node) || !isEnabled(registry, node)) continue;
        EffectExecutionStep step;
        step.node = node;
        const int nIn = textureInputCountFor(registry, node, kindRegistry);
        step.inputs.assign(static_cast<std::size_t>(nIn), -2);
        if (nIn > 0) {
            step.inputs[0] = plan.steps.empty()
                ? -1
                : static_cast<int>(plan.steps.size()) - 1;
        }
        plan.steps.push_back(std::move(step));
    }
    plan.outputIndex = plan.steps.empty()
        ? -1
        : static_cast<int>(plan.steps.size()) - 1;
    return plan;
}

// Peak number of simultaneously-live step outputs over the plan's
// execution (the final output stays live to the end).
int peakLiveIntermediates(const EffectExecutionPlan& plan) {
    const int n = static_cast<int>(plan.steps.size());
    std::vector<int> lastUse(static_cast<std::size_t>(n), -1);
    for (int i = 0; i < n; ++i) {
        for (int in : plan.steps[static_cast<std::size_t>(i)].inputs) {
            if (in >= 0) lastUse[static_cast<std::size_t>(in)] = i;
        }
    }
    if (plan.outputIndex >= 0) {
        lastUse[static_cast<std::size_t>(plan.outputIndex)] = n;  // held to frame end
    }
    int live = 0, peak = 0;
    for (int i = 0; i < n; ++i) {
        ++live;  // step i's own output
        peak = std::max(peak, live);
        for (int j = 0; j <= i; ++j) {
            if (lastUse[static_cast<std::size_t>(j)] == i) --live;
        }
    }
    return peak;
}

} // namespace

EffectExecutionPlan buildEffectExecutionPlan(const entt::registry& registry,
                                             const EffectChain&    chain,
                                             const EffectKindRegistry* kindRegistry) {
    if (chain.connections.empty()) {
        return linearPlan(registry, chain, kindRegistry);
    }

    // ---- Explicit DAG path ----

    // Enabled nodes in declaration order (stable tie-break for Kahn's).
    std::vector<entt::entity> enabledNodes;
    std::unordered_map<entt::entity, int> nodeOrdinal;  // declaration position
    for (auto node : chain.nodes) {
        if (!registry.valid(node)) continue;
        nodeOrdinal.emplace(node, static_cast<int>(nodeOrdinal.size()));
        if (isEnabled(registry, node)) enabledNodes.push_back(node);
    }
    if (enabledNodes.empty()) return {};

    // Producer of each (node, inputSocket): the src endpoint of the last
    // matching connection (last-wins mirrors the single-input replace rule
    // the connect command enforces going forward).
    struct Endpoint {
        entt::entity src{entt::null};
        bool         connected{false};
    };
    std::unordered_map<entt::entity, std::vector<Endpoint>> producers;
    for (auto node : chain.nodes) {
        if (!registry.valid(node)) continue;
        const int nIn = textureInputCountFor(registry, node, kindRegistry);
        producers[node].assign(static_cast<std::size_t>(nIn), {});
    }
    for (const auto& c : chain.connections) {
        if (c.dstNode == entt::null) continue;  // edge into the final-output sentinel
        auto it = producers.find(c.dstNode);
        if (it == producers.end()) continue;    // stale endpoint
        auto& slots = it->second;
        const std::size_t socket = c.dstSocket;
        if (socket >= slots.size()) continue;   // socket beyond the kind's inputs
        if (c.srcNode != entt::null && !producers.count(c.srcNode)) continue;
        slots[socket] = {c.srcNode, true};
    }

    // Bypass-rewire: resolve a producer through runs of disabled nodes by
    // chasing each disabled node's own first-input producer. A generator
    // (no inputs) or an unconnected input resolves to the layer source /
    // unconnected sentinel respectively.
    auto resolveProducer = [&](Endpoint ep) -> int /* -1 src, -2 unconnected, else ordinal */ {
        for (int guard = 0; guard < 64; ++guard) {
            if (!ep.connected) return -2;
            if (ep.src == entt::null) return -1;
            if (isEnabled(registry, ep.src)) {
                auto it = nodeOrdinal.find(ep.src);
                return (it != nodeOrdinal.end()) ? it->second : -2;
            }
            const auto& srcSlots = producers[ep.src];
            if (srcSlots.empty()) return -2;  // disabled generator: no passthrough
            ep = srcSlots[0];
        }
        return -2;  // pathological disabled loop
    };

    // Resolved input ordinals per enabled node.
    std::unordered_map<entt::entity, std::vector<int>> resolvedInputs;
    for (auto node : enabledNodes) {
        auto& out = resolvedInputs[node];
        for (const auto& ep : producers[node]) out.push_back(resolveProducer(ep));
    }

    // Kahn's over enabled nodes (edges: resolved ordinal producer → node),
    // ready set ordered by declaration position for determinism.
    std::unordered_map<int, entt::entity> ordinalToNode;
    for (auto node : enabledNodes) ordinalToNode[nodeOrdinal[node]] = node;
    std::unordered_map<entt::entity, int> indegree;
    std::unordered_map<entt::entity, std::vector<entt::entity>> consumers;
    for (auto node : enabledNodes) {
        int deg = 0;
        for (int in : resolvedInputs[node]) {
            if (in < 0) continue;
            auto it = ordinalToNode.find(in);
            if (it == ordinalToNode.end()) continue;  // producer disabled → treat unconnected
            if (it->second == node) continue;         // degenerate self-edge
            ++deg;
            consumers[it->second].push_back(node);
        }
        indegree[node] = deg;
    }

    EffectExecutionPlan plan;
    std::vector<entt::entity> ready;
    for (auto node : enabledNodes) {
        if (indegree[node] == 0) ready.push_back(node);
    }
    std::unordered_map<entt::entity, int> stepIndex;
    while (!ready.empty()) {
        // Lowest declaration ordinal first — deterministic across runs.
        auto it = std::min_element(ready.begin(), ready.end(),
            [&](entt::entity a, entt::entity b) {
                return nodeOrdinal[a] < nodeOrdinal[b];
            });
        entt::entity node = *it;
        ready.erase(it);

        stepIndex[node] = static_cast<int>(plan.steps.size());
        EffectExecutionStep step;
        step.node = node;
        plan.steps.push_back(std::move(step));

        for (auto consumer : consumers[node]) {
            if (--indegree[consumer] == 0) ready.push_back(consumer);
        }
    }

    if (plan.steps.size() != enabledNodes.size()) {
        // Cycle — refuse the topology, run the honest linear order.
        auto fallback = linearPlan(registry, chain, kindRegistry);
        fallback.linearFallback = true;
        return fallback;
    }

    // Translate resolved input ordinals → step indices.
    for (auto& step : plan.steps) {
        for (int in : resolvedInputs[step.node]) {
            if (in < 0) {
                step.inputs.push_back(in);
            } else {
                auto it = ordinalToNode.find(in);
                if (it == ordinalToNode.end()) {
                    step.inputs.push_back(-2);
                } else {
                    auto si = stepIndex.find(it->second);
                    step.inputs.push_back(si != stepIndex.end() ? si->second : -2);
                }
            }
        }
    }

    // Output: explicit outputNode (bypass-rewired if disabled), else the
    // last step.
    plan.outputIndex = static_cast<int>(plan.steps.size()) - 1;
    if (chain.outputNode != entt::null && producers.count(chain.outputNode)) {
        const int resolved = resolveProducer({chain.outputNode, true});
        if (resolved >= 0) {
            auto it = ordinalToNode.find(resolved);
            if (it != ordinalToNode.end()) {
                auto si = stepIndex.find(it->second);
                if (si != stepIndex.end()) plan.outputIndex = si->second;
            }
        }
    }

    if (peakLiveIntermediates(plan) > kMaxEffectGraphLiveIntermediates) {
        auto fallback = linearPlan(registry, chain, kindRegistry);
        fallback.linearFallback = true;
        return fallback;
    }

    return plan;
}

bool topologyWouldCycle(const EffectChain& chain,
                        entt::entity src, entt::entity dst) {
    if (src == entt::null || dst == entt::null) return false;
    if (src == dst) return true;

    // DFS from `src` backwards through existing connections: if we can
    // reach `dst` walking src's producer side, dst is upstream of src and
    // the new dst-consumes-src edge would close a loop.
    std::vector<entt::entity> stack{src};
    std::vector<entt::entity> visited;
    while (!stack.empty()) {
        entt::entity cur = stack.back();
        stack.pop_back();
        if (cur == dst) return true;
        if (std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
        visited.push_back(cur);
        for (const auto& c : chain.connections) {
            if (c.dstNode == cur && c.srcNode != entt::null) {
                stack.push_back(c.srcNode);
            }
        }
    }
    return false;
}

std::vector<EffectConnection> materializeLinearTopology(const EffectChain& chain) {
    std::vector<EffectConnection> conns;
    entt::entity prev = entt::null;  // layer-source sentinel
    for (auto node : chain.nodes) {
        conns.push_back({prev, node, 0, 0});
        prev = node;
    }
    conns.push_back({prev, entt::null, 0, 0});  // → final-output sentinel
    return conns;
}

} // namespace entity::effects
