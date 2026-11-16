// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "sm_manager.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <sstream>
#include <system_error>
#include <utility>

#include "common/context.h"
#include "common/wire_result.h"
#include "index/index_manager.h"
#include "record/rm_manager.h"
#include "record/rm_scan.h"
#include "storage/buffer_pool_manager.h"
#include "storage/disk_manager.h"

namespace {

namespace fs = std::filesystem;

/**
 * @brief 将数据库名约束为当前目录下的单个路径分量。
 * @throws RMDBError 名称为空、包含父目录或包含路径分隔符。
 */
fs::path database_path(const std::string& db_name) {
    fs::path path(db_name);
    if (path.empty() || path != path.filename() || path == "." || path == "..") {
        throw RMDBError("Invalid database name: " + db_name);
    }
    return path;
}

/**
 * @brief 序列化后再覆盖元数据文件，避免序列化失败时提前清空旧文件。
 * @throws InternalError 序列化、打开或写入失败。
 */
void write_meta_file(const fs::path& path, const DbMeta& db) {
    std::ostringstream serialized;
    serialized << db;
    if (!serialized) {
        throw InternalError("cannot serialize database metadata");
    }

    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) {
        throw InternalError("cannot open database metadata: " + path.string());
    }
    output << serialized.str();
    output.flush();
    if (!output) {
        throw InternalError("cannot write database metadata: " + path.string());
    }
}

/**
 * @brief 将系统命令的字符串结果转换成统一的 WireResultSet。
 * @throws InternalError 上下文为空、行列不匹配或结果超过教学版缓存上限。
 */
void set_string_result(Context* context,
                       std::vector<std::string> column_names,
                       std::vector<std::vector<std::string>> row_values) {
    if (context == nullptr) {
        throw InternalError("utility result requires a request context");
    }

    std::vector<WireResultColumn> columns;
    columns.reserve(column_names.size());
    for (auto& name : column_names) {
        columns.push_back({.name = std::move(name), .type = TYPE_STRING});
    }
    context->result().set_columns(std::move(columns));

    for (auto& values : row_values) {
        std::vector<WireResultCell> row;
        row.reserve(values.size());
        for (auto& value : values) {
            row.push_back({.type = TYPE_STRING, .str_val = std::move(value)});
        }
        context->result().add_row(std::move(row));
    }
}

}  // namespace

bool SmManager::is_dir(const std::string& db_name) const { return fs::is_directory(database_path(db_name)); }

void SmManager::create_db(const std::string& db_name) const {
    const fs::path db_path = database_path(db_name);
    if (fs::exists(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    if (!fs::create_directory(db_path)) {
        throw DatabaseExistsError(db_name);
    }

    try {
        DbMeta new_db;
        new_db.name_ = db_name;
        write_meta_file(db_path / DB_META_NAME, new_db);
        disk_manager_->create_file((db_path / LOG_FILE_NAME).string());
    } catch (...) {
        // 只回滚本次刚创建的目录，不触碰任何既有路径。
        std::error_code ignored;
        fs::remove_all(db_path, ignored);
        throw;
    }
}

void SmManager::drop_db(const std::string& db_name) {
    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path) || !fs::is_regular_file(db_path / DB_META_NAME)) {
        throw DatabaseNotFoundError(db_name);
    }
    if (db_.name_ == db_name) {
        throw InternalError("cannot drop an open database: " + db_name);
    }
    if (fs::remove_all(db_path) == 0) {
        throw DatabaseNotFoundError(db_name);
    }
}

