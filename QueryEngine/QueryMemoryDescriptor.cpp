/*
 * Copyright 2018 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "QueryMemoryDescriptor.h"

#include "Execute.h"
#include "ExpressionRewrite.h"
#include "GroupByAndAggregate.h"
#include "ScalarExprVisitor.h"
#include "StreamingTopN.h"

bool g_enable_smem_group_by{true};
extern bool g_enable_columnar_output;

namespace {

bool is_int_and_no_bigger_than(const SQLTypeInfo& ti, const size_t byte_width) {
  if (!ti.is_integer()) {
    return false;
  }
  return get_bit_width(ti) <= (byte_width * 8);
}

std::vector<ssize_t> target_expr_group_by_indices(
    const std::list<std::shared_ptr<Analyzer::Expr>>& groupby_exprs,
    const std::vector<Analyzer::Expr*>& target_exprs) {
  std::vector<ssize_t> indices(target_exprs.size(), -1);
  for (size_t target_idx = 0; target_idx < target_exprs.size(); ++target_idx) {
    const auto target_expr = target_exprs[target_idx];
    if (dynamic_cast<const Analyzer::AggExpr*>(target_expr)) {
      continue;
    }
    size_t group_idx = 0;
    for (const auto groupby_expr : groupby_exprs) {
      if (*target_expr == *groupby_expr) {
        indices[target_idx] = group_idx;
        break;
      }
      ++group_idx;
    }
  }
  return indices;
}

class UsedColumnsVisitor : public ScalarExprVisitor<std::unordered_set<int>> {
 protected:
  virtual std::unordered_set<int> visitColumnVar(
      const Analyzer::ColumnVar* column) const override {
    return {column->get_column_id()};
  }

  virtual std::unordered_set<int> aggregateResult(
      const std::unordered_set<int>& aggregate,
      const std::unordered_set<int>& next_result) const override {
    auto result = aggregate;
    result.insert(next_result.begin(), next_result.end());
    return result;
  }
};

std::vector<ssize_t> target_expr_proj_indices(const RelAlgExecutionUnit& ra_exe_unit,
                                              const Catalog_Namespace::Catalog& cat) {
  if (ra_exe_unit.input_descs.size() > 1 ||
      !ra_exe_unit.sort_info.order_entries.empty()) {
    return {};
  }
  std::vector<ssize_t> target_indices(ra_exe_unit.target_exprs.size(), -1);
  UsedColumnsVisitor columns_visitor;
  std::unordered_set<int> used_columns;
  for (const auto& simple_qual : ra_exe_unit.simple_quals) {
    const auto crt_used_columns = columns_visitor.visit(simple_qual.get());
    used_columns.insert(crt_used_columns.begin(), crt_used_columns.end());
  }
  for (const auto& qual : ra_exe_unit.quals) {
    const auto crt_used_columns = columns_visitor.visit(qual.get());
    used_columns.insert(crt_used_columns.begin(), crt_used_columns.end());
  }
  for (const auto& target : ra_exe_unit.target_exprs) {
    const auto col_var = dynamic_cast<const Analyzer::ColumnVar*>(target);
    if (col_var) {
      const auto cd = get_column_descriptor_maybe(
          col_var->get_column_id(), col_var->get_table_id(), cat);
      if (!cd || !cd->isVirtualCol) {
        continue;
      }
    }
    const auto crt_used_columns = columns_visitor.visit(target);
    used_columns.insert(crt_used_columns.begin(), crt_used_columns.end());
  }
  for (size_t target_idx = 0; target_idx < ra_exe_unit.target_exprs.size();
       ++target_idx) {
    const auto target_expr = ra_exe_unit.target_exprs[target_idx];
    CHECK(target_expr);
    const auto& ti = target_expr->get_type_info();
    const bool is_real_str_or_array =
        (ti.is_string() && ti.get_compression() == kENCODING_NONE) || ti.is_array();
    if (is_real_str_or_array) {
      continue;
    }
    if (ti.is_geometry()) {
      // TODO(adb): Ideally we could determine which physical columns are required for a
      // given query and fetch only those. For now, we bail on the memory optimization,
      // since it is possible that adding the physical columns could have unintended
      // consequences further down the execution path.
      return {};
    }
    const auto col_var = dynamic_cast<const Analyzer::ColumnVar*>(target_expr);
    if (!col_var) {
      continue;
    }
    if (!is_real_str_or_array &&
        used_columns.find(col_var->get_column_id()) == used_columns.end()) {
      target_indices[target_idx] = 0;
    }
  }
  return target_indices;
}

int8_t pick_baseline_key_component_width(const ExpressionRange& range) {
  if (range.getType() == ExpressionRangeType::Invalid) {
    return sizeof(int64_t);
  }
  switch (range.getType()) {
    case ExpressionRangeType::Integer:
      return range.getIntMax() < EMPTY_KEY_32 - 1 ? sizeof(int32_t) : sizeof(int64_t);
    case ExpressionRangeType::Float:
    case ExpressionRangeType::Double:
      return sizeof(int64_t);  // No compaction for floating point yet.
    default:
      CHECK(false);
  }
  return sizeof(int64_t);
}

// TODO(miyu): make sure following setting of compact width is correct in all cases.
int8_t pick_baseline_key_width(const RelAlgExecutionUnit& ra_exe_unit,
                               const std::vector<InputTableInfo>& query_infos,
                               const Executor* executor) {
  int8_t compact_width{4};
  for (const auto groupby_expr : ra_exe_unit.groupby_exprs) {
    const auto expr_range = getExpressionRange(groupby_expr.get(), query_infos, executor);
    compact_width =
        std::max(compact_width, pick_baseline_key_component_width(expr_range));
  }
  return compact_width;
}

}  // namespace

QueryMemoryDescriptor::QueryMemoryDescriptor(
    const Executor* executor,
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos,
    const std::vector<int8_t>& group_col_widths,
    const bool allow_multifrag,
    const bool is_group_by,
    const int8_t crt_min_byte_width,
    RenderInfo* render_info,
    const CountDistinctDescriptors count_distinct_descriptors,
    const bool must_use_baseline_sort)
    : executor_(executor)
    , allow_multifrag_(allow_multifrag)
    , query_desc_type_(ra_exe_unit.estimator ? QueryDescriptionType::Estimator
                                             : QueryDescriptionType::NonGroupedAggregate)
    , keyless_hash_(false)
    , interleaved_bins_on_gpu_(false)
    , idx_target_as_key_(-1)
    , init_val_(0)
    , group_col_widths_(group_col_widths)
    , group_col_compact_width_(0)
    , entry_count_(1)
    , min_val_(0)
    , max_val_(0)
    , bucket_(0)
    , has_nulls_(false)
    , sharing_(GroupByMemSharing::Shared)
    , count_distinct_descriptors_(count_distinct_descriptors)
    , sort_on_gpu_(false)
    , output_columnar_(false)
    , render_output_(false)
    , must_use_baseline_sort_(must_use_baseline_sort)
    , force_4byte_float_(false) {
  // Constructor for non-group by queries
  CHECK(!is_group_by);

  CHECK(!must_use_baseline_sort);
  CHECK(!render_info || !render_info->isPotentialInSituRender());

  const auto min_byte_width = QueryMemoryDescriptor::pick_target_compact_width(
      ra_exe_unit, query_infos, crt_min_byte_width);

  for (auto wid : get_col_byte_widths(ra_exe_unit.target_exprs, {})) {
    agg_col_widths_.push_back(
        {wid, static_cast<int8_t>(compact_byte_width(wid, min_byte_width))});
  }
}

QueryMemoryDescriptor::QueryMemoryDescriptor(
    const Executor* executor,
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos,
    const GroupByAndAggregate* group_by_and_agg,
    const std::vector<int8_t>& group_col_widths,
    const ColRangeInfo& col_range_info,
    const bool allow_multifrag,
    const bool is_group_by,
    const int8_t crt_min_byte_width,
    const bool sort_on_gpu_hint,
    const size_t shard_count,
    const size_t max_groups_buffer_entry_count,
    RenderInfo* render_info,
    const CountDistinctDescriptors count_distinct_descriptors,
    const bool must_use_baseline_sort,
    const bool output_columnar_hint)
    : executor_(executor)
    , allow_multifrag_(allow_multifrag)
    , query_desc_type_(col_range_info.hash_type_)
    , keyless_hash_(false)
    , interleaved_bins_on_gpu_(false)
    , idx_target_as_key_(-1)
    , init_val_(0)
    , group_col_widths_(group_col_widths)
    , group_col_compact_width_(0)
    , entry_count_(1)
    , min_val_(col_range_info.min)
    , max_val_(col_range_info.max)
    , bucket_(col_range_info.bucket)
    , has_nulls_(col_range_info.has_nulls)
    , sharing_(GroupByMemSharing::Shared)
    , count_distinct_descriptors_(count_distinct_descriptors)
    , sort_on_gpu_(false)
    , output_columnar_(false)
    , render_output_(false)
    , must_use_baseline_sort_(must_use_baseline_sort)
    , force_4byte_float_(false) {
  // Constructor for group by queries
  CHECK(!group_col_widths.empty());

  CHECK(group_by_and_agg);

  const auto min_byte_width = QueryMemoryDescriptor::pick_target_compact_width(
      ra_exe_unit, query_infos, crt_min_byte_width);

  for (auto wid : get_col_byte_widths(ra_exe_unit.target_exprs, {})) {
    agg_col_widths_.push_back(
        {wid, static_cast<int8_t>(compact_byte_width(wid, min_byte_width))});
  }

  switch (query_desc_type_) {
    case QueryDescriptionType::GroupByPerfectHash: {
      CHECK(!render_info || !render_info->isPotentialInSituRender());
      // multi-column group by query:
      if (group_col_widths_.size() > 1) {
        // max_val_ contains the expected cardinality of the output
        entry_count_ = static_cast<size_t>(max_val_);
        bucket_ = 0;
        return;
      }
      // single-column group by query:
      const auto keyless_info =
          group_by_and_agg->getKeylessInfo(ra_exe_unit.target_exprs, is_group_by);
      idx_target_as_key_ = keyless_info.target_index;
      init_val_ = keyless_info.init_val;

      keyless_hash_ =
          (!sort_on_gpu_hint ||
           !QueryMemoryDescriptor::many_entries(
               col_range_info.max, col_range_info.min, col_range_info.bucket)) &&
          !col_range_info.bucket && !must_use_baseline_sort && keyless_info.keyless;

      entry_count_ =
          std::max(group_by_and_agg->getBucketedCardinality(col_range_info), int64_t(1));
      const size_t interleaved_max_threshold{512};

      size_t gpu_smem_max_threshold{0};
      if (group_by_and_agg->device_type_ == ExecutorDeviceType::GPU) {
        const auto cuda_manager = executor_->getCatalog()->get_dataMgr().cudaMgr_;
        CHECK(cuda_manager);
        /*
         *  We only use shared memory strategy if GPU hardware provides native shared
         *memory atomics support. From CUDA Toolkit documentation:
         *https://docs.nvidia.com/cuda/pascal-tuning-guide/index.html#atomic-ops "Like
         *Maxwell, Pascal [and Volta] provides native shared memory atomic operations for
         *32-bit integer arithmetic, along with native 32 or 64-bit compare-and-swap
         *(CAS)."
         *
         **/
        if (cuda_manager->isArchMaxwellOrLaterForAll()) {
          // TODO(Saman): threshold should be eventually set as an optimized policy per
          // architecture.
          gpu_smem_max_threshold =
              std::min((cuda_manager->isArchVoltaForAll()) ? 4095LU : 2047LU,
                       (cuda_manager->maxSharedMemoryForAll / sizeof(int64_t) - 1));
        }
      }

      if (must_use_baseline_sort) {
        target_groupby_indices_ = target_expr_group_by_indices(ra_exe_unit.groupby_exprs,
                                                               ra_exe_unit.target_exprs);
        agg_col_widths_.clear();
        for (auto wid :
             get_col_byte_widths(ra_exe_unit.target_exprs, target_groupby_indices_)) {
          agg_col_widths_.push_back({wid, static_cast<int8_t>(wid ? 8 : 0)});
        }
      }
      const auto group_expr = ra_exe_unit.groupby_exprs.front().get();
      bool shared_mem_for_group_by =
          g_enable_smem_group_by && keyless_hash_ && keyless_info.shared_mem_support &&
          (entry_count_ <= gpu_smem_max_threshold) &&
          (group_by_and_agg->supportedExprForGpuSharedMemUsage(group_expr)) &&
          QueryMemoryDescriptor::countDescriptorsLogicallyEmpty(
              count_distinct_descriptors) &&
          !output_columnar_;  // TODO(Saman): add columnar support with the new smem
                              // support.

      bool has_varlen_sample_agg = false;
      for (const auto& target_expr : ra_exe_unit.target_exprs) {
        if (target_expr->get_contains_agg()) {
          const auto agg_expr = dynamic_cast<Analyzer::AggExpr*>(target_expr);
          CHECK(agg_expr);
          if (agg_expr->get_aggtype() == kSAMPLE &&
              agg_expr->get_type_info().is_varlen()) {
            has_varlen_sample_agg = true;
            break;
          }
        }
      }

      // TODO(Saman): should remove this after implementing shared memory path completely
      // through codegen We should not use the current shared memory path if more than 8
      // bytes per group is required
      if (shared_mem_for_group_by && (getRowSize() <= sizeof(int64_t))) {
        // TODO(adb / saman): Move this into a different enum so we can remove
        // GroupByMemSharing
        sharing_ = GroupByMemSharing::SharedForKeylessOneColumnKnownRange;
        interleaved_bins_on_gpu_ = false;
      } else {
        interleaved_bins_on_gpu_ = keyless_hash_ && !has_varlen_sample_agg &&
                                   (entry_count_ <= interleaved_max_threshold) &&
                                   QueryMemoryDescriptor::countDescriptorsLogicallyEmpty(
                                       count_distinct_descriptors);
      }
      return;
    }
    case QueryDescriptionType::GroupByBaselineHash: {
      output_columnar_ = output_columnar_hint;
      CHECK(!render_info || !render_info->isPotentialInSituRender());
      entry_count_ = shard_count
                         ? (max_groups_buffer_entry_count + shard_count - 1) / shard_count
                         : max_groups_buffer_entry_count;

      target_groupby_indices_ = target_expr_group_by_indices(ra_exe_unit.groupby_exprs,
                                                             ra_exe_unit.target_exprs);

      group_col_compact_width_ =
          output_columnar_ ? 8
                           : pick_baseline_key_width(ra_exe_unit, query_infos, executor);

      agg_col_widths_.clear();
      for (auto wid :
           get_col_byte_widths(ra_exe_unit.target_exprs, target_groupby_indices_)) {
        // Baseline layout goes through new result set and
        // ResultSetStorage::initializeRowWise assumes everything is padded to 8 bytes,
        // make it so.
        agg_col_widths_.push_back({wid, static_cast<int8_t>(wid ? 8 : 0)});
      }

      min_val_ = 0;
      max_val_ = 0;
      bucket_ = 0;
      has_nulls_ = false;
      return;
    }
    case QueryDescriptionType::Projection: {
      CHECK(!must_use_baseline_sort);

      output_columnar_ = output_columnar_hint;

      // Only projection queries support in-situ rendering
      render_output_ = render_info && render_info->isPotentialInSituRender();

      // TODO(adb): Can we attach this to the QMD as a class member?
      if (use_streaming_top_n(ra_exe_unit, *this) && !output_columnar_hint) {
        entry_count_ = ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit;
      } else {
        entry_count_ = ra_exe_unit.scan_limit
                           ? static_cast<size_t>(ra_exe_unit.scan_limit)
                           : max_groups_buffer_entry_count;
      }

      const auto catalog = executor_->getCatalog();
      CHECK(catalog);
      target_groupby_indices_ = executor_->plan_state_->allow_lazy_fetch_
                                    ? target_expr_proj_indices(ra_exe_unit, *catalog)
                                    : std::vector<ssize_t>{};
      agg_col_widths_.clear();
      for (auto wid :
           get_col_byte_widths(ra_exe_unit.target_exprs, target_groupby_indices_)) {
        // Baseline layout goes through new result set and
        // ResultSetStorage::initializeRowWise assumes everything is padded to 8 bytes,
        // make it so.
        agg_col_widths_.push_back({wid, static_cast<int8_t>(wid ? 8 : 0)});
      }

      bucket_ = 0;
      return;
    }
    default:
      CHECK(false);
  }
}

