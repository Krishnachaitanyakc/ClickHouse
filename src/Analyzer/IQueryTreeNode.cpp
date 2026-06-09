#include <Analyzer/IQueryTreeNode.h>

#include <memory>
#include <unordered_map>

#include <base/defines.h>

#include <Common/SipHash.h>

#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <IO/Operators.h>

#include <Parsers/ASTWithAlias.h>
#include <Parsers/IAST.h>

#include <boost/functional/hash.hpp>

namespace DB
{

namespace ErrorCodes
{
    extern const int UNSUPPORTED_METHOD;
}

const char * toString(QueryTreeNodeType type)
{
    switch (type)
    {
        case QueryTreeNodeType::IDENTIFIER: return "IDENTIFIER";
        case QueryTreeNodeType::MATCHER: return "MATCHER";
        case QueryTreeNodeType::TRANSFORMER: return "TRANSFORMER";
        case QueryTreeNodeType::LIST: return "LIST";
        case QueryTreeNodeType::CONSTANT: return "CONSTANT";
        case QueryTreeNodeType::FUNCTION: return "FUNCTION";
        case QueryTreeNodeType::COLUMN: return "COLUMN";
        case QueryTreeNodeType::LAMBDA: return "LAMBDA";
        case QueryTreeNodeType::SORT: return "SORT";
        case QueryTreeNodeType::INTERPOLATE: return "INTERPOLATE";
        case QueryTreeNodeType::WINDOW: return "WINDOW";
        case QueryTreeNodeType::TABLE: return "TABLE";
        case QueryTreeNodeType::TABLE_FUNCTION: return "TABLE_FUNCTION";
        case QueryTreeNodeType::QUERY: return "QUERY";
        case QueryTreeNodeType::ARRAY_JOIN: return "ARRAY_JOIN";
        case QueryTreeNodeType::CROSS_JOIN: return "CROSS_JOIN";
        case QueryTreeNodeType::JOIN: return "JOIN";
        case QueryTreeNodeType::UNION: return "UNION";
    }
}

IQueryTreeNode::IQueryTreeNode(size_t children_size, size_t weak_pointers_size)
{
    children.resize(children_size);
    weak_pointers.resize(weak_pointers_size);
}

IQueryTreeNode::IQueryTreeNode(size_t children_size)
{
    children.resize(children_size);
}

namespace
{

using NodePair = std::pair<const IQueryTreeNode *, const IQueryTreeNode *>;

struct NodePairHash
{
    size_t operator()(const NodePair & node_pair) const
    {
        auto hash = std::hash<const IQueryTreeNode *>();

        size_t result = 0;
        boost::hash_combine(result, hash(node_pair.first));
        boost::hash_combine(result, hash(node_pair.second));

        return result;
    }
};

}

bool IQueryTreeNode::isEqual(const IQueryTreeNode & rhs, CompareOptions compare_options) const
{
    if (this == &rhs)
        return true;

    std::vector<NodePair> nodes_to_process;
    std::unordered_set<NodePair, NodePairHash> equals_pairs;

    nodes_to_process.emplace_back(this, &rhs);

    while (!nodes_to_process.empty())
    {
        auto nodes_to_compare = nodes_to_process.back();
        nodes_to_process.pop_back();

        const auto * lhs_node_to_compare = nodes_to_compare.first;
        const auto * rhs_node_to_compare = nodes_to_compare.second;

        assert(lhs_node_to_compare);
        assert(rhs_node_to_compare);

        if (equals_pairs.contains(std::make_pair(lhs_node_to_compare, rhs_node_to_compare)))
            continue;

        if (lhs_node_to_compare == rhs_node_to_compare)
        {
            equals_pairs.emplace(lhs_node_to_compare, rhs_node_to_compare);
            continue;
        }

        if (lhs_node_to_compare->getNodeType() != rhs_node_to_compare->getNodeType() ||
            !lhs_node_to_compare->isEqualImpl(*rhs_node_to_compare, compare_options))
            return false;

        if (compare_options.compare_aliases && lhs_node_to_compare->alias != rhs_node_to_compare->alias)
            return false;

        const auto & lhs_children = lhs_node_to_compare->children;
        const auto & rhs_children = rhs_node_to_compare->children;

        size_t lhs_children_size = lhs_children.size();
        if (lhs_children_size != rhs_children.size())
            return false;

        for (size_t i = 0; i < lhs_children_size; ++i)
        {
            const auto & lhs_child = lhs_children[i];
            const auto & rhs_child = rhs_children[i];

            if (!lhs_child && !rhs_child)
                continue;
            if (lhs_child && !rhs_child)
                return false;
            if (!lhs_child && rhs_child)
                return false;

            nodes_to_process.emplace_back(lhs_child.get(), rhs_child.get());
        }

        const auto & lhs_weak_pointers = lhs_node_to_compare->weak_pointers;
        const auto & rhs_weak_pointers = rhs_node_to_compare->weak_pointers;

        size_t lhs_weak_pointers_size = lhs_weak_pointers.size();

        if (lhs_weak_pointers_size != rhs_weak_pointers.size())
            return false;

        for (size_t i = 0; i < lhs_weak_pointers_size; ++i)
        {
            auto lhs_strong_pointer = lhs_weak_pointers[i].lock();
            auto rhs_strong_pointer = rhs_weak_pointers[i].lock();

            if (!lhs_strong_pointer && !rhs_strong_pointer)
                continue;
            if (lhs_strong_pointer && !rhs_strong_pointer)
                return false;
            if (!lhs_strong_pointer && rhs_strong_pointer)
                return false;

            nodes_to_process.emplace_back(lhs_strong_pointer.get(), rhs_strong_pointer.get());
        }

        equals_pairs.emplace(lhs_node_to_compare, rhs_node_to_compare);
    }

    return true;
}

namespace
{

constexpr bool areDefaultCompareOptions(const IQueryTreeNode::CompareOptions & options) noexcept
{
    return options.compare_aliases && options.compare_types && !options.ignore_cte;
}

}

/** Compute tree hash with `root` as the subtree root.
  *
  * Each node owns its own `HashState`. After a child's `HashState` is finalized
  * into a 128-bit `Hash`, that `Hash` (as `low64` + `high64`) is folded into the
  * parent's `HashState`. The cache short-circuit therefore composes identically
  * to a recomputed subtree: in both cases the parent absorbs exactly the same
  * 16 bytes for that child. This is what makes the cached and uncached paths
  * observationally equal — a hard precondition for using the hash as a cache
  * key elsewhere (`QueryAnalyzer::function_cache`, `prepared_sets`, etc.).
  *
  * Traversal is an iterative two-phase post-order DFS (no native recursion, so
  * deep trees do not blow the stack). Each in-flight node has an entry in
  * `subtree_states`, holding its `HashState` and a subtree-local
  * `weak_node_to_identifier` map. The map is per-subtree so that the same
  * subtree produces the same hash regardless of the ancestor context it is
  * walked in (a previous global map made the same subtree hash differently
  * depending on traversal order across siblings).
  *
  * When `use_cache` is true and `compare_options` are default, every subtree we
  * fully compute is also written to that node's `cached_default_hash` — the
  * next walker that reaches it can short-circuit. This is what gives the
  * algorithm its O(N) bottom-up behaviour across N analyzer resolution calls.
  */
IQueryTreeNode::Hash IQueryTreeNode::computeTreeHash(
    const IQueryTreeNode & root,
    CompareOptions compare_options,
    bool use_cache)
{
    /// Cache writes only happen on default options; non-default option calls
    /// must never touch the cache (see `getTreeHash` for the contract).
    const bool may_write_cache = use_cache && areDefaultCompareOptions(compare_options);

    /// Per-in-flight-node state. `std::deque` gives stable references — frames
    /// hold indices into this container while we push children below them.
    ///
    /// `weak_node_to_identifier` is stored behind a `unique_ptr` and is allocated
    /// lazily on the first weak-pointer encounter for this subtree. The vast
    /// majority of nodes never see a weak pointer, so paying for an
    /// `unordered_map` (a heap-allocated bucket array) per node was the dominant
    /// per-node cost in the previous version of this walker.
    struct SubtreeState
    {
        HashState hash_state;
        std::unique_ptr<std::unordered_map<const IQueryTreeNode *, size_t>> weak_node_to_identifier;
    };
    std::deque<SubtreeState> subtree_states;

    enum class Phase : uint8_t { Enter, Leave };

    /// `parent_state_index` is the index of the parent's `SubtreeState` that
    /// this frame's finalized hash will be folded into. For the root, the
    /// parent index is unused and a finalized hash is returned to the caller.
    /// `is_root` prevents the root from being short-circuited by its own
    /// cached value — that decision belongs to `getTreeHash`.
    /// `is_weak_node` selects the weak-pointer code path (first visit assigns
    /// an identifier in the parent's subtree-local map; second visit reuses
    /// it and contributes the identifier to the parent's `HashState`).
    struct Frame
    {
        const IQueryTreeNode * node;
        size_t parent_state_index;
        size_t own_state_index;
        Phase phase;
        bool is_weak_node;
        bool is_root;
    };

    std::vector<Frame> nodes_to_process;
    nodes_to_process.push_back({&root, /*parent_state_index=*/ 0, /*own_state_index=*/ 0, Phase::Enter, /*is_weak_node=*/ false, /*is_root=*/ true});

    /// Holds the finalized root hash once the root's `Leave` phase runs. Using
    /// an `optional` rather than returning early keeps the loop structure flat.
    std::optional<Hash> root_hash;

    while (!nodes_to_process.empty())
    {
        Frame frame = nodes_to_process.back();
        nodes_to_process.pop_back();

        const auto * node_to_process = frame.node;

        if (frame.phase == Phase::Leave)
        {
            /// Finalize this node's HashState into a 128-bit value.
            auto computed_hash = getSipHash128AsPair(subtree_states[frame.own_state_index].hash_state);

            /// If this subtree was computed under default options with caching
            /// enabled, store the result on the node. The root is included —
            /// `getTreeHash` writes it again afterwards, which is harmless.
            if (may_write_cache)
                node_to_process->cached_default_hash = computed_hash;

            if (frame.is_root)
            {
                root_hash = computed_hash;
            }
            else
            {
                auto & parent_state = subtree_states[frame.parent_state_index].hash_state;
                parent_state.update(computed_hash.low64);
                parent_state.update(computed_hash.high64);
            }

            /// The own state is no longer needed. Because all of this node's
            /// descendants have already finished (post-order), and because we
            /// always allocate fresh indices for new frames, we cannot shrink
            /// the deque safely from the back without tracking outstanding
            /// indices. Leaving the entries in place is fine: total memory is
            /// O(in-flight nodes + already-finished nodes) which is bounded by
            /// the subtree size — the same order as the previous single-state
            /// implementation.
            continue;
        }

        if (frame.is_weak_node)
        {
            /// Weak-pointer handling lives on the PARENT'S subtree state: the
            /// identifier map and the bytes contributed are both for the
            /// parent, not for the weak target node itself. We never create
            /// our own SubtreeState in this branch.
            ///
            /// The map is allocated lazily on the first weak-pointer encounter
            /// for this parent subtree — most subtrees never see a weak pointer
            /// at all, and allocating an `unordered_map` per node was the
            /// dominant per-node cost in the previous version of this walker.
            auto & parent_state = subtree_states[frame.parent_state_index];
            if (!parent_state.weak_node_to_identifier)
                parent_state.weak_node_to_identifier = std::make_unique<std::unordered_map<const IQueryTreeNode *, size_t>>();
            auto & parent_weak_map = *parent_state.weak_node_to_identifier;

            auto node_identifier_it = parent_weak_map.find(node_to_process);
            if (node_identifier_it != parent_weak_map.end())
            {
                parent_state.hash_state.update(node_identifier_it->second);
                continue;
            }

            parent_weak_map.emplace(node_to_process, parent_weak_map.size());
            /// Fall through to the regular `Enter` handling below so that the
            /// weak target's full subtree contributes to the parent's hash on
            /// first encounter. To do that we treat the rest of this branch
            /// exactly like a non-weak Enter, by clearing the weak flag and
            /// re-pushing — but the simpler thing is to inline the rest here.
        }

        /// Subtree short-circuit: a non-root descendant whose default-options
        /// hash is already cached contributes its cached 16 bytes to the
        /// parent and is not re-walked. Because finalizing a freshly-computed
        /// subtree also folds 16 bytes into the parent, the two paths are
        /// observationally identical (this is the whole point of using a
        /// per-subtree HashState).
        if (use_cache && !frame.is_root && node_to_process->cached_default_hash.has_value())
        {
            const auto & cached = *node_to_process->cached_default_hash;
            auto & parent_state = subtree_states[frame.parent_state_index].hash_state;
            parent_state.update(cached.low64);
            parent_state.update(cached.high64);
            continue;
        }

        /// Allocate a fresh SubtreeState for this node. The Leave frame
        /// references it via `own_state_index`.
        const size_t own_state_index = subtree_states.size();
        subtree_states.emplace_back();
        auto & own_state = subtree_states[own_state_index].hash_state;

        own_state.update(static_cast<size_t>(node_to_process->getNodeType()));
        if (compare_options.compare_aliases && !node_to_process->alias.empty())
        {
            own_state.update(node_to_process->alias.size());
            own_state.update(node_to_process->alias);
        }

        node_to_process->updateTreeHashImpl(own_state, compare_options);

        own_state.update(node_to_process->children.size());

        /// Push the Leave frame BEFORE children so it runs after they finish
        /// (LIFO stack ⇒ Leave pops last).
        nodes_to_process.push_back({node_to_process, frame.parent_state_index, own_state_index, Phase::Leave, /*is_weak_node=*/ false, frame.is_root});

        /// children.size() has already been folded into own_state above; the
        /// child contributions are appended in source order. We push children
        /// in REVERSE so the LIFO stack visits them in forward order — this
        /// is essential because the parent's HashState is order-sensitive and
        /// must match the uncached order (children[0] folded first, then [1],
        /// then [2], ...).
        for (auto it = node_to_process->children.rbegin(); it != node_to_process->children.rend(); ++it)
        {
            const auto & node_to_process_child = *it;
            if (!node_to_process_child)
                continue;

            nodes_to_process.push_back({node_to_process_child.get(), own_state_index, /*own_state_index=*/ 0, Phase::Enter, /*is_weak_node=*/ false, /*is_root=*/ false});
        }

        own_state.update(node_to_process->weak_pointers.size());

        /// Weak pointers contribute AFTER children (matches uncached order).
        /// Reverse-push for the same LIFO reason.
        for (auto it = node_to_process->weak_pointers.rbegin(); it != node_to_process->weak_pointers.rend(); ++it)
        {
            auto strong_pointer = it->lock();
            if (!strong_pointer)
                continue;

            nodes_to_process.push_back({strong_pointer.get(), own_state_index, /*own_state_index=*/ 0, Phase::Enter, /*is_weak_node=*/ true, /*is_root=*/ false});
        }
    }

    chassert(root_hash.has_value());
    return *root_hash;
}

IQueryTreeNode::Hash IQueryTreeNode::getTreeHash(CompareOptions compare_options) const
{
    /** Cache fast path: only for default `CompareOptions`. The cache is keyed on
      * the implicit "default options" assumption — every variant (`ignore_cte`,
      * `compare_aliases=false`, etc.) bypasses the cache and is computed fresh.
      */
    const bool default_options = areDefaultCompareOptions(compare_options);

    if (default_options && cached_default_hash.has_value())
    {
#if defined(DEBUG_OR_SANITIZER_BUILD)
        /// Debug/sanitizer safety net: recompute uncached and verify the cached
        /// value still matches. If a mutator forgot to call
        /// `invalidateTreeHashCache` this assertion fires in
        /// Debug/ASan/UBSan/TSan/MSan CI and points directly at the bug. The
        /// gate matches `chassert` so the check runs in every CI build that
        /// already enables runtime invariant checks. Release builds without
        /// sanitizers pay only the O(1) cache lookup.
        auto fresh_hash = computeTreeHash(*this, compare_options, /*use_cache=*/ false);
        chassert(fresh_hash == *cached_default_hash);
#endif
        return *cached_default_hash;
    }

    auto computed_hash = computeTreeHash(*this, compare_options, /*use_cache=*/ default_options);

    if (default_options)
        cached_default_hash = computed_hash;

    return computed_hash;
}

IQueryTreeNode::Hash IQueryTreeNode::getTreeHashUncached(CompareOptions compare_options) const
{
    /// Always bypass the cache — no read, no write. Use this from code that may
    /// run concurrently on a shared node (see header for the thread-safety
    /// contract). The cost is O(subtree size) per call.
    return computeTreeHash(*this, compare_options, /*use_cache=*/ false);
}

void IQueryTreeNode::invalidateTreeHashCacheRecursive() const noexcept
{
    /// Iterative DFS over `children`. Weak pointers are NOT followed because they
    /// reference nodes (typically column sources) whose lifetime and ownership
    /// belong to another part of the tree; invalidating through them risks
    /// clearing caches outside this subtree.
    std::vector<const IQueryTreeNode *> stack;
    stack.push_back(this);

    while (!stack.empty())
    {
        const auto * node = stack.back();
        stack.pop_back();

        node->cached_default_hash.reset();

        for (const auto & child : node->children)
        {
            if (child)
                stack.push_back(child.get());
        }
    }
}

QueryTreeNodePtr IQueryTreeNode::clone() const
{
    return cloneAndReplace({});
}

QueryTreeNodePtr IQueryTreeNode::cloneAndReplace(const ReplacementMap & replacement_map) const
{
    /** Clone tree with this node as root.
      *
      * Algorithm
      * For each node we clone state and also create mapping old pointer to new pointer.
      * For each cloned node we update weak pointers array.
      *
      * After that we can update pointer in weak pointers array using old pointer to new pointer mapping.
      */
    std::unordered_map<const IQueryTreeNode *, QueryTreeNodePtr> old_pointer_to_new_pointer;
    std::vector<QueryTreeNodeWeakPtr *> weak_pointers_to_update_after_clone;

    QueryTreeNodePtr result_cloned_node_place;

    std::vector<std::pair<const IQueryTreeNode *, QueryTreeNodePtr *>> nodes_to_clone;
    nodes_to_clone.emplace_back(this, &result_cloned_node_place);

    while (!nodes_to_clone.empty())
    {
        const auto [node_to_clone, place_for_cloned_node] = nodes_to_clone.back();
        nodes_to_clone.pop_back();

        auto already_cloned_node_it = old_pointer_to_new_pointer.find(node_to_clone);
        if (already_cloned_node_it != old_pointer_to_new_pointer.end())
        {
            *place_for_cloned_node = already_cloned_node_it->second;
            continue;
        }

        auto it = replacement_map.find(node_to_clone);
        auto node_clone = it != replacement_map.end() ? it->second : node_to_clone->cloneImpl();
        *place_for_cloned_node = node_clone;

        old_pointer_to_new_pointer.emplace(node_to_clone, node_clone);

        if (it != replacement_map.end())
            continue;

        node_clone->original_ast = node_to_clone->original_ast;
        node_clone->setAlias(node_to_clone->alias);
        node_clone->parenthesized = node_to_clone->parenthesized;
        node_clone->children = node_to_clone->children;
        node_clone->weak_pointers = node_to_clone->weak_pointers;

        for (auto & child : node_clone->children)
        {
            if (!child)
                continue;

            nodes_to_clone.emplace_back(child.get(), &child);
        }

        for (auto & weak_pointer : node_clone->weak_pointers)
        {
            weak_pointers_to_update_after_clone.push_back(&weak_pointer);
        }
    }

    /** Ensure all replacement_map entries are in old_pointer_to_new_pointer.
      * When a node is replaced, its children are not traversed and thus not added
      * to old_pointer_to_new_pointer. If those children are also in the replacement_map
      * (e.g., inner column sources of an ARRAY_JOIN being replaced), their entries
      * must be available for weak pointer updates below.
      */
    for (const auto & [old_ptr, new_ptr] : replacement_map)
        old_pointer_to_new_pointer.emplace(old_ptr, new_ptr);

    /** Update weak pointers to new pointers if they were changed during clone.
      * To do this we check old pointer to new pointer map, if weak pointer
      * strong pointer exists as old pointer in map, reinitialize weak pointer with new pointer.
      */
    for (auto & weak_pointer_ptr : weak_pointers_to_update_after_clone)
    {
        assert(weak_pointer_ptr);
        auto strong_pointer = weak_pointer_ptr->lock();
        auto it = old_pointer_to_new_pointer.find(strong_pointer.get());

        /** If node had weak pointer to some other node and this node is not part of cloned subtree do not update weak pointer.
          * It will continue to point to previous location and it is expected.
          *
          * Example: SELECT id FROM test_table;
          * During analysis `id` is resolved as column node and `test_table` is column source.
          * If we clone `id` column, result column node weak source pointer will point to the same `test_table` column source.
          */
        if (it == old_pointer_to_new_pointer.end())
            continue;

        *weak_pointer_ptr = it->second;
    }
    result_cloned_node_place->original_ast = original_ast;

    return result_cloned_node_place;
}

QueryTreeNodePtr IQueryTreeNode::cloneAndReplace(const QueryTreeNodePtr & node_to_replace, QueryTreeNodePtr replacement_node) const
{
    ReplacementMap replacement_map;
    replacement_map.emplace(node_to_replace.get(), std::move(replacement_node));

    return cloneAndReplace(replacement_map);
}

ASTPtr IQueryTreeNode::toAST(const ConvertToASTOptions & options) const
{
    auto converted_node = toASTImpl(options);

    if (auto * /*ast_with_alias*/ _ = dynamic_cast<ASTWithAlias *>(converted_node.get()))
        converted_node->setAlias(alias);

    converted_node->setParenthesized(parenthesized);

    return converted_node;
}

String IQueryTreeNode::formatOriginalASTForErrorMessage() const
{
    if (!original_ast)
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Original AST was not set");

    return original_ast->formatForErrorMessage();
}

String IQueryTreeNode::formatConvertedASTForErrorMessage() const
{
    return toAST()->formatForErrorMessage();
}

String IQueryTreeNode::dumpTree() const
{
    WriteBufferFromOwnString buffer;
    dumpTree(buffer);

    return buffer.str();
}

size_t IQueryTreeNode::FormatState::getNodeId(const IQueryTreeNode * node)
{
    auto [it, _] = node_to_id.emplace(node, node_to_id.size());
    return it->second;
}

void IQueryTreeNode::dumpTree(WriteBuffer & buffer) const
{
    FormatState state;
    dumpTreeImpl(buffer, state, 0);
}

}
