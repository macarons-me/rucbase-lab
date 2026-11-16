// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"
#include "common/config.h"
#include "common/defs.h"
#include "common/errors.h"
#include "executor_abstract.h"
#include "record/rm_defs.h"
#include "record/rm_file_handle.h"
#include "record/rm_scan.h"
#include "system/sm_manager.h"
#include "system/sm_meta.h"

class SeqScanExecutor : public AbstractExecutor {
private:
    std::string tab_name_;              // 表的名称
    std::vector<Condition> conds_;      // scan的条件
    RmFileHandle* fh_;                  // 表的数据文件句柄
    std::vector<ColMeta> cols_;         // scan后生成的记录的字段
    size_t len_;                        // scan后生成的每条记录的长度
    std::vector<Condition> fed_conds_;  // 同conds_，两个字段相同

    Rid rid_{.page_no = INVALID_PAGE_ID, .slot_no = -1};
    std::unique_ptr<RecScan> scan_;  // table_iterator

    SmManager* sm_manager_;

public:
    SeqScanExecutor(SmManager* sm_manager, std::string tab_name, std::vector<Condition> conds, Context* context) {
        sm_manager_ = sm_manager;
        tab_name_ = std::move(tab_name);
        conds_ = std::move(conds);
        TabMeta& tab = sm_manager_->db_.get_table(tab_name_);
        fh_ = sm_manager_->fhs_.at(tab_name_).get();
        cols_ = tab.cols;
        len_ = cols_.back().offset + cols_.back().len;

        context_ = context;

        fed_conds_ = conds_;
    }

    /**
     * @brief 构建表迭代器scan_,并开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     * @post 若表中没有满足条件的记录，rid_.page_no 为 RM_NO_PAGE，is_end() 返回 true。
     */
    void begin_tuple() override {
        scan_ = std::make_unique<RmScan>(fh_);
        seek_next_match();
    }

    /**
     * @brief 从当前scan_指向的记录开始迭代扫描,直到扫描到第一个满足谓词条件的元组停止,并赋值给rid_
     *
     * @note 已到达表尾时再次调用不产生任何效果，便于上层算子按统一节奏推进孩子。
     */
    void next_tuple() override {
        if (scan_ == nullptr || scan_->is_end()) {
            return;
        }
        scan_->next();
        seek_next_match();
    }

    /**
     * @brief 返回下一个满足扫描条件的记录
     *
     * @return std::unique_ptr<RmRecord> rid_ 指向的当前记录，不推进迭代位置。
     * @throws InternalError 在已经扫描到表尾时调用。
     */
    std::unique_ptr<RmRecord> next() override {
        if (is_end()) {
            throw InternalError("SeqScanExecutor::next called after the end");
        }
        return fh_->get_record(rid_, context_);
    }

    size_t tuple_len() const override { return len_; }

    const std::vector<ColMeta>& cols() const override { return cols_; }

    ColMeta get_col_offset(const TabCol& target) override { return *get_col(cols_, target); }

    Rid& rid() override { return rid_; }

    bool is_end() const override { return scan_ == nullptr || scan_->is_end(); }

private:
    /**
     * @brief 从 scan_ 的当前位置向后寻找第一条满足谓词条件的记录，并把位置同步到 rid_。
     *
     * @post 找到时 rid_ 指向该记录；否则 rid_ 置为 RM_NO_PAGE，且 scan_->is_end() 为 true。
     */
    void seek_next_match() {
        while (!scan_->is_end()) {
            const Rid candidate = scan_->rid();
            if (record_satisfies(*fh_->get_record(candidate, context_))) {
                rid_ = candidate;
                return;
            }
            scan_->next();
        }
        rid_ = {.page_no = RM_NO_PAGE, .slot_no = -1};
    }

    /** @return 记录是否满足全部扫描条件（条件之间是 AND）。 */
    bool record_satisfies(const RmRecord& record) const {
        for (const auto& cond : fed_conds_) {
            if (!match_condition(record, cond)) {
                return false;
            }
        }
        return true;
    }

    /**
     * @brief 判断单条比较条件是否成立。
     *
     * 条件的两侧都已绑定到本表的列（字面量已在 Analyzer 中按左侧列长度编码），
     * 因此这里可以直接按列的偏移和长度定位字段。
     *
     * @throws IncompatibleTypeError 比较两侧类型不一致。
     * @throws InternalError 字面量未被正确编码。
     */
    bool match_condition(const RmRecord& record, const Condition& cond) const {
        const auto lhs_col = get_col(cols_, cond.lhs_col);
        const char* lhs = record.data + lhs_col->offset;

        int cmp = 0;
        if (cond.is_rhs_val) {
            if (cond.rhs_val.type != lhs_col->type) {
                throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(cond.rhs_val.type));
            }
            if (cond.rhs_val.raw == nullptr || cond.rhs_val.raw->size != lhs_col->len) {
                throw InternalError("SeqScan condition value is not encoded to the column length");
            }
            cmp = compare_field(lhs, cond.rhs_val.raw->data, *lhs_col);
        } else {
            const auto rhs_col = get_col(cols_, cond.rhs_col);
            if (rhs_col->type != lhs_col->type) {
                throw IncompatibleTypeError(coltype2str(lhs_col->type), coltype2str(rhs_col->type));
            }
            cmp = compare_field(lhs, record.data + rhs_col->offset, *lhs_col);
        }
        return apply_comparison(cmp, cond.op);
    }

    /**
     * @brief 按列类型比较两个定长字段。
     *
     * 字符串列是定长 CHAR(n)，按列长度整体 memcmp：不足 n 的部分在写入时已补 '\0'，
     * 因此与字面量比较、列与列比较的结果与 SQL 语义一致。
     *
     * @return 负数表示 lhs < rhs，0 表示相等，正数表示 lhs > rhs。
     */
    static int compare_field(const char* lhs, const char* rhs, const ColMeta& col) {
        switch (col.type) {
            case TYPE_INT: {
                int lhs_val = 0;
                int rhs_val = 0;
                std::memcpy(&lhs_val, lhs, sizeof(lhs_val));
                std::memcpy(&rhs_val, rhs, sizeof(rhs_val));
                return (lhs_val > rhs_val) - (lhs_val < rhs_val);
            }
            case TYPE_FLOAT: {
                float lhs_val = 0;
                float rhs_val = 0;
                std::memcpy(&lhs_val, lhs, sizeof(lhs_val));
                std::memcpy(&rhs_val, rhs, sizeof(rhs_val));
                return (lhs_val > rhs_val) - (lhs_val < rhs_val);
            }
            case TYPE_STRING: {
                const int cmp = std::memcmp(lhs, rhs, static_cast<size_t>(col.len));
                return (cmp > 0) - (cmp < 0);
            }
        }
        throw InternalError("Unexpected column type in SeqScan comparison");
    }

    /** @brief 把三值比较结果套用到比较运算符上。 */
    static bool apply_comparison(int cmp, CompOp op) {
        switch (op) {
            case OP_EQ:
                return cmp == 0;
            case OP_NE:
                return cmp != 0;
            case OP_LT:
                return cmp < 0;
            case OP_GT:
                return cmp > 0;
            case OP_LE:
                return cmp <= 0;
            case OP_GE:
                return cmp >= 0;
        }
        throw InternalError("Unexpected comparison operator in SeqScan");
    }
};
