#include <gtest/gtest.h>

#include <memory>

#include <DataTypes/DataTypesNumber.h>
#include <DataTypes/DataTypeNullable.h>

#include <Analyzer/ColumnNode.h>
#include <Analyzer/IQueryTreeNode.h>
#include <Analyzer/ListNode.h>

using namespace DB;

namespace
{

class StubSourceNode final : public IQueryTreeNode
{
public:
    StubSourceNode() : IQueryTreeNode(0 /*children_size*/) {}

    QueryTreeNodeType getNodeType() const override { return QueryTreeNodeType::TABLE; }
    void dumpTreeImpl(WriteBuffer &, FormatState &, size_t) const override {}
    bool isEqualImpl(const IQueryTreeNode &, CompareOptions) const override { return true; }
    void updateTreeHashImpl(HashState &, CompareOptions) const override {}
    QueryTreeNodePtr cloneImpl() const override { return std::make_shared<StubSourceNode>(); }
    ASTPtr toASTImpl(const ConvertToASTOptions &) const override { return nullptr; }
};

QueryTreeNodePtr makeColumn(const String & name, DataTypePtr type, const QueryTreeNodePtr & source)
{
    NameAndTypePair column(name, std::move(type));
    return std::make_shared<ColumnNode>(column, source);
}

QueryTreeNodePtr buildSmallTree()
{
    auto source = std::make_shared<StubSourceNode>();
    auto list = std::make_shared<ListNode>();
    list->getNodes().push_back(makeColumn("a", std::make_shared<DataTypeUInt64>(), source));
    list->getNodes().push_back(makeColumn("b", std::make_shared<DataTypeUInt64>(), source));
    list->getNodes().push_back(makeColumn("c", std::make_shared<DataTypeUInt64>(), source));
    return list;
}

}

TEST(TreeHashMemoization, RepeatedCallsReturnEqualHash)
{
    auto tree = buildSmallTree();

    auto hash1 = tree->getTreeHash();
    auto hash2 = tree->getTreeHash();
    auto hash3 = tree->getTreeHash();

    ASSERT_EQ(hash1, hash2);
    ASSERT_EQ(hash1, hash3);
}

TEST(TreeHashMemoization, ColumnTypeMutationInvalidatesHash)
{
    auto source = std::make_shared<StubSourceNode>();
    auto list = std::make_shared<ListNode>();
    auto col = std::make_shared<ColumnNode>(NameAndTypePair("a", std::make_shared<DataTypeUInt64>()), source);
    list->getNodes().push_back(col);

    auto hash_before = list->getTreeHash();

    /// Mutate the column's type. Without `invalidateTreeHashCache` the column's cached
    /// hash would be stale and the list's cache (filled by the previous call) would
    /// still return the pre-mutation value. The explicit recursive invalidation on the
    /// root models what a deep mutator should do when re-querying an ancestor hash.
    col->setColumnType(std::make_shared<DataTypeUInt32>());
    list->invalidateTreeHashCacheRecursive();

    auto hash_after = list->getTreeHash();

    ASSERT_NE(hash_before, hash_after);
}

TEST(TreeHashMemoization, AliasChangeInvalidatesHash)
{
    auto tree = buildSmallTree();
    auto hash_before = tree->getTreeHash();

    tree->setAlias("renamed_root");
    auto hash_after = tree->getTreeHash();

    ASSERT_NE(hash_before, hash_after);
}

TEST(TreeHashMemoization, NonDefaultOptionsAreNotConflatedWithCache)
{
    auto tree = buildSmallTree();

    /// Populate the default-options cache.
    auto default_hash_first = tree->getTreeHash();

    /// Query with non-default options. The result must equal a freshly-computed
    /// non-default hash (the default cache must NOT be reused for it).
    IQueryTreeNode::CompareOptions options{.compare_aliases = false, .compare_types = true, .ignore_cte = false};
    auto non_default_hash = tree->getTreeHash(options);

    /// Default-options call must still return the same default hash; the
    /// non-default call must not have overwritten the cache with a different value.
    auto default_hash_second = tree->getTreeHash();

    ASSERT_EQ(default_hash_first, default_hash_second);

    /// For a tree with no aliases, the default and non-default hashes happen to
    /// coincide, so additionally add an alias and confirm that the default
    /// hash diverges from the non-default hash.
    tree->setAlias("with_alias");
    auto default_with_alias = tree->getTreeHash();
    auto non_default_with_alias = tree->getTreeHash(options);

    ASSERT_NE(default_with_alias, non_default_with_alias);
    /// The non-default options exclude the alias from the hash, so adding an
    /// alias must NOT change the non-default hash. This confirms the
    /// non-default path is genuinely option-sensitive (and that the cached
    /// default value never leaked into the non-default result).
    ASSERT_EQ(non_default_hash, non_default_with_alias);
}

