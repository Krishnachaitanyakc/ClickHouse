#pragma once

#include <memory>
#include <optional>
#include <vector>
#include <deque>

#include <Parsers/IAST_fwd.h>
#include <Common/Exception.h>
#include <Common/TypePromotion.h>

#include <city.h>

class SipHash;

namespace DB
{

namespace ErrorCodes
{
extern const int UNSUPPORTED_METHOD;
}

class IDataType;
using DataTypePtr = std::shared_ptr<const IDataType>;

class WriteBuffer;

/// Query tree node type
enum class QueryTreeNodeType : uint8_t
{
    IDENTIFIER,
    MATCHER,
    TRANSFORMER,
    LIST,
    CONSTANT,
    FUNCTION,
    COLUMN,
    LAMBDA,
    SORT,
    INTERPOLATE,
    WINDOW,
    TABLE,
    TABLE_FUNCTION,
    QUERY,
    ARRAY_JOIN,
    CROSS_JOIN,
    JOIN,
    UNION,
};

/// Convert query tree node type to string
const char * toString(QueryTreeNodeType type);

/** Query tree is a semantic representation of query.
  * Query tree node represent node in query tree.
  * IQueryTreeNode is base class for all query tree nodes.
  *
  * Important property of query tree is that each query tree node can contain weak pointers to other
  * query tree nodes. Keeping weak pointer to other query tree nodes can be useful for example for column
  * to keep weak pointer to column source, column source can be table, lambda, subquery and preserving of
  * such information can significantly simplify query planning.
  *
  * Another important property of query tree it must be convertible to AST without losing information.
  */
class IQueryTreeNode;
using QueryTreeNodePtr = std::shared_ptr<IQueryTreeNode>;
using QueryTreeNodes = std::vector<QueryTreeNodePtr>;
using QueryTreeNodesDeque = std::deque<QueryTreeNodePtr>;
using QueryTreeNodeWeakPtr = std::weak_ptr<IQueryTreeNode>;
using QueryTreeWeakNodes = std::vector<QueryTreeNodeWeakPtr>;

struct ConvertToASTOptions
{
    /// Add _CAST if constant literal type is different from column type
    bool add_cast_for_constants = true;

    /// Identifiers are fully qualified (`database.table.column`), otherwise names are just column names (`column`)
    bool fully_qualified_identifiers = true;

    /// Identifiers are qualified but database name is not added (`table.column`) if set to false.
    bool qualify_indentifiers_with_database = true;

    /// Set CTE name in ASTSubquery field.
    bool set_subquery_cte_name = true;
};

class IQueryTreeNode : public TypePromotion<IQueryTreeNode>
{
public:
    virtual ~IQueryTreeNode() = default;

    /// Get query tree node type
    virtual QueryTreeNodeType getNodeType() const = 0;

    /// Get query tree node type name
    const char * getNodeTypeName() const
    {
        return toString(getNodeType());
    }

    /** Get result type of query tree node that can be used as part of expression.
      * If node does not support this method exception is thrown.
      * TODO: Maybe this can be a part of ExpressionQueryTreeNode.
      */
    virtual DataTypePtr getResultType() const
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Method getResultType is not supported for {} query tree node", getNodeTypeName());
    }

    virtual void convertToNullable()
    {
        throw Exception(ErrorCodes::UNSUPPORTED_METHOD, "Method convertToNullable is not supported for {} query tree node", getNodeTypeName());
    }

    struct CompareOptions
    {
        bool compare_aliases = true;
        bool compare_types = true;
        /// Do not compare the cte name or check the is_cte flag for the query node.
        /// Calculate a hash as if is_cte is false and cte_name is empty.
        bool ignore_cte = false;
    };

    /** Is tree equal to other tree with node root.
      *
      * With default compare options aliases of query tree nodes are compared during isEqual call.
      * Original ASTs of query tree nodes are not compared during isEqual call.
      */
    bool isEqual(const IQueryTreeNode & rhs, CompareOptions compare_options = { .compare_aliases = true, .compare_types = true, .ignore_cte = false }) const;

    using Hash = CityHash_v1_0_2::uint128;
    using HashState = SipHash;

