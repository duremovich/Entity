/**
 * Unit tests for effects::buildEffectExecutionPlan and its command-side
 * helpers (topologyWouldCycle, materializeLinearTopology) — the single
 * source of truth for effect-graph evaluation order (DAG executor,
 * ADR-0019 amendment).
 */

#include <gtest/gtest.h>

#include "entity/components/Effect.hpp"
#include "entity/components/EffectChain.hpp"
#include "entity/effects/EffectChainTopo.hpp"
#include "entity/effects/EffectKindRegistry.hpp"

#include <entt/entt.hpp>

using namespace entity;

namespace {

// Registry fixture: builtins registered once (blur etc. = 1 input;
// core.gen.* = 0 inputs).
effects::EffectKindRegistry& kindRegistry() {
    static effects::EffectKindRegistry* reg = [] {
        auto* r = new effects::EffectKindRegistry();
        r->registerBuiltins();
        return r;
    }();
    return *reg;
}

entt::entity makeEffect(entt::registry& reg, const char* stableId,
                        bool enabled = true) {
    entt::entity e = reg.create();
    auto& fx = reg.emplace<Effect>(e);
    fx.kindId  = effects::fnv1a32(stableId);
    fx.enabled = enabled;
    return e;
}

int stepOf(const effects::EffectExecutionPlan& plan, entt::entity node) {
    for (std::size_t i = 0; i < plan.steps.size(); ++i) {
        if (plan.steps[i].node == node) return static_cast<int>(i);
    }
    return -1;
}

} // namespace

TEST(EffectChainTopo, LinearStackDegenerateCase) {
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    EffectChain chain;
    chain.nodes = {a, b};

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    ASSERT_EQ(plan.steps.size(), 2u);
    EXPECT_FALSE(plan.linearFallback);
    EXPECT_EQ(plan.steps[0].node, a);
    ASSERT_EQ(plan.steps[0].inputs.size(), 1u);
    EXPECT_EQ(plan.steps[0].inputs[0], -1);   // layer source
    EXPECT_EQ(plan.steps[1].node, b);
    EXPECT_EQ(plan.steps[1].inputs[0], 0);    // previous step
    EXPECT_EQ(plan.outputIndex, 1);
}

TEST(EffectChainTopo, GeneratorLedLinearStack) {
    entt::registry reg;
    auto gen = makeEffect(reg, "core.gen.linear_gradient");
    auto blur = makeEffect(reg, "core.gaussian_blur");
    EffectChain chain;
    chain.nodes = {gen, blur};

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    ASSERT_EQ(plan.steps.size(), 2u);
    EXPECT_TRUE(plan.steps[0].inputs.empty());  // generator: no inputs
    EXPECT_EQ(plan.steps[1].inputs[0], 0);
}

TEST(EffectChainTopo, ExplicitTopologyReordersDeclarationOrder) {
    // Declaration order [a, b] but connections say source→b→a→output.
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    EffectChain chain;
    chain.nodes = {a, b};
    chain.connections = {
        {entt::null, b, 0, 0},
        {b, a, 0, 0},
        {a, entt::null, 0, 0},
    };
    chain.outputNode = a;

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    ASSERT_EQ(plan.steps.size(), 2u);
    EXPECT_FALSE(plan.linearFallback);
    EXPECT_EQ(plan.steps[0].node, b);
    EXPECT_EQ(plan.steps[0].inputs[0], -1);
    EXPECT_EQ(plan.steps[1].node, a);
    EXPECT_EQ(plan.steps[1].inputs[0], 0);
    EXPECT_EQ(plan.outputIndex, stepOf(plan, a));
}

TEST(EffectChainTopo, DisabledNodeBypassRewire) {
    // source → a → b(disabled) → c: c must read a's output directly.
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert", /*enabled=*/false);
    auto c = makeEffect(reg, "core.vignette");
    EffectChain chain;
    chain.nodes = {a, b, c};
    chain.connections = {
        {entt::null, a, 0, 0},
        {a, b, 0, 0},
        {b, c, 0, 0},
        {c, entt::null, 0, 0},
    };
    chain.outputNode = c;

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    ASSERT_EQ(plan.steps.size(), 2u);  // b dropped
    EXPECT_EQ(stepOf(plan, b), -1);
    const int aStep = stepOf(plan, a);
    const int cStep = stepOf(plan, c);
    ASSERT_GE(aStep, 0);
    ASSERT_GE(cStep, 0);
    EXPECT_EQ(plan.steps[static_cast<std::size_t>(cStep)].inputs[0], aStep);
    EXPECT_EQ(plan.outputIndex, cStep);
}

