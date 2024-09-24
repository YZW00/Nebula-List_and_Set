/* Copyright (c) 2020 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */
#ifndef NEBULA_GRAPH_UTIL_OPTIMIZERUTILS_H_
#define NEBULA_GRAPH_UTIL_OPTIMIZERUTILS_H_

#include "graph/util/SchemaUtil.h"

namespace nebula {

class Expression;

namespace meta {
namespace cpp2 {
class ColumnDef;
class IndexItem;
}  // namespace cpp2
}  // namespace meta

namespace storage {
namespace cpp2 {
class IndexQueryContext;
}  // namespace cpp2
}  // namespace storage

namespace graph {

class IndexScan;

class OptimizerUtils {
 public:
  OptimizerUtils() = delete;

  // Compare `a` and `b`, if `a`>`b` then swap a and b.That means `b`>=`a` after call this function.
  static Status compareAndSwapBound(std::pair<Value, bool> &a, std::pair<Value, bool> &b);
  static void eraseInvalidIndexItems(
      int32_t schemaId, std::vector<std::shared_ptr<nebula::meta::cpp2::IndexItem>> *indexItems);

  // Find optimal index according to filter expression and all valid indexes.
  //
  // For relational condition expression:
  //   1. iterate all indexes
  //   2. select the best column hint for each index
  //     2.1. generate column hint according to the first field of index
  //
  // For logical condition expression(only logical `AND' expression):
  //   1. same steps as above 1, 2
  //   2. for multiple columns combined index:
  //     * iterate each field of index
  //     * iterate each operand expression of filter condition
  //     * collect all column hints generated by operand expression for each index field
  //     * process collected column hints, for example, merge the begin and end values of
  //       range scan
  //   3. sort all index results generated by each index
  //   4. select the largest score index result
  //   5. process the selected index result:
  //     * find the first not prefix column hint and ignore all followed hints except first
  //       range hint
  //     * check whether filter conditions are used, if not, place the unused expression parts
  //       into column hint filter
  //
  // For logical `OR' condition expression, use above steps to generate
  // different `IndexQueryContext' for each operand of filter condition, nebula
  // storage will union all results of multiple index contexts
  static bool findOptimalIndex(
      const Expression *condition,
      const std::vector<std::shared_ptr<nebula::meta::cpp2::IndexItem>> &indexItems,
      bool *isPrefixScan,
      nebula::storage::cpp2::IndexQueryContext *ictx);

  static bool relExprHasIndex(
      const Expression *expr,
      const std::vector<std::shared_ptr<nebula::meta::cpp2::IndexItem>> &indexItems);

  static void copyIndexScanData(const nebula::graph::IndexScan *from,
                                nebula::graph::IndexScan *to,
                                QueryContext *qctx);

  //---------------------------------------------------------------

  using IndexItemPtr = std::shared_ptr<meta::cpp2::IndexItem>;
  using IndexQueryContextList = std::vector<storage::cpp2::IndexQueryContext>;

  struct ScanKind {
    enum class Kind {
      kUnknown = 0,
      kMultipleScan,
      kSingleScan,
    };

   private:
    Kind kind_;

   public:
    ScanKind() {
      kind_ = Kind::kUnknown;
    }
    void setKind(Kind k) {
      kind_ = k;
    }
    Kind getKind() {
      return kind_;
    }
    bool isSingleScan() {
      return kind_ == Kind::kSingleScan;
    }
  };

  // col_   : index column name
  // relOP_ : Relational operator , for example c1 > 1 , the relOP_ == kRelGT
  //                                            1 > c1 , the relOP_ == kRelLT
  // value_ : Constant value. from ConstantExpression.
  struct FilterItem {
    std::string col_;
    Expression::Kind relOP_;
    Value value_;

    FilterItem(const std::string &col, RelationalExpression::Kind relOP, const Value &value)
        : col_(col), relOP_(relOP), value_(value) {}
  };

  static Status createIndexQueryCtx(Expression *filter,
                                    QueryContext *qctx,
                                    const IndexScan *node,
                                    IndexQueryContextList &iqctx);

  static Status createIndexQueryCtx(IndexQueryContextList &iqctx,
                                    QueryContext *qctx,
                                    const IndexScan *node);

  static Status createIndexQueryCtx(IndexQueryContextList &iqctx,
                                    ScanKind kind,
                                    const std::vector<FilterItem> &items,
                                    graph::QueryContext *qctx,
                                    const IndexScan *node);
  static IndexItemPtr findLightestIndex(graph::QueryContext *qctx, const IndexScan *node);

  static Status createMultipleIQC(IndexQueryContextList &iqctx,
                                  const std::vector<FilterItem> &items,
                                  graph::QueryContext *qctx,
                                  const IndexScan *node);
  static Status createSingleIQC(IndexQueryContextList &iqctx,
                                const std::vector<FilterItem> &items,
                                graph::QueryContext *qctx,
                                const IndexScan *node);
  static Status appendIQCtx(const IndexItemPtr &index,
                            const std::vector<FilterItem> &items,
                            IndexQueryContextList &iqctx,
                            const Expression *filter = nullptr);
  static Status appendIQCtx(const IndexItemPtr &index,
                            IndexQueryContextList &iqctx,
                            const Expression *filter = nullptr);
  static Status appendColHint(std::vector<storage::cpp2::IndexColumnHint> &hints,
                              const std::vector<FilterItem> &items,
                              const meta::cpp2::ColumnDef &col);
  static bool verifyType(const Value &val);
  static size_t hintCount(const std::vector<FilterItem> &items);
  static IndexItemPtr findOptimalIndex(graph::QueryContext *qctx,
                                       const IndexScan *node,
                                       const std::vector<FilterItem> &items);
  static std::vector<IndexItemPtr> findIndexForRangeScan(const std::vector<IndexItemPtr> &indexes,
                                                         const std::vector<FilterItem> &items);
  static std::vector<IndexItemPtr> findIndexForEqualScan(const std::vector<IndexItemPtr> &indexes,
                                                         const std::vector<FilterItem> &items);
  static std::vector<IndexItemPtr> findValidIndex(graph::QueryContext *qctx,
                                                  const IndexScan *node,
                                                  const std::vector<FilterItem> &items);
  static std::vector<IndexItemPtr> allIndexesBySchema(graph::QueryContext *qctx,
                                                      const IndexScan *node);
  static Status analyzeExpression(Expression *expr,
                                  std::vector<FilterItem> *items,
                                  ScanKind *kind,
                                  bool isEdge,
                                  QueryContext *qctx);

  template <typename E,
            typename = std::enable_if_t<std::is_same<E, EdgePropertyExpression>::value ||
                                        std::is_same<E, LabelTagPropertyExpression>::value ||
                                        std::is_same<E, TagPropertyExpression>::value>>
  static Status addFilterItem(RelationalExpression *expr,
                              std::vector<FilterItem> *items,
                              QueryContext *qctx);
};

}  // namespace graph
}  // namespace nebula
#endif  // NEBULA_GRAPH_UTIL_OPTIMIZERUTILS_H_
