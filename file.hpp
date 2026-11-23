// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: Copyright OpenBMC Authors

#pragma once

#include <unistd.h>
namespace phosphor
{
namespace gpio
{
/** @class FileDescriptor
 *  @brief Responsible for handling file descriptor
 */
/* @class FileDescriptor
 *  @brief 负责管理文件描述符 */
class FileDescriptor
{
  private:
    /** @brief File descriptor for the gpio input device */
    /* @brief GPIO 输入设备的文件描述符 */
    int fd = -1;

  public:
    FileDescriptor() = default;
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;
    FileDescriptor(FileDescriptor&&) = delete;
    FileDescriptor& operator=(FileDescriptor&&) = delete;

    /** @brief Saves File descriptor and uses it to do file operation
     *
     *  @param[in] fd - File descriptor
     */
    /* @brief 保存文件描述符并用它执行文件操作
     *  @param[in] fd - 文件描述符 */
    explicit FileDescriptor(int fd) : fd(fd)
    {
        // Nothing
        // 无需操作
    }

    ~FileDescriptor()
    {
        if (fd >= 0)
        {
            close(fd);
        }
    }

    int operator()()
    {
        return fd;
    }

    operator bool() const
    {
        return fd != -1;
    }

    void set(int descriptor)
    {
        fd = descriptor;
    }
};

} // namespace gpio
} // namespace phosphor