TEST(EffectChainTopo, CycleFallsBackToLinear) {
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    EffectChain chain;
    chain.nodes = {a, b};
    chain.connections = {
        {a, b, 0, 0},
        {b, a, 0, 0},  // cycle
    };

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    EXPECT_TRUE(plan.linearFallback);
    ASSERT_EQ(plan.steps.size(), 2u);
    EXPECT_EQ(plan.steps[0].node, a);   // declaration order
    EXPECT_EQ(plan.steps[1].node, b);
    EXPECT_EQ(plan.steps[1].inputs[0], 0);
}

TEST(EffectChainTopo, DiamondBranchesResolve) {
    // source fans out to a and b; both feed c... c has 1 input in v1
    // kinds, so model the diamond as: source→a, source→b, a→c,
    // b unconsumed (dead branch). Verifies parallel branches sort and
    // dead branches stay in the plan without cycling.
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    auto c = makeEffect(reg, "core.vignette");
    EffectChain chain;
    chain.nodes = {a, b, c};
    chain.connections = {
        {entt::null, a, 0, 0},
        {entt::null, b, 0, 0},
        {a, c, 0, 0},
        {c, entt::null, 0, 0},
    };
    chain.outputNode = c;

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    EXPECT_FALSE(plan.linearFallback);
    ASSERT_EQ(plan.steps.size(), 3u);
    const int aStep = stepOf(plan, a);
    const int cStep = stepOf(plan, c);
    EXPECT_LT(aStep, cStep);  // producer before consumer
    EXPECT_EQ(plan.steps[static_cast<std::size_t>(cStep)].inputs[0], aStep);
    EXPECT_EQ(plan.outputIndex, cStep);
    // b reads the layer source on its own branch.
    const int bStep = stepOf(plan, b);
    EXPECT_EQ(plan.steps[static_cast<std::size_t>(bStep)].inputs[0], -1);
}

TEST(EffectChainTopo, DisabledOutputNodeResolvesToProducer) {
    // source → a → b(disabled) with outputNode = b: output must resolve
    // to a via bypass.
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert", /*enabled=*/false);
    EffectChain chain;
    chain.nodes = {a, b};
    chain.connections = {
        {entt::null, a, 0, 0},
        {a, b, 0, 0},
        {b, entt::null, 0, 0},
    };
    chain.outputNode = b;

    auto plan = effects::buildEffectExecutionPlan(reg, chain, &kindRegistry());
    ASSERT_EQ(plan.steps.size(), 1u);
    EXPECT_EQ(plan.steps[0].node, a);
    EXPECT_EQ(plan.outputIndex, 0);
}

TEST(EffectChainTopo, WouldCycleDetection) {
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    auto c = makeEffect(reg, "core.vignette");
    EffectChain chain;
    chain.nodes = {a, b, c};
    chain.connections = {
        {a, b, 0, 0},
        {b, c, 0, 0},
    };

    // c → a closes a loop; a → c does not (already reachable downstream).
    EXPECT_TRUE(effects::topologyWouldCycle(chain, c, a));
    EXPECT_FALSE(effects::topologyWouldCycle(chain, a, c));
    // Self-edge always cycles; sentinel endpoints never do.
    EXPECT_TRUE(effects::topologyWouldCycle(chain, a, a));
    EXPECT_FALSE(effects::topologyWouldCycle(chain, entt::null, a));
    EXPECT_FALSE(effects::topologyWouldCycle(chain, a, entt::null));
}

TEST(EffectChainTopo, MaterializeLinearTopology) {
    entt::registry reg;
    auto a = makeEffect(reg, "core.gaussian_blur");
    auto b = makeEffect(reg, "core.invert");
    EffectChain chain;
    chain.nodes = {a, b};

    auto conns = effects::materializeLinearTopology(chain);
    ASSERT_EQ(conns.size(), 3u);
    EXPECT_EQ(conns[0].srcNode, entt::entity{entt::null});
    EXPECT_EQ(conns[0].dstNode, a);
    EXPECT_EQ(conns[1].srcNode, a);
    EXPECT_EQ(conns[1].dstNode, b);
    EXPECT_EQ(conns[2].srcNode, b);
    EXPECT_EQ(conns[2].dstNode, entt::entity{entt::null});
}