QueryMemoryDescriptor::QueryMemoryDescriptor()
    : executor_(nullptr)
    , allow_multifrag_(false)
    , query_desc_type_(QueryDescriptionType::Projection)
    , keyless_hash_(false)
    , interleaved_bins_on_gpu_(false)
    , idx_target_as_key_(0)
    , init_val_(0)
    , group_col_compact_width_(0)
    , entry_count_(0)
    , min_val_(0)
    , max_val_(0)
    , bucket_(0)
    , has_nulls_(false)
    , sharing_(GroupByMemSharing::Shared)
    , sort_on_gpu_(false)
    , output_columnar_(false)
    , render_output_(false)
    , must_use_baseline_sort_(false)
    , force_4byte_float_(false) {}

QueryMemoryDescriptor::QueryMemoryDescriptor(const Executor* executor,
                                             const size_t entry_count,
                                             const QueryDescriptionType query_desc_type)
    : executor_(nullptr)
    , allow_multifrag_(false)
    , query_desc_type_(query_desc_type)
    , keyless_hash_(false)
    , interleaved_bins_on_gpu_(false)
    , idx_target_as_key_(0)
    , init_val_(0)
    , group_col_compact_width_(0)
    , entry_count_(entry_count)
    , min_val_(0)
    , max_val_(0)
    , bucket_(0)
    , has_nulls_(false)
    , sharing_(GroupByMemSharing::Shared)
    , sort_on_gpu_(false)
    , output_columnar_(false)
    , render_output_(false)
    , must_use_baseline_sort_(false)
    , force_4byte_float_(false) {}

