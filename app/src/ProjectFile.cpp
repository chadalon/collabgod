// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Atomic file writes for projects (#25). See ProjectFile.h. Written with each system's own
// calls rather than juce::TemporaryFile, because JUCE's move falls back to a copy on POSIX
// when a rename fails, which is not atomic, and does not flush the folder.

#include "ProjectFile.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <cerrno>
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

        juce::Result renameOver (const juce::File& temporary, const juce::File& target)
        {
            // One rename on the same volume: atomic on NTFS. WRITE_THROUGH returns only once it is on disk.
            if (MoveFileExW (temporary.getFullPathName().toWideCharPointer(),
                             target.getFullPathName().toWideCharPointer(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0)
                return lastError ("cannot replace " + target.getFullPathName());

            return juce::Result::ok();
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
                if (written <= 0)
                    result = lastError ("cannot write " + path);
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

    juce::Result writeFileAtomically (const juce::File& target, const void* data, size_t size)
    {
        if (target.isDirectory())
            return juce::Result::fail (target.getFullPathName() + " is a folder");

        if (! target.getParentDirectory().isDirectory())
            return juce::Result::fail ("no folder " + target.getParentDirectory().getFullPathName());

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
