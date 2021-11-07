#include "Actions.hpp"
#include "MemAllocHeap.hpp"
#include "StatCache.hpp"
#include "BinaryData.hpp"

#if defined(TUNDRA_APPLE)

#include <copyfile.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Banned.hpp"

ExecResult CopyFiles(const FrozenFileAndHash* src_files, const FrozenFileAndHash* target_files, size_t files_count, StatCache* stat_cache, MemAllocHeap* heap)
{
    ExecResult result;
    memset(&result, 0, sizeof(result));
    char tmpBuffer[1024];

    for(size_t i = 0; i < files_count; ++i)
    {
        const char* src_file = src_files[i].m_Filename;
        const char* target_file = target_files[i].m_Filename;

        FileInfo src_file_info = StatCacheStat(stat_cache, src_file);
        if (!src_file_info.Exists())
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s does not exist.", src_file);
            break;
        }

        if (src_file_info.IsDirectory())
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The source path %s is a directory, which is not supported.", src_file);
            break;
        }

        FileInfo dst_file_info = StatCacheStat(stat_cache, target_file);
        if (dst_file_info.Exists())
        {
            if (dst_file_info.IsDirectory())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists as a directory.", target_file);
                break;
            }

            if (dst_file_info.IsReadOnly())
            {
                result.m_ReturnCode = -1;
                snprintf(tmpBuffer, sizeof(tmpBuffer), "The target path %s already exists and is read-only.", target_file);
                break;
            }

            // Normally, this should not be needed, as copyfile will unlink the target file itself, if COPYFILE_UNLINK
            // is used. But there is a special case where this will not work (even if COPYFILE_NOFOLLOW_DST is used):
            // if target_file is a symlink pointing to src_file. In that case, copyfile will conclude that target and 
            // source are the same, and early out, doing nothing.
            //
            // In certain builds, we may use symlinks or copies depending on build settings. So it should be possible
            // to use the copyfiles action to replace a symlink to a file with a copy to a file. So, to make this case
            // work, we need to manually unlink symlinks before calling copyfile.
            if (dst_file_info.IsSymlink())
                unlink(target_file);
        }

        int copyfile_flags = COPYFILE_ALL | COPYFILE_UNLINK | COPYFILE_CLONE;
        // copyfile on older versions of macOS chokes if you ask to a sparse copy of data when the file is 0 bytes
        if (src_file_info.m_Size > 0)
            copyfile_flags |= COPYFILE_DATA_SPARSE;
        result.m_ReturnCode = copyfile(src_file, target_file, nullptr, copyfile_flags);

        // Mark the stat cache dirty regardless of whether we failed or not - the target file is in an unknown state now
        StatCacheMarkDirty(stat_cache, target_file, target_files[i].m_FilenameHash);

        if (result.m_ReturnCode < 0)
        {
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Copying the file %s failed: %s", target_file, strerror(errno));
            break;
        }

        // Force the file to have the current timestamp
        if (src_file_info.IsSymlink())
            result.m_ReturnCode = lutimes(target_file, NULL);
        else
            result.m_ReturnCode = utimes(target_file, NULL);
        if (result.m_ReturnCode < 0)
        {
            snprintf(tmpBuffer, sizeof(tmpBuffer), "Updating the timestamp on the file %s failed: %s", target_file, strerror(errno));
            break;
        }

        // Nothing else to do for symlinks
        if (src_file_info.IsSymlink())
            continue;

        if (src_file_info.IsReadOnly())
        {
            // The source file was readonly, so we will need to wipe the read-only bit on the target file
            struct stat dst_stat;
            result.m_ReturnCode = stat(target_file, &dst_stat);
            if (result.m_ReturnCode < 0)
            {
                snprintf(tmpBuffer, sizeof(tmpBuffer), "stat on the target file %s after the copy failed: %s", target_file, strerror(errno));
                break;
            }

            result.m_ReturnCode = chmod(target_file, (dst_stat.st_mode & 0x00007777) | S_IWUSR);
            if (result.m_ReturnCode < 0)
            {
                snprintf(tmpBuffer, sizeof(tmpBuffer), "Making the target file %s writable failed: %s", target_file, strerror(errno));
                break;
            }
        }

        // Verify that the copied file is the same size as the source.
        // It's OK to use the statcache for this now because we've finished modifying the file
        dst_file_info = StatCacheStat(stat_cache, target_file);
        if (dst_file_info.m_Size != src_file_info.m_Size)
        {
            result.m_ReturnCode = -1;
            snprintf(tmpBuffer, sizeof(tmpBuffer), "The copied file %s is %llu bytes, but the source file %s was %llu bytes.", target_file, dst_file_info.m_Size, src_file, src_file_info.m_Size);
            break;
        }
    }

    if (result.m_ReturnCode != 0)
    {
        InitOutputBuffer(&result.m_OutputBuffer, heap);
        EmitOutputBytesToDestination(&result, tmpBuffer, strlen(tmpBuffer));
    }

    return result;
}

#endif