QueryMemoryDescriptor::QueryMemoryDescriptor(const QueryDescriptionType query_desc_type,
                                             const int64_t min_val,
                                             const int64_t max_val,
                                             const bool has_nulls,
                                             const std::vector<int8_t>& group_col_widths)
    : executor_(nullptr)
    , allow_multifrag_(false)
    , query_desc_type_(query_desc_type)
    , keyless_hash_(false)
    , interleaved_bins_on_gpu_(false)
    , idx_target_as_key_(0)
    , init_val_(0)
    , group_col_widths_(group_col_widths)
    , group_col_compact_width_(0)
    , entry_count_(0)
    , min_val_(min_val)
    , max_val_(max_val)
    , bucket_(0)
    , has_nulls_(false)
    , sharing_(GroupByMemSharing::Shared)
    , sort_on_gpu_(false)
    , output_columnar_(false)
    , render_output_(false)
    , must_use_baseline_sort_(false)
    , force_4byte_float_(false) {}

QueryMemoryDescriptor::QueryMemoryDescriptor(
    const Executor* executor,
    const bool allow_multifrag,
    const QueryDescriptionType query_desc_type,
    const bool keyless_hash,
    const bool interleaved_bins_on_gpu,
    const int32_t idx_target_as_key,
    const int64_t init_val,
    const std::vector<int8_t>& group_col_widths,
    const int8_t group_col_compact_width,
    const std::vector<ColWidths>& agg_col_widths,
    const std::vector<ssize_t>& target_groupby_indices,
    const size_t entry_count,
    const int64_t min_val,
    const int64_t max_val,
    const int64_t bucket,
    const bool has_nulls,
    const GroupByMemSharing sharing,
    const CountDistinctDescriptors count_distinct_descriptors,
    const bool sort_on_gpu,
    const bool output_columnar,
    const bool render_output,
    const std::vector<int8_t>& key_column_pad_bytes,
    const std::vector<int8_t>& target_column_pad_bytes,
    const bool must_use_baseline_sort)
    : agg_col_widths_(agg_col_widths)
    , executor_(executor)
    , allow_multifrag_(allow_multifrag)
    , query_desc_type_(query_desc_type)
    , keyless_hash_(keyless_hash)
    , interleaved_bins_on_gpu_(interleaved_bins_on_gpu)
    , idx_target_as_key_(idx_target_as_key)
    , init_val_(init_val)
    , group_col_widths_(group_col_widths)
    , group_col_compact_width_(group_col_compact_width)
    , target_groupby_indices_(target_groupby_indices)
    , entry_count_(entry_count)
    , min_val_(min_val)
    , max_val_(max_val)
    , bucket_(bucket)
    , has_nulls_(has_nulls)
    , sharing_(sharing)
    , count_distinct_descriptors_(count_distinct_descriptors)
    , sort_on_gpu_(sort_on_gpu)
    , output_columnar_(output_columnar)
    , render_output_(render_output)
    , key_column_pad_bytes_(key_column_pad_bytes)
    , target_column_pad_bytes_(target_column_pad_bytes)
    , must_use_baseline_sort_(must_use_baseline_sort)
    , force_4byte_float_(false) {}