    /** Get tree hash identifying current tree
      *
      * Alias of query tree node is part of query tree hash.
      * Original AST is not part of query tree hash.
      *
      * Memoization
      * -----------
      * When `compare_options` equals the default value
      * `{compare_aliases=true, compare_types=true, ignore_cte=false}` the computed
      * subtree hash is cached on this node. Subsequent calls with default options
      * return the cached value in O(1). The walker also consumes cached hashes
      * from descendants instead of re-walking their subtrees, so the FIRST call
      * at any subtree root takes O(uncached descendants) work; for a tree analyzed
      * bottom-up this collapses the total work across N calls from O(N^2) to O(N).
      *
      * Calls with non-default `compare_options` bypass the cache (both read and
      * write) and always perform a full traversal — these are rare enough that
      * the extra work is acceptable and the alternative (a per-option cache) is
      * far more complex.
      *
      * Cache invalidation contract
      * ---------------------------
      * Mutators that change state participating in the hash MUST invalidate the
      * cache on the mutated node via `invalidateTreeHashCache`. Because nodes do
      * not carry a parent pointer, the invalidation cannot walk up to ancestors;
      * callers that mutate deep inside a tree and intend to re-query an ancestor
      * hash must call `invalidateTreeHashCacheRecursive` on that ancestor.
      *
      * Thread safety
      * -------------
      * The cache is NOT thread-safe. A first call from one thread writes
      * `cached_default_hash`; a concurrent call on the same node from another
      * thread is a data race under the C++ memory model and TSan will report it.
      * Callers that may operate on a node shared across threads MUST use
      * `getTreeHashUncached` instead — that overload never reads or writes the
      * cache and is safe for concurrent calls on a `const` node.
      *
      * In Debug and sanitizer builds (Address/Thread/Memory/UB sanitizer), every
      * cached return additionally recomputes the hash from scratch and asserts
      * the values match. This catches missed mutator invalidations in the same
      * CI configurations that already pay the slower-build cost. The check is
      * disabled in release builds without sanitizers because it would otherwise
      * defeat the O(1) cache lookup.
      */
    Hash getTreeHash(CompareOptions compare_options = { .compare_aliases = true, .compare_types = true, .ignore_cte = false }) const;

    /** Identical to `getTreeHash` but never reads or writes the cache.
      *
      * Use this overload from any code path that may run concurrently on a node
      * shared across threads (e.g. `ConcurrentHashJoin`). The cached fast path
      * mutates `cached_default_hash` lazily and is not synchronised; calling
      * `getTreeHash` from multiple threads on the same `const` node is a data
      * race. This overload computes the hash in O(subtree size) every call and
      * is safe for `const`-correct concurrent access.
      */
    Hash getTreeHashUncached(CompareOptions compare_options = { .compare_aliases = true, .compare_types = true, .ignore_cte = false }) const;

    /** Invalidate the cached tree hash on this node only.
      * Must be called by every mutator that changes state participating in the hash.
      */
    void invalidateTreeHashCache() const noexcept
    {
        cached_default_hash.reset();
    }

    /** Invalidate the cached tree hash on this node and on every descendant
      * reachable through `children`. Use when a deep mutation may have invalidated
      * caches that an ancestor's cached hash transitively embeds.
      */
    void invalidateTreeHashCacheRecursive() const noexcept;

    /// Test-only: returns true if the default-options hash is currently cached
    /// on this node. Exists for unit tests that verify the memoization contract
    /// (cache populated on first call, cleared by mutators, never populated for
    /// non-default options). Production code must not depend on this.
    bool isTreeHashCachedForTest() const noexcept
    {
        return cached_default_hash.has_value();
    }

    /// Get a deep copy of the query tree
    QueryTreeNodePtr clone() const;

    /** Get a deep copy of the query tree.
      * If node to clone is key in replacement map, then instead of clone it
      * use value node from replacement map.
      */
    using ReplacementMap = std::unordered_map<const IQueryTreeNode *, QueryTreeNodePtr>;
    QueryTreeNodePtr cloneAndReplace(const ReplacementMap & replacement_map) const;

    /** Get a deep copy of the query tree.
      * If node to clone is node to replace, then instead of clone it use replacement node.
      */
    QueryTreeNodePtr cloneAndReplace(const QueryTreeNodePtr & node_to_replace, QueryTreeNodePtr replacement_node) const;

    /// Returns true if node has alias, false otherwise
    bool hasAlias() const
    {
        return !alias.empty();
    }

    /// Get node alias
    const String & getAlias() const
    {
        return alias;
    }

    const String & getOriginalAlias() const
    {
        return original_alias.empty() ? alias : original_alias;
    }

    /// Set node alias
    void setAlias(String alias_value)
    {
        if (original_alias.empty())
            original_alias = std::move(alias);

        alias = std::move(alias_value);
        invalidateTreeHashCache();
    }

