// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Atomic file writes for projects (#25). See ProjectFile.h. Written with each system's own
// calls rather than juce::TemporaryFile, because JUCE's move falls back to a copy on POSIX
// when a rename fails, which is not atomic, and does not flush the folder.

#include "ProjectFile.h"

#if JUCE_WINDOWS
 #include <iterator>
 #include <windows.h>
#else
 #include <cerrno>
 #include <climits>
 #include <cstdlib>
 #include <cstring>
 #include <fcntl.h>
 #include <sys/stat.h>
 #include <unistd.h>
#endif

namespace collabgod
{
    namespace
    {
        juce::File temporaryBeside (const juce::File& target)
        {
            auto& random = juce::Random::getSystemRandom();
            const auto tag = juce::String::toHexString (random.nextInt64()).paddedLeft ('0', 16);
            return target.getSiblingFile ("." + target.getFileName() + "." + tag + ".tmp");
        }

       #if JUCE_WINDOWS
        juce::Result lastError (const juce::String& what)
        {
            const auto code = GetLastError();
            wchar_t* message = nullptr;
            FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                            nullptr, code, 0, (LPWSTR) &message, 0, nullptr);
            const auto text = message != nullptr ? juce::String (message).trim() : juce::String ((int) code);
            LocalFree (message);
            return juce::Result::fail (what + ": " + text);
        }