bool QueryMemoryDescriptor::operator==(const QueryMemoryDescriptor& other) const {
  // Note that this method does not check ptr reference members (e.g. executor_) or
  // entry_count_
  if (query_desc_type_ != other.query_desc_type_) {
    return false;
  }
  if (keyless_hash_ != other.keyless_hash_) {
    return false;
  }
  if (interleaved_bins_on_gpu_ != other.interleaved_bins_on_gpu_) {
    return false;
  }
  if (idx_target_as_key_ != other.idx_target_as_key_) {
    return false;
  }
  if (init_val_ != other.init_val_) {
    return false;
  }
  if (force_4byte_float_ != other.force_4byte_float_) {
    return false;
  }
  if (group_col_widths_ != other.group_col_widths_) {
    return false;
  }
  if (group_col_compact_width_ != other.group_col_compact_width_) {
    return false;
  }
  if (agg_col_widths_ != other.agg_col_widths_) {
    return false;
  }
  if (target_groupby_indices_ != other.target_groupby_indices_) {
    return false;
  }
  if (min_val_ != other.min_val_) {
    return false;
  }
  if (max_val_ != other.max_val_) {
    return false;
  }
  if (bucket_ != other.bucket_) {
    return false;
  }
  if (has_nulls_ != other.has_nulls_) {
    return false;
  }
  if (sharing_ != other.sharing_) {
    return false;
  }
  if (count_distinct_descriptors_.size() != count_distinct_descriptors_.size()) {
    return false;
  } else {
    // Count distinct descriptors can legitimately differ in device only.
    for (size_t i = 0; i < count_distinct_descriptors_.size(); ++i) {
      auto ref_count_distinct_desc = other.count_distinct_descriptors_[i];
      auto count_distinct_desc = count_distinct_descriptors_[i];
      count_distinct_desc.device_type = ref_count_distinct_desc.device_type;
      if (ref_count_distinct_desc != count_distinct_desc) {
        return false;
      }
    }
  }
  if (sort_on_gpu_ != other.sort_on_gpu_) {
    return false;
  }
  if (output_columnar_ != other.output_columnar_) {
    return false;
  }
  if (key_column_pad_bytes_ != other.key_column_pad_bytes_) {
    return false;
  }
  if (target_column_pad_bytes_ != other.target_column_pad_bytes_) {
    return false;
  }
  return true;
}