void SmManager::open_db(const std::string& db_name) {
    if (!db_.name_.empty() || !fhs_.empty() || !indexes_.empty()) {
        throw InternalError("Database is already open");
    }

    const fs::path db_path = database_path(db_name);
    if (!fs::is_directory(db_path) || !fs::is_regular_file(db_path / DB_META_NAME)) {
        throw DatabaseNotFoundError(db_name);
    }

    const fs::path previous_directory = fs::current_path();
    fs::current_path(db_path);

    try {
        std::ifstream metadata(DB_META_NAME);
        if (!metadata) {
            throw InternalError("cannot open database metadata: " + db_name);
        }

        DbMeta loaded_db;
        metadata >> loaded_db;
        if (!metadata) {
            throw InternalError("cannot read database metadata: " + db_name);
        }
        if (loaded_db.name_ != db_name) {
            throw InternalError("database metadata name does not match directory: " + db_name);
        }

        for (const auto& table : loaded_db.tabs_ | std::views::values) {
            auto file_handle = rm_manager_->open_file(table.name);
            fhs_.emplace(table.name, std::move(file_handle));

            for (const auto& index : table.indexes) {
                const std::string index_name = index_manager_->make_index_name(table.name, index.cols);
                auto index_handle = index_manager_->open_index(table.name, index.cols);
                const auto [_, inserted] = indexes_.emplace(index_name, std::move(index_handle));
                if (!inserted) {
                    throw InternalError("Duplicate index handle: " + index_name);
                }
            }
        }
        db_ = std::move(loaded_db);
    } catch (...) {
        // 打开失败时恢复到调用前的工作目录，避免服务端把失败状态带到后续请求。
        for (auto& [_, index] : indexes_) {
            try {
                index_manager_->close_index(index.get());
            } catch (...) {
            }
        }
        indexes_.clear();
        for (auto& [_, file_handle] : fhs_) {
            try {
                rm_manager_->close_file(file_handle.get());
            } catch (...) {
            }
        }
        fhs_.clear();
        db_ = DbMeta{};
        std::error_code ignored;
        fs::current_path(previous_directory, ignored);
        throw;
    }
}

void SmManager::flush_meta() const { write_meta_file(DB_META_NAME, db_); }

void SmManager::show_database(Context* context) const { set_string_result(context, {"Database"}, {{db_.name_}}); }

void SmManager::close_db() {
    if (db_.name_.empty() && fhs_.empty() && indexes_.empty()) {
        return;
    }

    std::exception_ptr first_error;
    try {
        flush_meta();
    } catch (...) {
        first_error = std::current_exception();
    }

    for (auto& [_, index] : indexes_) {
        try {
            index_manager_->close_index(index.get());
        } catch (...) {
            if (first_error == nullptr) {
                first_error = std::current_exception();
            }
        }
    }
    indexes_.clear();

    for (auto& [_, file_handle] : fhs_) {
        try {
            rm_manager_->close_file(file_handle.get());
        } catch (...) {
            if (first_error == nullptr) {
                first_error = std::current_exception();
            }
        }
    }
    fhs_.clear();
    db_ = DbMeta{};

    std::error_code path_error;
    fs::current_path("..", path_error);
    if (path_error && first_error == nullptr) {
        first_error = std::make_exception_ptr(
            InternalError("cannot leave database directory: " + path_error.message()));
    }
    if (first_error != nullptr) {
        std::rethrow_exception(first_error);
    }
}

void SmManager::show_tables(Context* context) {
    std::vector<std::vector<std::string>> rows;
    rows.reserve(db_.tabs_.size());
    // C++20：`map | std::views::values` 只遍历 value（表元数据），不关心 key。
    // 等价于 for (const auto& [name, tab] : db_.tabs_) { ... 使用 tab ... }
    for (const auto& tab : db_.tabs_ | std::views::values) {
        rows.push_back({tab.name});
    }
    set_string_result(context, {"Tables"}, std::move(rows));
}

void SmManager::desc_table(const std::string& tab_name, Context* context) {
    const TabMeta& tab = db_.get_table(tab_name);

    std::vector<std::vector<std::string>> rows;
    rows.reserve(tab.cols.size());
    for (const auto& col : tab.cols) {
        rows.push_back({col.name, coltype2str(col.type), col.index ? "YES" : "NO"});
    }
    set_string_result(context, {"Field", "Type", "Index"}, std::move(rows));
}