        juce::Result writeTemporary (const juce::File& temporary, const void* data, size_t size)
        {
            const auto handle = CreateFileW (temporary.getFullPathName().toWideCharPointer(), GENERIC_WRITE, 0,
                                             nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return lastError ("cannot create " + temporary.getFullPathName());

            auto result = juce::Result::ok();
            auto* bytes = static_cast<const char*> (data);
            for (size_t done = 0; done < size && result.wasOk();)
            {
                const auto chunk = (DWORD) juce::jmin (size - done, (size_t) (1 << 30));
                DWORD written = 0;
                if (WriteFile (handle, bytes + done, chunk, &written, nullptr) == 0 || written == 0)
                    result = lastError ("cannot write " + temporary.getFullPathName());
                done += written;
            }

            // Down to the disk before the rename, or a crash could leave the new name on old blocks.
            if (result.wasOk() && FlushFileBuffers (handle) == 0)
                result = lastError ("cannot flush " + temporary.getFullPathName());

            if (CloseHandle (handle) == 0 && result.wasOk())
                result = lastError ("cannot close " + temporary.getFullPathName());

            return result;
        }

        /** The file a link at `target` points to, so the save lands there and the link stays a link. */
        juce::Result resolve (const juce::File& target, juce::File& real)
        {
            real = target;
            const auto attributes = GetFileAttributesW (target.getFullPathName().toWideCharPointer());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0)
                return juce::Result::ok();

            // Opening follows the link (or junction, or cloud placeholder) to what it names.
            const auto handle = CreateFileW (target.getFullPathName().toWideCharPointer(), 0,
                                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                return lastError (target.getFullPathName() + " is a link to nothing");

            wchar_t path[32768] {};
            const auto length = GetFinalPathNameByHandleW (handle, path, (DWORD) std::size (path), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            auto result = length == 0 || length >= std::size (path) ? lastError ("cannot follow " + target.getFullPathName())
                                                                    : juce::Result::ok();
            CloseHandle (handle);
            if (result.failed())
                return result;

            // The answer is in \\?\ form: \\?\C:\... or \\?\UNC\server\share\...
            auto name = juce::String (path);
            if (name.startsWith ("\\\\?\\UNC\\"))
                name = "\\\\" + name.substring (8);
            else if (name.startsWith ("\\\\?\\"))
                name = name.substring (4);
            real = juce::File (name);
            return juce::Result::ok();
        }

        /** A file the person made read-only is not saved over. Checked first, since the rename's
            retries would otherwise spend a second on it before giving up. */
        juce::Result checkWritable (const juce::File& target)
        {
            const auto attributes = GetFileAttributesW (target.getFullPathName().toWideCharPointer());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0)
                return juce::Result::fail (target.getFullPathName() + " is read-only");
            return juce::Result::ok();
        }

        juce::Result renameOver (const juce::File& temporary, const juce::File& target)
        {
            // One rename on the same volume: atomic on NTFS. WRITE_THROUGH returns only once it is on disk.
            // Another program reading the project at that moment (a virus scanner, the search indexer, a
            // backup) can make the rename fail with a sharing error; that passes, so try again for a while.
            for (int attempt = 0;; ++attempt)
            {
                if (MoveFileExW (temporary.getFullPathName().toWideCharPointer(),
                                 target.getFullPathName().toWideCharPointer(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0)
                    return juce::Result::ok();

                const auto code = GetLastError();
                const auto passing = code == ERROR_ACCESS_DENIED || code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION;
                if (! passing || attempt == 40)
                {
                    SetLastError (code);
                    return lastError ("cannot replace " + target.getFullPathName());
                }

                Sleep (25);
            }
        }
       #else
        juce::Result lastError (const juce::String& what)
        {
            return juce::Result::fail (what + ": " + juce::String (std::strerror (errno)));
        }

        /** fsync, except on macOS, where only F_FULLFSYNC asks the drive to write its cache out. */
        int flushToDisk (int fd)
        {
           #if JUCE_MAC
            if (fcntl (fd, F_FULLFSYNC) == 0)
                return 0;
            // Some file systems (network shares, FAT) do not support it.
           #endif
            return fsync (fd);
        }

        juce::Result writeTemporary (const juce::File& temporary, const void* data, size_t size, mode_t mode)
        {
            const auto path = temporary.getFullPathName();
            const auto fd = open (path.toRawUTF8(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
            if (fd < 0)
                return lastError ("cannot create " + path);

            auto result = juce::Result::ok();

            // open() applies the umask; keep the mode the target already had.
            if (fchmod (fd, mode) != 0)
                result = lastError ("cannot set the mode of " + path);

            auto* bytes = static_cast<const char*> (data);
            for (size_t done = 0; done < size && result.wasOk();)
            {
                const auto written = write (fd, bytes + done, size - done);
                if (written < 0 && errno == EINTR)
                    continue;
                if (written < 0)
                    result = lastError ("cannot write " + path);
                else if (written == 0)
                    result = juce::Result::fail ("cannot write " + path + ": no bytes were written"); // errno is not set
                else
                    done += (size_t) written;
            }

            // Down to the disk before the rename, or a crash could leave the new name on blocks never written.
            if (result.wasOk() && flushToDisk (fd) != 0)
                result = lastError ("cannot flush " + path);

            if (close (fd) != 0 && result.wasOk())
                result = lastError ("cannot close " + path);

            return result;
        }

        juce::Result renameOver (const juce::File& temporary, const juce::File& target)
        {
            if (rename (temporary.getFullPathName().toRawUTF8(), target.getFullPathName().toRawUTF8()) != 0)
                return lastError ("cannot replace " + target.getFullPathName());

            // The rename is a change to the folder: flush the folder so it survives a power cut. Some
            // file systems refuse to flush a folder; the file itself is already safe by then.
            const auto folder = open (target.getParentDirectory().getFullPathName().toRawUTF8(),
                                      O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (folder >= 0)
            {
                flushToDisk (folder);
                close (folder);
            }

            return juce::Result::ok();
        }

        /** The file a symbolic link at `target` points to, through any chain of links, so the save
            lands there and the link stays a link. A folder link on the way needs nothing: the
            temporary file is made in whatever folder the path leads to. */
        juce::Result resolve (const juce::File& target, juce::File& real)
        {
            real = target;
            struct stat info {};
            if (lstat (target.getFullPathName().toRawUTF8(), &info) != 0 || ! S_ISLNK (info.st_mode))
                return juce::Result::ok();

            char path[PATH_MAX] {};
            if (realpath (target.getFullPathName().toRawUTF8(), path) == nullptr)
                return lastError (target.getFullPathName() + " is a link to nothing");

            real = juce::File (juce::String (juce::CharPointer_UTF8 (path)));
            return juce::Result::ok();
        }

        /** A file the person made read-only is not saved over, as on Windows. POSIX would allow it,
            since replacing a file needs only the folder to be writable. */
        juce::Result checkWritable (const juce::File& target)
        {
            if (access (target.getFullPathName().toRawUTF8(), F_OK) == 0 && access (target.getFullPathName().toRawUTF8(), W_OK) != 0)
                return lastError (target.getFullPathName() + " is read-only");
            return juce::Result::ok();
        }

        /** The target's permissions if it exists, so a save does not change who may read it. */
        mode_t modeFor (const juce::File& target)
        {
            struct stat info {};
            if (stat (target.getFullPathName().toRawUTF8(), &info) == 0)
                return info.st_mode & 07777;
            return 0644;
        }
       #endif
    }

    juce::String atomicWriteTemporaryPattern (const juce::File& target)
    {
        return "." + target.getFileName() + ".*.tmp";
    }

    juce::Result writeFileAtomically (const juce::File& given, const void* data, size_t size)
    {
        // Save through a link to the file it names, never over the link itself.
        juce::File target;
        if (const auto resolved = resolve (given, target); resolved.failed())
            return resolved;

        if (target.isDirectory())
            return juce::Result::fail (target.getFullPathName() + " is a folder");

        if (! target.getParentDirectory().isDirectory())
            return juce::Result::fail ("no folder " + target.getParentDirectory().getFullPathName());

        if (const auto writable = checkWritable (target); writable.failed())
            return writable;

        const auto temporary = temporaryBeside (target);

       #if JUCE_WINDOWS
        auto result = writeTemporary (temporary, data, size);
       #else
        auto result = writeTemporary (temporary, data, size, modeFor (target));
       #endif

        if (result.wasOk())
            result = renameOver (temporary, target);

        if (result.failed())
            temporary.deleteFile();

        return result;
    }

    juce::Result writeFileAtomically (const juce::File& target, const juce::String& text)
    {
        const auto utf8 = text.toUTF8();
        return writeFileAtomically (target, utf8.getAddress(), utf8.sizeInBytes() - 1);
    }
}