TEST(TreeHashMemoization, RecursiveInvalidationClearsDescendants)
{
    auto source = std::make_shared<StubSourceNode>();
    auto outer = std::make_shared<ListNode>();
    auto inner = std::make_shared<ListNode>();
    auto col = std::make_shared<ColumnNode>(NameAndTypePair("a", std::make_shared<DataTypeUInt64>()), source);
    inner->getNodes().push_back(col);
    outer->getNodes().push_back(inner);

    /// Populate caches on outer, inner, and the column.
    (void)outer->getTreeHash();
    (void)inner->getTreeHash();
    (void)col->getTreeHash();

    /// Mutate the column's type to a different type — without invalidation, the
    /// column's cache would be stale. Then run a recursive invalidation from the
    /// root; this must clear caches on every descendant. After invalidation, a
    /// fresh `getTreeHash` from the root computes a value reflecting the new type.
    col->setColumnType(std::make_shared<DataTypeUInt32>());
    outer->invalidateTreeHashCacheRecursive();

    auto fresh_hash = outer->getTreeHash();

    /// Construct an equivalent tree with the post-mutation type and confirm the
    /// recursively-invalidated tree's hash equals the freshly-built tree's hash.
    auto source2 = std::make_shared<StubSourceNode>();
    auto outer2 = std::make_shared<ListNode>();
    auto inner2 = std::make_shared<ListNode>();
    auto col2 = std::make_shared<ColumnNode>(NameAndTypePair("a", std::make_shared<DataTypeUInt32>()), source2);
    inner2->getNodes().push_back(col2);
    outer2->getNodes().push_back(inner2);

    ASSERT_EQ(fresh_hash, outer2->getTreeHash());
}

TEST(TreeHashMemoization, ChildHashCompositionMatchesUncached)
{
    /// The walker is allowed to short-circuit cached descendants. The resulting
    /// hash must equal the hash computed without any caching (i.e., from a fresh
    /// tree). This protects against bugs in the cached-vs-uncached composition.
    auto cached_tree = buildSmallTree();
    /// Prime caches bottom-up by hashing each leaf first.
    auto & cached_list = cached_tree->as<ListNode &>();
    for (const auto & leaf : cached_list.getNodes())
        (void)leaf->getTreeHash();
    auto cached_root_hash = cached_tree->getTreeHash();

    /// Build a structurally identical tree but never prime any caches.
    auto fresh_tree = buildSmallTree();
    auto fresh_root_hash = fresh_tree->getTreeHash();

    ASSERT_EQ(cached_root_hash, fresh_root_hash);
}

TEST(TreeHashMemoization, CacheIsActuallyPopulatedAfterDefaultCall)
{
    /// Direct verification of the memoization contract: after a default-options
    /// `getTreeHash` call, the node MUST hold a cached hash value. A regression
    /// that silently disables the cache (e.g., `areDefaultCompareOptions`
    /// returning false unconditionally, or the lazy store being removed) would
    /// pass every other test in this suite but trip this one.
    auto tree = buildSmallTree();
    ASSERT_FALSE(tree->isTreeHashCachedForTest());

    (void)tree->getTreeHash();
    ASSERT_TRUE(tree->isTreeHashCachedForTest());

    /// Recursive invalidation clears it again.
    tree->invalidateTreeHashCacheRecursive();
    ASSERT_FALSE(tree->isTreeHashCachedForTest());
}

TEST(TreeHashMemoization, NonDefaultOptionsDoNotPopulateCache)
{
    /// A `getTreeHash` call with non-default options must not write the cache —
    /// it would poison the default-options key with an option-specific value.
    auto tree = buildSmallTree();
    ASSERT_FALSE(tree->isTreeHashCachedForTest());

    IQueryTreeNode::CompareOptions options{.compare_aliases = false, .compare_types = true, .ignore_cte = false};
    (void)tree->getTreeHash(options);

    ASSERT_FALSE(tree->isTreeHashCachedForTest());
}

TEST(TreeHashMemoization, MutatorClearsCache)
{
    /// `setAlias` is one of the instrumented mutators; calling it must clear
    /// the cached hash on the mutated node.
    auto tree = buildSmallTree();
    (void)tree->getTreeHash();
    ASSERT_TRUE(tree->isTreeHashCachedForTest());

    tree->setAlias("new_alias");
    ASSERT_FALSE(tree->isTreeHashCachedForTest());
}

TEST(TreeHashMemoization, UncachedOverloadNeverPopulatesCache)
{
    /// `getTreeHashUncached` is the thread-safe entry point; it must never
    /// touch the cache (neither read it nor write it). A caller running on a
    /// shared node from multiple threads relies on this invariant.
    auto tree = buildSmallTree();
    ASSERT_FALSE(tree->isTreeHashCachedForTest());

    auto uncached_hash = tree->getTreeHashUncached();
    ASSERT_FALSE(tree->isTreeHashCachedForTest());

    /// And the uncached overload must agree with the cached overload's result.
    auto cached_hash = tree->getTreeHash();
    ASSERT_EQ(uncached_hash, cached_hash);
}
