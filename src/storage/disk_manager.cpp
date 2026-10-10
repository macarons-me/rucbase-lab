// Copyright (c) 2023-2027 Renmin University of China
// SPDX-License-Identifier: MulanPSL-2.0

#include "storage/disk_manager.h"

#include <assert.h>    // for assert
#include <fcntl.h>     // for open
#include <sys/stat.h>  // for stat
#include <unistd.h>    // for lseek

#include <algorithm>
#include <filesystem>
#include <limits>
#include <system_error>

#include "common/defs.h"
#include "common/errors.h"

namespace fs = std::filesystem;

DiskManager::DiskManager() = default;

/**
 * @description: 将数据写入文件的指定磁盘页面中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 写入目标页面的page_id
 * @param {char} *offset 要写入磁盘的数据
 * @param {int} num_bytes 要写入磁盘的数据大小
 */
void DiskManager::write_page(int fd, page_id_t page_no, const char* offset, int num_bytes) {
    // 1.lseek()定位到文件头，通过(fd,page_no)可以定位指定页面及其在磁盘文件中的偏移量
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    if (lseek(fd, file_offset, SEEK_SET) < 0) {
        throw UnixError();
    }
    // 2.调用write()函数
    // 注意write返回值与num_bytes不等时 throw InternalError("DiskManager::write_page Error");
    ssize_t bytes_written = write(fd, offset, num_bytes);
    if (bytes_written != num_bytes) {
        throw InternalError("DiskManager::write_page Error");
    }
}

/**
 * @description: 读取文件中指定编号的页面中的部分数据到内存中
 * @param {int} fd 磁盘文件的文件句柄
 * @param {page_id_t} page_no 指定的页面编号
 * @param {char} *offset 读取的内容写入到offset中
 * @param {int} num_bytes 读取的数据量大小
 */
void DiskManager::read_page(int fd, page_id_t page_no, char* offset, int num_bytes) {
    // 1.lseek()定位到文件头，通过(fd,page_no)可以定位指定页面及其在磁盘文件中的偏移量
    off_t file_offset = static_cast<off_t>(page_no) * PAGE_SIZE;
    if (lseek(fd, file_offset, SEEK_SET) < 0) {
        throw UnixError();
    }
    // 2.调用read()函数
    // 注意read返回值与num_bytes不等时，throw InternalError("DiskManager::read_page Error");
    ssize_t bytes_read = read(fd, offset, num_bytes);
    if (bytes_read != num_bytes) {
        throw InternalError("DiskManager::read_page Error");
    }
}

/**
 * @description: 分配一个新的页号
 * @return {page_id_t} 分配的新页号
 * @param {int} fd 指定文件的文件句柄
 */
page_id_t DiskManager::allocate_page(int fd) {
    // 简单的自增分配策略，指定文件的页面编号加1
    assert(fd >= 0 && fd < MAX_FD);
    return fd2pageno_[fd]++;
}

void DiskManager::deallocate_page(__attribute__((unused)) page_id_t page_id) {}

bool DiskManager::is_dir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void DiskManager::create_dir(const std::string& path) {
    std::error_code error;
    if (fs::create_directory(path, error)) {
        return;
    }
    if (error) {
        throw InternalError("cannot create directory " + path + ": " + error.message());
    }
    throw FileExistsError(path);
}

void DiskManager::destroy_dir(const std::string& path) {
    std::error_code error;
    const auto removed = fs::remove_all(path, error);
    if (error) {
        throw InternalError("cannot remove directory " + path + ": " + error.message());
    }
    if (removed == 0) {
        throw FileNotFoundError(path);
    }
}

/**
 * @description: 判断指定路径文件是否存在
 * @return {bool} 若指定路径文件存在则返回true
 * @param {string} &path 指定路径文件
 */
