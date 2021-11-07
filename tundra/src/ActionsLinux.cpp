#include <fcntl.h>

#include "Actions.hpp"
#include "MemAllocHeap.hpp"
#include "StatCache.hpp"
#include "BinaryData.hpp"
#include "MemAllocLinear.hpp"

#if defined(TUNDRA_LINUX)

#include <filesystem>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/fs.h>
#ifdef FICLONE
#include <sys/ioctl.h>
#endif

#include "Banned.hpp"

char* ReadSymbolicLink(const char* path, MemAllocLinear* allocator, struct stat* stat_info)
{
    struct stat s;
    if (stat_info == nullptr)
    {
        stat_info = &s;
        if (lstat(path, &s) != 0)
        {
            Log(kError, "Failed to stat %s", path);
            return nullptr;
        }
    }

    const size_t bufferSize = stat_info->st_size + 1;
    char* link_target = static_cast<char*>(LinearAllocate(allocator, bufferSize, 1));

    int link_size = readlink(path, link_target, bufferSize);
    if (link_size >= bufferSize)
        return nullptr;

    link_target[link_size] = 0;
    return link_target;
}

ExecResult CopyFiles(const FrozenFileAndHash* src_files, const FrozenFileAndHash* target_files, size_t files_count, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));
    char tmpBuffer[1024];

    int in_file = -1, out_file = -1;
    int temporary_pipe[2] = { -1, -1 };
    pipe(temporary_pipe);

    MemAllocLinear scratch;
    LinearAllocInit(&scratch, heap, 4096, "CopyFiles scratch memory");

    for(size_t i = 0; i < files_count; ++i)
    {
        LinearAllocReset(&scratch);

        const char* src_file = src_files[i].m_Filename;
        const char* dst_file = target_files[i].m_Filename;

        // The StatCache is not used for the file info because we need full stat info (file modes, etc) which FileInfo doesn't give us
        struct stat src_stat;
        if (lstat(src_file, &src_stat) != 0)
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The properties of source file %s could not be retrieved: %s", src_file, strerror(errno));
            break;
        }

        if (S_ISDIR(src_stat.st_mode))
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s is a directory, which is not supported.", src_file);
            break;
        }

        struct stat dst_stat;
        if (lstat(dst_file, &dst_stat) != 0)
        {
            if (errno != ENOENT)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The properties of the destination file %s could not be retrieved: %s", src_file, strerror(errno));
                break;
            }
        }
        else
        {
            if (S_ISDIR(dst_stat.st_mode))
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists as a directory.", dst_file);
                break;
            }

            if ((dst_stat.st_mode & S_IWRITE) == 0)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists and is read-only.", dst_file);
                break;
            }

            // Always remove the target file first if it existed, to avoid any weirdnesses when opening it for writing
            unlink(dst_file);
        }

        if ((src_stat.st_mode & S_IFMT) == S_IFLNK)
        {
            // It's a symlink
            char* link_target = ReadSymbolicLink(src_file, &scratch, &src_stat);
            if (link_target == nullptr)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The source symlink %s could not be read.", src_file);
                break;
            }

            if (symlink(link_target, dst_file) != 0)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target symlink %s could not be created.", dst_file);
                break;
            }

            // Verify the link was copied correctly
            char* final_target = ReadSymbolicLink(dst_file, &scratch, nullptr);
            if (final_target == nullptr)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The destination symlink %s could not be read.", dst_file);
                break;
            }

            if (strcmp(link_target, final_target) != 0)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied symlink %s had contents \"%s\", but the source symlink %s had different contents \"%s\".", dst_file, final_target, src_file, link_target);
                break;
            }

            // Mark the stat cache dirty
            StatCacheMarkDirty(stat_cache, dst_file, target_files[i].m_FilenameHash);
        }
        else
        {
            // It's a regular file
            in_file = open(src_file, O_RDONLY);
            if (in_file == -1)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The source file %s could not be opened for reading: %s", src_file, strerror(errno));
                break;
            }

            // Ensure that the target file is opened with a writable mode, even if the input file was readonly
            out_file = open(dst_file, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, (src_stat.st_mode & 0x0fff) | S_IWUSR);
            if (out_file == -1)
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The destination file %s could not be opened for writing: %s", dst_file, strerror(errno));
                break;
            }

            do {
#ifdef FICLONE
                // Try the IOCTL. We don't particularly care about errors here, we'll just fall back if this fails
                if (ioctl(out_file, FICLONE, in_file) != -1)
                {
                    // It worked!
                    break;
                }
#endif

                // Otherwise, next fastest method is to ask the kernel to do the copy using splice
                size_t bytes_in, bytes_out;
                bool successful_splice_to_target = false;
                do
                {
                    bytes_in = splice(in_file, NULL, temporary_pipe[1], NULL, src_stat.st_blksize, 0);
                    if (bytes_in == -1)
                    {
                        result.m_ReturnCode = -1;
                        snprintf(tmpBuffer, sizeof(tmpBuffer), "Reading from the source file using 'splice' %s failed: %s", src_file, strerror(errno));
                        break;
                    }

                    bytes_out = splice(temporary_pipe[0], NULL, out_file, NULL, bytes_in, 0);
                    if (bytes_out == -1)
                    {
                        // If the writing splice call fails the first time with EINVAL, it might be that the target file system does not support splicing.
                        // This was observed on ecryptfs encrypted file systems. We encountered the same issue with sendfile as well.
                        if (errno == EINVAL && !successful_splice_to_target)
                        {
                            // Unfortunately this fallback doesn't allow us to reuse the already opened file handles, but at least 
                            std::error_code fs_copy_error;
                            if (!std::filesystem::copy_file(src_file, dst_file, std::filesystem::copy_options::overwrite_existing, fs_copy_error))
                            {
                                result.m_ReturnCode = -1;
                                snprintf(tmpBuffer, sizeof(tmpBuffer), "Copying file from %s to %s using std::filesystem failed: %s", src_file, dst_file, fs_copy_error.message().c_str());
                            }
                            break;
                        }

                        result.m_ReturnCode = -1;
                        snprintf(tmpBuffer, sizeof(tmpBuffer), "Writing to the destination file using 'splice' %s failed: %s", dst_file, strerror(errno));
                        break;
                    }

                    // Splice worked at least once.
                    successful_splice_to_target = true;
                } while (bytes_out > 0);
            } while (false);

            close(in_file);
            in_file = -1;

            close(out_file);
            out_file = -1;

            // Mark the stat cache dirty regardless of whether we failed or not - the target file is in an unknown state now
            StatCacheMarkDirty(stat_cache, dst_file, target_files[i].m_FilenameHash);

            if (result.m_ReturnCode == 0)
            {
                // Verify that the copied file is the same size as the source.
                // It's OK to use the statcache for this now because we've finished modifying the file
                FileInfo dst_file_info = StatCacheStat(stat_cache, dst_file);
                if (dst_file_info.m_Size != src_stat.st_size)
                {
                    result.m_ReturnCode = -1;
                    snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied file %s is %" PRId64 " bytes, but the source file %s was %" PRId64 " bytes.", dst_file, dst_file_info.m_Size, src_file, src_stat.st_size);
                    break;
                }
            }
        }

        if (result.m_ReturnCode < 0)
            break;
    }

    close(temporary_pipe[0]);
    close(temporary_pipe[1]);
    if (in_file != -1)
        close(in_file);
    if (out_file != -1)
        close(out_file);

    if (result.m_ReturnCode != 0)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));
    }

    LinearAllocDestroy(&scratch);

    return result;
}

#endif