std::unique_ptr<QueryExecutionContext> QueryMemoryDescriptor::getQueryExecutionContext(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<int64_t>& init_agg_vals,
    const Executor* executor,
    const ExecutorDeviceType device_type,
    const int device_id,
    const std::vector<std::vector<const int8_t*>>& col_buffers,
    const std::vector<std::vector<const int8_t*>>& iter_buffers,
    const std::vector<std::vector<uint64_t>>& frag_offsets,
    std::shared_ptr<RowSetMemoryOwner> row_set_mem_owner,
    const bool output_columnar,
    const bool sort_on_gpu,
    RenderInfo* render_info) const {
  return std::unique_ptr<QueryExecutionContext>(
      new QueryExecutionContext(ra_exe_unit,
                                *this,
                                init_agg_vals,
                                executor,
                                device_type,
                                device_id,
                                col_buffers,
                                iter_buffers,
                                frag_offsets,
                                row_set_mem_owner,
                                output_columnar,
                                sort_on_gpu,
                                render_info));
}

int8_t QueryMemoryDescriptor::pick_target_compact_width(
    const RelAlgExecutionUnit& ra_exe_unit,
    const std::vector<InputTableInfo>& query_infos,
    const int8_t crt_min_byte_width) {
  if (g_bigint_count) {
    return sizeof(int64_t);
  }
  int8_t compact_width{0};
  auto col_it = ra_exe_unit.input_col_descs.begin();
  int unnest_array_col_id{std::numeric_limits<int>::min()};
  for (const auto groupby_expr : ra_exe_unit.groupby_exprs) {
    const auto uoper = dynamic_cast<Analyzer::UOper*>(groupby_expr.get());
    if (uoper && uoper->get_optype() == kUNNEST) {
      const auto& arg_ti = uoper->get_operand()->get_type_info();
      CHECK(arg_ti.is_array());
      const auto& elem_ti = arg_ti.get_elem_type();
      if (elem_ti.is_string() && elem_ti.get_compression() == kENCODING_DICT) {
        unnest_array_col_id = (*col_it)->getColId();
      } else {
        compact_width = crt_min_byte_width;
        break;
      }
    }
    ++col_it;
  }
  if (!compact_width &&
      (ra_exe_unit.groupby_exprs.size() != 1 || !ra_exe_unit.groupby_exprs.front())) {
    compact_width = crt_min_byte_width;
  }
  if (!compact_width) {
    col_it = ra_exe_unit.input_col_descs.begin();
    std::advance(col_it, ra_exe_unit.groupby_exprs.size());
    for (const auto target : ra_exe_unit.target_exprs) {
      const auto& ti = target->get_type_info();
      const auto agg = dynamic_cast<const Analyzer::AggExpr*>(target);
      if (agg && agg->get_arg()) {
        compact_width = crt_min_byte_width;
        break;
      }

      if (agg) {
        CHECK_EQ(kCOUNT, agg->get_aggtype());
        CHECK(!agg->get_is_distinct());
        ++col_it;
        continue;
      }

      if (is_int_and_no_bigger_than(ti, 4) ||
          (ti.is_string() && ti.get_compression() == kENCODING_DICT)) {
        ++col_it;
        continue;
      }

      const auto uoper = dynamic_cast<Analyzer::UOper*>(target);
      if (uoper && uoper->get_optype() == kUNNEST &&
          (*col_it)->getColId() == unnest_array_col_id) {
        const auto arg_ti = uoper->get_operand()->get_type_info();
        CHECK(arg_ti.is_array());
        const auto& elem_ti = arg_ti.get_elem_type();
        if (elem_ti.is_string() && elem_ti.get_compression() == kENCODING_DICT) {
          ++col_it;
          continue;
        }
      }

      compact_width = crt_min_byte_width;
      break;
    }
  }
  if (!compact_width) {
    size_t total_tuples{0};
    for (const auto& qi : query_infos) {
      total_tuples += qi.info.getNumTuples();
    }
    return total_tuples <= static_cast<size_t>(std::numeric_limits<uint32_t>::max()) ||
                   unnest_array_col_id != std::numeric_limits<int>::min()
               ? 4
               : crt_min_byte_width;
  } else {
    // TODO(miyu): relax this condition to allow more cases just w/o padding
    for (auto wid : get_col_byte_widths(ra_exe_unit.target_exprs, {})) {
      compact_width = std::max(compact_width, wid);
    }
    return compact_width;
  }
}