void SmManager::create_table(const std::string& tab_name, const std::vector<ColDef>& col_defs, Context* context) {
    if (db_.is_table(tab_name)) {
        throw TableExistsError(tab_name);
    }
    // C++20：unordered_map::contains，避免 find/end 比较。
    if (fhs_.contains(tab_name)) {
        throw InternalError("Record handle already exists: " + tab_name);
    }
    if (col_defs.empty()) {
        throw InvalidRecordSizeError(0);
    }

    std::vector<std::string> col_names;
    col_names.reserve(col_defs.size());
    int record_size = 0;
    // C++17 结构化绑定：ColDef 是聚合类型，可拆成 name/type/len。
    // 等价于 for (const auto& col_def : col_defs) { 使用 col_def.name 等 }
    for (const auto& [name, type, len] : col_defs) {
        // C++20 ranges::find：直接对容器查找，无需 begin()/end()。
        if (std::ranges::find(col_names, name) != col_names.end()) {
            throw ColumnExistsError(name);
        }
        col_names.push_back(name);

        const bool fixed_length_mismatch = (type == TYPE_INT && len != static_cast<int>(sizeof(int))) ||
                                           (type == TYPE_FLOAT && len != static_cast<int>(sizeof(float)));
        if (len <= 0 || fixed_length_mismatch) {
            throw InvalidColLengthError(len);
        }
        if (len > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(len);
        }
        const int next_record_size = record_size + len;
        if (next_record_size > RM_MAX_RECORD_SIZE) {
            throw InvalidRecordSizeError(next_record_size);
        }
        record_size = next_record_size;
    }

    // 字段按声明顺序连续存放，offset 是字段在一条记录中的起始位置。
    int curr_offset = 0;
    TabMeta tab;
    tab.name = tab_name;
    tab.cols.reserve(col_defs.size());
    // 同上：结构化绑定展开 ColDef 的三个成员。
    for (const auto& [name, type, len] : col_defs) {
        ColMeta col;
        col.tab_name = tab_name;
        col.name = name;
        col.type = type;
        col.len = len;
        col.offset = curr_offset;
        curr_offset += len;
        tab.cols.push_back(std::move(col));
    }

    // 先构造候选目录，只有文件句柄和 db.meta 都准备成功后才替换当前目录。
    DbMeta next_db = db_;
    next_db.tabs_[tab_name] = tab;
    rm_manager_->create_file(tab_name, record_size);

    try {
        fhs_.emplace(tab_name, rm_manager_->open_file(tab_name));
        write_meta_file(DB_META_NAME, next_db);
        db_ = std::move(next_db);
    } catch (...) {
        // 只清理本次建表产生的句柄和文件，再保留原目录供调用者继续使用。
        if (auto handle = fhs_.find(tab_name); handle != fhs_.end()) {
            rm_manager_->close_file(handle->second.get());
            fhs_.erase(handle);
        }
        rm_manager_->destroy_file(tab_name);
        throw;
    }
}

void SmManager::drop_table(const std::string& tab_name, Context* context) {
    static_cast<void>(context);
    const TabMeta& table = db_.get_table(tab_name);
    if (!fhs_.contains(tab_name)) {
        throw InternalError("Record handle not found for table: " + tab_name);
    }

    // 先验证所有待删除对象，避免删到一半才发现目录不一致。
    for (const auto& index : table.indexes) {
        const std::string index_name = index_manager_->make_index_name(tab_name, index.cols);
        if (!indexes_.contains(index_name) || !index_manager_->exists(tab_name, index.cols)) {
            throw InternalError("Index file or handle not found: " + index_name);
        }
    }
    if (!disk_manager_->is_file(tab_name)) {
        throw InternalError("cannot verify record file: " + tab_name);
    }

    for (const auto& index : table.indexes) {
        const std::string index_name = index_manager_->make_index_name(tab_name, index.cols);
        auto index_handle = indexes_.find(index_name);
        index_manager_->close_index(index_handle->second.get());
        indexes_.erase(index_handle);
        index_manager_->destroy_index(tab_name, index.cols);
    }

    auto file_handle = fhs_.find(tab_name);
    rm_manager_->close_file(file_handle->second.get());
    fhs_.erase(file_handle);
    rm_manager_->destroy_file(tab_name);

    DbMeta next_db = db_;
    next_db.tabs_.erase(tab_name);
    write_meta_file(DB_META_NAME, next_db);
    db_ = std::move(next_db);
}