bool DiskManager::is_file(const std::string& path) {
    // 用struct stat获取文件信息
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

/**
 * @description: 用于创建指定路径文件
 * @return {*}
 * @param {string} &path
 */
void DiskManager::create_file(const std::string& path) {
    // 注意不能重复创建相同文件
    if (is_file(path)) {
        throw FileExistsError(path);
    }
    // 调用open()函数，使用O_CREAT模式
    int fd = open(path.c_str(), O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        throw UnixError();
    }
    close(fd);
}

/**
 * @description: 删除指定路径的文件
 * @param {string} &path 文件所在路径
 */
void DiskManager::destroy_file(const std::string& path) {
    // 注意不能删除未关闭的文件
    if (path2fd_.count(path) > 0) {
        throw FileNotClosedError(path);
    }
    // 调用unlink()函数
    if (unlink(path.c_str()) != 0) {
        throw FileNotFoundError(path);
    }
}

/**
 * @description: 打开指定路径文件
 * @return {int} 返回打开的文件的文件句柄
 * @param {string} &path 文件所在路径
 */
int DiskManager::open_file(const std::string& path) {
    // 注意不能重复打开相同文件
    if (path2fd_.count(path) > 0) {
        return path2fd_[path];
    }
    // 调用open()函数，使用O_RDWR模式
    int fd = open(path.c_str(), O_RDWR);
    if (fd < 0) {
        throw FileNotFoundError(path);
    }
    // 更新文件打开列表
    path2fd_[path] = fd;
    fd2path_[fd] = path;
    return fd;
}

/**
 * @description:用于关闭指定路径文件
 * @param {int} fd 打开的文件的文件句柄
 */
void DiskManager::close_file(int fd) {
    // 注意不能关闭未打开的文件
    auto it = fd2path_.find(fd);
    if (it == fd2path_.end()) {
        throw FileNotOpenError(fd);
    }
    std::string path = it->second;
    // 调用close()函数
    if (::close(fd) != 0) {
        throw UnixError();
    }
    // 更新文件打开列表
    fd2path_.erase(it);
    path2fd_.erase(path);
}

/**
 * @description: 获得文件的大小
 * @return {int} 文件的大小
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_size(const std::string& file_name) {
    struct stat stat_buf;
    int rc = stat(file_name.c_str(), &stat_buf);
    if (rc != 0) {
        return -1;
    }
    if (stat_buf.st_size < 0 || stat_buf.st_size > std::numeric_limits<int>::max()) {
        throw InternalError("file is too large for the teaching storage manager: " + file_name);
    }
    return static_cast<int>(stat_buf.st_size);
}

int64_t DiskManager::get_file_size(int fd) {
    struct stat stat_buf;
    int rc = fstat(fd, &stat_buf);
    return rc == 0 ? stat_buf.st_size : -1;
}

/**
 * @description: 根据文件句柄获得文件名
 * @return {string} 文件句柄对应文件的文件名
 * @param {int} fd 文件句柄
 */
std::string DiskManager::get_file_name(int fd) {
    // C++20：unordered_map::contains 判断键是否存在，等价于 count(fd)!=0 / find!=end。
    if (!fd2path_.contains(fd)) {
        throw FileNotOpenError(fd);
    }
    return fd2path_[fd];
}

/**
 * @description:  获得文件名对应的文件句柄
 * @return {int} 文件句柄
 * @param {string} &file_name 文件名
 */
int DiskManager::get_file_fd(const std::string& file_name) {
    if (!path2fd_.contains(file_name)) {
        return open_file(file_name);
    }
    return path2fd_[file_name];
}

/**
 * @description:  读取日志文件内容
 * @return {int} 返回读取的数据量，若为-1说明读取数据的起始位置超过了文件大小
 * @param {char} *log_data 读取内容到log_data中
 * @param {int} size 读取的数据量大小
 * @param {int} offset 读取的内容在文件中的位置
 */
int DiskManager::read_log(char* log_data, int size, int offset) {
    // read log file from the previous end
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    if (log_fd_ < 0) {
        throw FileNotOpenError(log_fd_);
    }
    int file_size = get_file_size(LOG_FILE_NAME);
    if (offset > file_size) {
        return -1;
    }

    size = std::min(size, file_size - offset);
    if (size == 0) return 0;
    if (lseek(log_fd_, offset, SEEK_SET) < 0) {
        throw UnixError();
    }
    ssize_t bytes_read = read(log_fd_, log_data, size);
    if (bytes_read < 0) {
        throw UnixError();
    }
    if (bytes_read != size) {
        throw InternalError("short read from database log");
    }
    return static_cast<int>(bytes_read);
}

/**
 * @description: 写日志内容
 * @param {char} *log_data 要写入的日志内容
 * @param {int} size 要写入的内容大小
 */
void DiskManager::write_log(char* log_data, int size) {
    if (log_fd_ == -1) {
        log_fd_ = open_file(LOG_FILE_NAME);
    }
    if (log_fd_ < 0) {
        throw FileNotOpenError(log_fd_);
    }

    // write from the file_end
    if (lseek(log_fd_, 0, SEEK_END) < 0) {
        throw UnixError();
    }
    ssize_t bytes_write = write(log_fd_, log_data, size);
    if (bytes_write != size) {
        throw UnixError();
    }
}