size_t QueryMemoryDescriptor::getColsSize() const {
  size_t total_bytes{0};
  for (size_t col_idx = 0; col_idx < agg_col_widths_.size(); ++col_idx) {
    auto chosen_bytes = agg_col_widths_[col_idx].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      total_bytes = align_to_int64(total_bytes);
    }
    total_bytes += chosen_bytes;
  }
  return total_bytes;
}

size_t QueryMemoryDescriptor::getRowSize() const {
  CHECK(!output_columnar_);
  size_t total_bytes{0};
  if (keyless_hash_) {
    CHECK_EQ(size_t(1), group_col_widths_.size());
  } else {
    total_bytes += group_col_widths_.size() * getEffectiveKeyWidth();
    total_bytes = align_to_int64(total_bytes);
  }
  total_bytes += getColsSize();
  return align_to_int64(total_bytes);
}

size_t QueryMemoryDescriptor::getWarpCount() const {
  return (interleaved_bins_on_gpu_ ? executor_->warpSize() : 1);
}

size_t QueryMemoryDescriptor::getCompactByteWidth() const {
  if (agg_col_widths_.empty()) {
    return 8;
  }
  size_t compact_width{0};
  for (const auto col_width : agg_col_widths_) {
    if (col_width.compact != 0) {
      compact_width = col_width.compact;
      break;
    }
  }
  if (!compact_width) {
    return 0;
  }
  CHECK_GT(compact_width, size_t(0));
  for (const auto col_width : agg_col_widths_) {
    if (col_width.compact == 0) {
      continue;
    }
    CHECK_EQ(col_width.compact, compact_width);
  }
  return compact_width;
}

// TODO(miyu): remove if unnecessary
bool QueryMemoryDescriptor::isCompactLayoutIsometric() const {
  if (agg_col_widths_.empty()) {
    return true;
  }
  const auto compact_width = agg_col_widths_.front().compact;
  for (const auto col_width : agg_col_widths_) {
    if (col_width.compact != compact_width) {
      return false;
    }
  }
  return true;
}

size_t QueryMemoryDescriptor::getTotalBytesOfColumnarBuffers(
    const std::vector<ColWidths>& col_widths) const {
  CHECK(output_columnar_);
  size_t total_bytes{0};
  const auto is_isometric = isCompactLayoutIsometric();
  for (size_t col_idx = 0; col_idx < col_widths.size(); ++col_idx) {
    total_bytes += col_widths[col_idx].compact * entry_count_;
    if (!is_isometric) {
      total_bytes = align_to_int64(total_bytes);
    }
  }
  return align_to_int64(total_bytes);
}