void SmManager::create_index(const std::string& tab_name,
                             const std::vector<std::string>& col_names,
                             bool unique,
                             Context* context) {
    const TabMeta& table = db_.get_table(tab_name);
    if (col_names.empty()) {
        throw IndexNotFoundError(tab_name, col_names);
    }
    if (table.is_index(col_names)) {
        throw IndexExistsError(tab_name, col_names);
    }

    std::vector<ColMeta> index_columns;
    index_columns.reserve(col_names.size());
    for (const auto& col_name : col_names) {
        if (std::ranges::find_if(index_columns, [&](const ColMeta& column) {
                return column.name == col_name;
            }) != index_columns.end()) {
            throw ColumnExistsError(col_name);
        }
        index_columns.push_back(*table.get_col(col_name));
    }

    IndexMeta index = IndexMeta::make(tab_name, std::move(index_columns), unique);
    const std::string index_name = index_manager_->make_index_name(tab_name, index.cols);
    if (indexes_.contains(index_name) || index_manager_->exists(tab_name, index.cols)) {
        throw IndexExistsError(tab_name, col_names);
    }

    const auto file_handle = fhs_.find(tab_name);
    if (file_handle == fhs_.end()) {
        throw InternalError("Record handle not found for table: " + tab_name);
    }

    std::unique_ptr<BPlusTree> new_index;
    try {
        index_manager_->create_index(index);
        new_index = index_manager_->open_index(tab_name, index.cols);

        RmScan scan(file_handle->second.get());
        while (!scan.is_end()) {
            const Rid rid = scan.rid();
            const std::unique_ptr<RmRecord> record = file_handle->second->get_record(rid, context);
            const std::vector<char> key = index.make_key(record->data);
            new_index->insert_entry(key.data(), rid, context == nullptr ? nullptr : context->transaction());
            scan.next();
        }

        DbMeta next_db = db_;
        TabMeta& next_table = next_db.get_table(tab_name);
        next_table.indexes.push_back(index);
        for (const auto& col_name : col_names) {
            next_table.get_col(col_name)->index = true;
        }

        const auto [_, inserted] = indexes_.emplace(index_name, std::move(new_index));
        if (!inserted) {
            throw InternalError("Duplicate index handle: " + index_name);
        }

        try {
            write_meta_file(DB_META_NAME, next_db);
        } catch (...) {
            auto handle = indexes_.find(index_name);
            index_manager_->close_index(handle->second.get());
            indexes_.erase(handle);
            index_manager_->destroy_index(tab_name, index.cols);
            throw;
        }
        db_ = std::move(next_db);
    } catch (...) {
        // open_index 或导入已有记录失败时，索引尚未写入目录；关闭并删除它就是完整的回滚。
        auto handle = indexes_.find(index_name);
        if (handle != indexes_.end()) {
            try {
                index_manager_->close_index(handle->second.get());
            } catch (...) {
            }
            indexes_.erase(handle);
        } else if (new_index != nullptr) {
            try {
                index_manager_->close_index(new_index.get());
            } catch (...) {
            }
        }
        if (index_manager_->exists(tab_name, index.cols)) {
            try {
                index_manager_->destroy_index(tab_name, index.cols);
            } catch (...) {
            }
        }
        throw;
    }
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<std::string>& col_names, Context* context) {
    static_cast<void>(context);
    const TabMeta& table = db_.get_table(tab_name);
    const auto index = table.get_index_meta(col_names);
    const IndexMeta index_to_drop = *index;
    drop_index_impl(tab_name, index_to_drop);
}

void SmManager::drop_index(const std::string& tab_name, const std::vector<ColMeta>& cols, Context* context) {
    std::vector<std::string> col_names;
    col_names.reserve(cols.size());
    for (const auto& col : cols) {
        col_names.push_back(col.name);
    }
    drop_index(tab_name, col_names, context);
}

void SmManager::drop_index_impl(const std::string& tab_name, const IndexMeta& index) {
    const std::string index_name = index_manager_->make_index_name(tab_name, index.cols);
    auto index_handle = indexes_.find(index_name);
    if (index_handle == indexes_.end() || !index_manager_->exists(tab_name, index.cols)) {
        throw InternalError("Index file or handle not found: " + index_name);
    }

    index_manager_->close_index(index_handle->second.get());
    indexes_.erase(index_handle);
    index_manager_->destroy_index(tab_name, index.cols);

    DbMeta next_db = db_;
    TabMeta& next_table = next_db.get_table(tab_name);
    std::vector<std::string> col_names;
    col_names.reserve(index.cols.size());
    for (const auto& col : index.cols) {
        col_names.push_back(col.name);
    }
    next_table.indexes.erase(next_table.get_index_meta(col_names));

    for (auto& column : next_table.cols) {
        column.index = std::ranges::any_of(next_table.indexes, [&](const IndexMeta& remaining) {
            return std::ranges::any_of(
                remaining.cols,
                [&](const ColMeta& indexed_column) { return indexed_column.name == column.name; });
        });
    }

    write_meta_file(DB_META_NAME, next_db);
    db_ = std::move(next_db);
}