    /// Remove node alias
    void removeAlias()
    {
        alias = {};
        invalidateTreeHashCache();
    }

    /// Returns true if the expression was parenthesized in the original query
    bool isParenthesized() const
    {
        return parenthesized;
    }

    /// Set parenthesized flag
    void setParenthesized(bool value)
    {
        parenthesized = value;
    }

    /// Returns true if query tree node has original AST, false otherwise
    bool hasOriginalAST() const
    {
        return original_ast != nullptr;
    }

    /// Get query tree node original AST
    const ASTPtr & getOriginalAST() const
    {
        return original_ast;
    }

    /** Set query tree node original AST.
      * This AST will not be modified later.
      */
    void setOriginalAST(ASTPtr original_ast_value)
    {
        original_ast = std::move(original_ast_value);
    }

    /** If query tree has original AST format it for error message.
      * Otherwise exception is thrown.
      */
    String formatOriginalASTForErrorMessage() const;

    /// Convert query tree to AST
    ASTPtr toAST(const ConvertToASTOptions & options = {}) const;

    /// Convert query tree to AST and then format it for error message.
    String formatConvertedASTForErrorMessage() const;

    /** Format AST for error message.
      * If original AST exists use `formatOriginalASTForErrorMessage`.
      * Otherwise use `formatConvertedASTForErrorMessage`.
      */
    String formatASTForErrorMessage() const
    {
        if (original_ast)
            return formatOriginalASTForErrorMessage();

        return formatConvertedASTForErrorMessage();
    }

    /// Dump query tree to string
    String dumpTree() const;

    /// Dump query tree to buffer
    void dumpTree(WriteBuffer & buffer) const;

    class FormatState
    {
    public:
        size_t getNodeId(const IQueryTreeNode * node);

    private:
        std::unordered_map<const IQueryTreeNode *, size_t> node_to_id;
    };

    /** Dump query tree to buffer starting with indent.
      *
      * Node must also dump its children.
      */
    virtual void dumpTreeImpl(WriteBuffer & buffer, FormatState & format_state, size_t indent) const = 0;

    /// Get query tree node children
    QueryTreeNodes & getChildren()
    {
        return children;
    }

    /// Get query tree node children
    const QueryTreeNodes & getChildren() const
    {
        return children;
    }

protected:
    /** Construct query tree node.
      * Resize children to children size.
      * Resize weak pointers to weak pointers size.
      */
    explicit IQueryTreeNode(size_t children_size, size_t weak_pointers_size);

    /// Construct query tree node and resize children to children size
    explicit IQueryTreeNode(size_t children_size);

    /** Subclass must compare its internal state with rhs node internal state and do not compare children or weak pointers to other
      * query tree nodes.
      */
    virtual bool isEqualImpl(const IQueryTreeNode & rhs, CompareOptions compare_options) const = 0;

    /** Subclass must update tree hash with its internal state and do not update tree hash for children or weak pointers to other
      * query tree nodes.
      */
    virtual void updateTreeHashImpl(HashState & hash_state, CompareOptions compare_options) const = 0;

    /** Subclass must clone its internal state and do not clone children or weak pointers to other
      * query tree nodes.
      */
    virtual QueryTreeNodePtr cloneImpl() const = 0;

    /// Subclass must convert its internal state and its children to AST
    virtual ASTPtr toASTImpl(const ConvertToASTOptions & options) const = 0;

    QueryTreeNodes children;
    QueryTreeWeakNodes weak_pointers;

private:
    String alias;
    /// An alias from query. Alias can be replaced by query passes,
    /// but we need to keep the original one to support additional_table_filters.
    String original_alias;
    ASTPtr original_ast;
    /// If the expression has extra parentheses around it in the original query
    bool parenthesized = false;

    /** Cached subtree hash computed with the default `CompareOptions`.
      * Populated lazily on the first call to `getTreeHash` with default options
      * and cleared by `invalidateTreeHashCache`. See the comment on `getTreeHash`
      * for the full memoization and invalidation contract.
      */
    mutable std::optional<Hash> cached_default_hash;

    /// Implementation helper used by `getTreeHash`. Walks the subtree rooted at
    /// `root`; if `use_cache` is true, descendants with a populated
    /// `cached_default_hash` short-circuit and contribute their cached value
    /// instead of being re-walked.
    static Hash computeTreeHash(
        const IQueryTreeNode & root,
        CompareOptions compare_options,
        bool use_cache);
};

}