size_t QueryMemoryDescriptor::getKeyOffInBytes(const size_t bin,
                                               const size_t key_idx) const {
  CHECK(!keyless_hash_);
  if (output_columnar_) {
    CHECK_EQ(size_t(0), key_idx);
    return bin * sizeof(int64_t);
  }

  CHECK_LT(key_idx, group_col_widths_.size());
  auto offset = bin * getRowSize();
  CHECK_EQ(size_t(0), offset % sizeof(int64_t));
  offset += key_idx * getEffectiveKeyWidth();
  return offset;
}

size_t QueryMemoryDescriptor::getNextKeyOffInBytes(const size_t crt_idx) const {
  CHECK(!keyless_hash_);
  CHECK_LT(crt_idx, group_col_widths_.size());
  if (output_columnar_) {
    CHECK_EQ(size_t(0), crt_idx);
  }
  return getEffectiveKeyWidth();
}

size_t QueryMemoryDescriptor::getColOnlyOffInBytes(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths_.size());
  size_t offset{0};
  for (size_t index = 0; index < col_idx; ++index) {
    const auto chosen_bytes = agg_col_widths_[index].compact;
    if (chosen_bytes == sizeof(int64_t)) {
      offset = align_to_int64(offset);
    }
    offset += chosen_bytes;
  }

  if (sizeof(int64_t) == agg_col_widths_[col_idx].compact) {
    offset = align_to_int64(offset);
  }

  return offset;
}

size_t QueryMemoryDescriptor::getColOffInBytes(const size_t bin,
                                               const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths_.size());
  const auto warp_count = getWarpCount();
  if (output_columnar_) {
    CHECK((bin < entry_count_) || (bin == 0 && entry_count_ == 0));
    CHECK_EQ(size_t(1), warp_count);
    size_t offset{0};
    const auto is_isometric = isCompactLayoutIsometric();
    if (!keyless_hash_) {
      offset = group_col_widths_.size() * sizeof(int64_t) * entry_count_;
    }
    for (size_t index = 0; index < col_idx; ++index) {
      offset += agg_col_widths_[index].compact * entry_count_;
      if (!is_isometric) {
        offset = align_to_int64(offset);
      }
    }
    offset += bin * agg_col_widths_[col_idx].compact;
    return offset;
  }

  auto offset = bin * warp_count * getRowSize();
  if (keyless_hash_) {
    CHECK_EQ(size_t(1), group_col_widths_.size());
  } else {
    offset += group_col_widths_.size() * getEffectiveKeyWidth();
    offset = align_to_int64(offset);
  }
  offset += getColOnlyOffInBytes(col_idx);
  return offset;
}

size_t QueryMemoryDescriptor::getConsistColOffInBytes(const size_t bin,
                                                      const size_t col_idx) const {
  CHECK(output_columnar_ && !agg_col_widths_.empty());
  return (keyless_hash_ ? 0 : sizeof(int64_t) * entry_count_) +
         (col_idx * entry_count_ + bin) * agg_col_widths_[0].compact;
}

size_t QueryMemoryDescriptor::getColOffInBytesInNextBin(const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths_.size());
  auto warp_count = getWarpCount();
  if (output_columnar_) {
    CHECK_EQ(size_t(1), group_col_widths_.size());
    CHECK_EQ(size_t(1), warp_count);
    return agg_col_widths_[col_idx].compact;
  }

  return warp_count * getRowSize();
}

size_t QueryMemoryDescriptor::getNextColOffInBytes(const int8_t* col_ptr,
                                                   const size_t bin,
                                                   const size_t col_idx) const {
  CHECK_LT(col_idx, agg_col_widths_.size());
  CHECK(!output_columnar_ || bin < entry_count_);
  size_t offset{0};
  auto warp_count = getWarpCount();
  const auto chosen_bytes = agg_col_widths_[col_idx].compact;
  if (col_idx + 1 == agg_col_widths_.size()) {
    if (output_columnar_) {
      return (entry_count_ - bin) * chosen_bytes;
    } else {
      return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
    }
  }

  const auto next_chosen_bytes = agg_col_widths_[col_idx + 1].compact;
  if (output_columnar_) {
    CHECK_EQ(size_t(1), group_col_widths_.size());
    CHECK_EQ(size_t(1), warp_count);

    offset = entry_count_ * chosen_bytes;
    if (!isCompactLayoutIsometric()) {
      offset = align_to_int64(offset);
    }
    offset += bin * (next_chosen_bytes - chosen_bytes);
    return offset;
  }

  if (next_chosen_bytes == sizeof(int64_t)) {
    return static_cast<size_t>(align_to_int64(col_ptr + chosen_bytes) - col_ptr);
  } else {
    return chosen_bytes;
  }
}

size_t QueryMemoryDescriptor::getBufferSizeQuad(
    const ExecutorDeviceType device_type) const {
  const auto size_bytes = getBufferSizeBytes(device_type);
  CHECK_EQ(size_t(0), size_bytes % sizeof(int64_t));
  return getBufferSizeBytes(device_type) / sizeof(int64_t);
}

size_t QueryMemoryDescriptor::getBufferSizeBytes(
    const RelAlgExecutionUnit& ra_exe_unit,
    const unsigned thread_count,
    const ExecutorDeviceType device_type) const {
  if (use_streaming_top_n(ra_exe_unit, *this)) {
    const size_t n = ra_exe_unit.sort_info.offset + ra_exe_unit.sort_info.limit;
    return streaming_top_n::get_heap_size(getRowSize(), n, thread_count);
  }
  return getBufferSizeBytes(device_type);
}

size_t QueryMemoryDescriptor::getBufferSizeBytes(
    const ExecutorDeviceType device_type) const {
  if (keyless_hash_) {
    CHECK_GE(group_col_widths_.size(), size_t(1));
    auto total_bytes = align_to_int64(getColsSize());

    return (interleavedBins(device_type) ? executor_->warpSize() : 1) * entry_count_ *
           total_bytes;
  }

  size_t total_bytes{0};
  if (output_columnar_) {
    total_bytes = sizeof(int64_t) * group_col_widths_.size() * entry_count_ +
                  getTotalBytesOfColumnarBuffers(agg_col_widths_);
  } else {
    total_bytes = getRowSize() * entry_count_;
  }

  return total_bytes;
}

bool QueryMemoryDescriptor::usesGetGroupValueFast() const {
  return (query_desc_type_ == QueryDescriptionType::GroupByPerfectHash &&
          getGroupbyColCount() == 1);
}

bool QueryMemoryDescriptor::usesCachedContext() const {
  return allow_multifrag_ &&
         (usesGetGroupValueFast() ||
          query_desc_type_ == QueryDescriptionType::GroupByPerfectHash);
}

bool QueryMemoryDescriptor::threadsShareMemory() const {
  return query_desc_type_ != QueryDescriptionType::NonGroupedAggregate;
}

bool QueryMemoryDescriptor::blocksShareMemory() const {
  if (g_cluster) {
    return true;
  }
  if (!countDescriptorsLogicallyEmpty(count_distinct_descriptors_)) {
    return true;
  }
  if (executor_->isCPUOnly() || render_output_ ||
      query_desc_type_ == QueryDescriptionType::GroupByBaselineHash ||
      query_desc_type_ == QueryDescriptionType::Projection ||
      (query_desc_type_ == QueryDescriptionType::GroupByPerfectHash &&
       getGroupbyColCount() > 1)) {
    return true;
  }
  return usesCachedContext() && !sharedMemBytes(ExecutorDeviceType::GPU) &&
         many_entries(max_val_, min_val_, bucket_);
}

bool QueryMemoryDescriptor::lazyInitGroups(const ExecutorDeviceType device_type) const {
  return device_type == ExecutorDeviceType::GPU && !render_output_ &&
         countDescriptorsLogicallyEmpty(count_distinct_descriptors_);
}

bool QueryMemoryDescriptor::interleavedBins(const ExecutorDeviceType device_type) const {
  return interleaved_bins_on_gpu_ && device_type == ExecutorDeviceType::GPU;
}

size_t QueryMemoryDescriptor::sharedMemBytes(const ExecutorDeviceType device_type) const {
  CHECK(device_type == ExecutorDeviceType::CPU || device_type == ExecutorDeviceType::GPU);
  if (device_type == ExecutorDeviceType::CPU) {
    return 0;
  }
  // if performing keyless aggregate query with a single column group-by:
  if (sharing_ == GroupByMemSharing::SharedForKeylessOneColumnKnownRange) {
    CHECK_EQ(getRowSize(),
             sizeof(int64_t));  // Currently just designed for this scenario
    size_t shared_mem_size =
        (/*bin_count=*/entry_count_ + 1) * sizeof(int64_t);  // one extra for NULL values
    CHECK(shared_mem_size <=
          executor_->getCatalog()->get_dataMgr().cudaMgr_->maxSharedMemoryForAll);
    return shared_mem_size;
  }
  const size_t shared_mem_threshold{0};
  const size_t shared_mem_bytes{getBufferSizeBytes(ExecutorDeviceType::GPU)};
  if (!usesGetGroupValueFast() || shared_mem_bytes > shared_mem_threshold) {
    return 0;
  }
  return shared_mem_bytes;
}

bool QueryMemoryDescriptor::isWarpSyncRequired(
    const ExecutorDeviceType device_type) const {
  if (device_type != ExecutorDeviceType::GPU) {
    return false;
  } else {
    auto cuda_manager = executor_->getCatalog()->get_dataMgr().cudaMgr_;
    CHECK(cuda_manager);
    return cuda_manager->isArchVoltaForAll();
  }
}

bool QueryMemoryDescriptor::canOutputColumnar() const {
  return usesGetGroupValueFast() && threadsShareMemory() && blocksShareMemory() &&
         !interleavedBins(ExecutorDeviceType::GPU) &&
         countDescriptorsLogicallyEmpty(count_distinct_descriptors_);
}
