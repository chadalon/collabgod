// CollabGod. Copyright (C) 2026 chadalon and contributors.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// `CollabGod --project-file-test`, run by ctest. No window and no audio device.
//
// The crash test is the one that matters: a second copy of this program saves one file
// over and over, a whole new generation each time, and is killed at a random moment, as a
// crash would stop it. After every kill the file must hold one whole generation. A kill
// cannot test a power cut; that is what the flushes in ProjectFile.cpp are for.

#include "ProjectFileTests.h"

#include "ProjectFile.h"

#include <iostream>

#if ! JUCE_WINDOWS
 #include <sys/stat.h>
#endif

namespace collabgod
{
    namespace
    {
        struct Results
        {
            int passed = 0;
            int failed = 0;

            bool expect (bool condition, const juce::String& what)
            {
                if (condition)
                    ++passed;
                else
                {
                    ++failed;
                    std::cout << "FAIL " << what << std::endl;
                }
                return condition;
            }
        };

        juce::String contentsOf (const juce::File& file)
        {
            juce::MemoryBlock block;
            file.loadFileAsData (block);
            return block.toString();
        }

        int temporariesBeside (const juce::File& target)
        {
            return (int) target.getParentDirectory()
                       .findChildFiles (juce::File::findFiles, false, atomicWriteTemporaryPattern (target), juce::File::FollowSymlinks::no)
                       .size();
        }

        // A generation: a header line with its number and size, then that many bytes, all the
        // same value. Any mix of two generations, or a cut-off one, fails `isWholeGeneration`.
        constexpr size_t generationBytes = 4 * 1024 * 1024;

        juce::MemoryBlock generation (int number)
        {
            const auto header = "generation " + juce::String (number) + " " + juce::String ((juce::int64) generationBytes) + "\n";
            juce::MemoryBlock block (header.toRawUTF8(), header.getNumBytesAsUTF8());
            juce::MemoryBlock body (generationBytes);
            body.fillWith ((juce::uint8) (number % 251));
            block.append (body.getData(), body.getSize());
            return block;
        }

        /** The generation the file holds whole, or -1 if it is anything else. */
        int wholeGenerationIn (const juce::File& file)
        {
            juce::MemoryBlock block;
            if (! file.loadFileAsData (block))
                return -1;

            const auto* bytes = static_cast<const char*> (block.getData());
            const auto* end = bytes + block.getSize();
            const auto* newline = std::find (bytes, end, '\n');
            if (newline == end)
                return -1;

            const auto words = juce::StringArray::fromTokens (juce::String (bytes, (size_t) (newline - bytes)), " ", "");
            if (words.size() != 3 || words[0] != "generation" || words[2].getLargeIntValue() != (juce::int64) generationBytes)
                return -1;

            const auto number = words[1].getIntValue();
            const auto* body = newline + 1;
            if ((size_t) (end - body) != generationBytes)
                return -1;

            const auto expected = (char) (juce::uint8) (number % 251);
            return std::all_of (body, end, [expected] (char c) { return c == expected; }) ? number : -1;
        }

        /** A save through a link to a project lands in the file it names, and the link stays a link:
            people keep projects in synced folders and link to them. */
        void links (Results& results, const juce::File& folder)
        {
            const auto realFolder = folder.getChildFile ("synced");
            const auto linkFolder = folder.getChildFile ("links");
            realFolder.createDirectory();
            linkFolder.createDirectory();
            const auto real = realFolder.getChildFile ("song.collabgod");
            const auto link = linkFolder.getChildFile ("song.collabgod");
            writeFileAtomically (real, juce::String ("old"));

            // Windows needs developer mode or an administrator to make one; CI's runners can.
            if (! real.createSymbolicLink (link, true) || ! link.isSymbolicLink())
            {
                std::cout << "SKIP links: this user cannot make a symbolic link" << std::endl;
                return;
            }

            results.expect (writeFileAtomically (link, juce::String ("new")).wasOk(), "a save through a link works");
            results.expect (contentsOf (real) == "new", "a save through a link lands in the file it names");
            results.expect (link.isSymbolicLink(), "and the link is still a link");
            results.expect (temporariesBeside (real) == 0 && temporariesBeside (link) == 0, "and leaves no temporary file");

            // A link to a link: followed to the end.
            const auto second = linkFolder.getChildFile ("again.collabgod");
            if (link.createSymbolicLink (second, true))
            {
                results.expect (writeFileAtomically (second, juce::String ("twice")).wasOk() && contentsOf (real) == "twice",
                                "a save through a chain of links lands in the file at the end");
                results.expect (second.isSymbolicLink() && link.isSymbolicLink(), "and every link stays a link");
            }

            // A link to nothing: refused, rather than making a file where the link was.
            const auto nowhere = linkFolder.getChildFile ("gone.collabgod");
            if (realFolder.getChildFile ("deleted.collabgod").createSymbolicLink (nowhere, true))
            {
                const auto refused = writeFileAtomically (nowhere, juce::String ("x"));
                results.expect (refused.failed(), "a save through a link to nothing fails");
                results.expect (nowhere.isSymbolicLink() && ! realFolder.getChildFile ("deleted.collabgod").exists(),
                                "and changes nothing");
            }
        }

        /** A file the person made read-only is not saved over, and saying so does not take long. */
        void readOnlyFile (Results& results, const juce::File& folder)
        {
            const auto target = folder.getChildFile ("locked.collabgod");
            writeFileAtomically (target, juce::String ("old"));
            target.setReadOnly (true);

            // Root may write to any file on POSIX, so check first. (Windows honours the attribute for everyone.)
            if (target.hasWriteAccess())
            {
                std::cout << "SKIP a read-only file: this user can write to one anyway" << std::endl;
                target.setReadOnly (false);
                return;
            }

            const auto started = juce::Time::getMillisecondCounterHiRes();
            const auto refused = writeFileAtomically (target, juce::String ("new"));
            const auto took = juce::Time::getMillisecondCounterHiRes() - started;
            target.setReadOnly (false);

            results.expect (refused.failed(), "a save over a read-only file fails");
            results.expect (contentsOf (target) == "old", "and leaves it whole");
            results.expect (temporariesBeside (target) == 0, "and leaves no temporary file");
            results.expect (took < 250.0, "and fails at once, not after the retries (" + juce::String (took, 0) + " ms)");
        }

        void basics (Results& results, const juce::File& folder)
        {
            const auto target = folder.getChildFile ("song.collabgod");

            results.expect (writeFileAtomically (target, juce::String ("{\"version\":1}\n")).wasOk(), "a new file is written");
            results.expect (contentsOf (target) == "{\"version\":1}\n", "a new file holds what was written");

            const juce::String text (juce::CharPointer_UTF8 ("{\"name\":\"Gr\xc3\xbc\xc3\x9f" "e\"}\n"));
            results.expect (writeFileAtomically (target, text).wasOk(), "an existing file is replaced");
            juce::MemoryBlock raw;
            target.loadFileAsData (raw);
            results.expect (raw.toString() == text && raw.getSize() == text.getNumBytesAsUTF8(),
                            "the replacement is the new text, as UTF-8 with no byte order mark");
            results.expect (temporariesBeside (target) == 0, "no temporary file is left after a write");

            results.expect (writeFileAtomically (target, juce::String()).wasOk() && target.getSize() == 0,
                            "an empty file can be written");

            const auto nowhere = folder.getChildFile ("missing").getChildFile ("song.collabgod");
            const auto failed = writeFileAtomically (nowhere, juce::String ("x"));
            results.expect (failed.failed() && failed.getErrorMessage().isNotEmpty(), "a write into a missing folder fails with a reason");
            results.expect (! nowhere.exists() && ! nowhere.getParentDirectory().exists(), "a failed write makes nothing");

            const auto aFolder = folder.getChildFile ("a folder");
            aFolder.createDirectory();
            results.expect (writeFileAtomically (aFolder, juce::String ("x")).failed() && aFolder.isDirectory(),
                            "a folder is never written over");

            links (results, folder);
            readOnlyFile (results, folder);

           #if ! JUCE_WINDOWS
            // A target that cannot be replaced: the write fails, and the old file is untouched.
            const auto locked = folder.getChildFile ("locked");
            locked.createDirectory();
            const auto inside = locked.getChildFile ("song.collabgod");
            writeFileAtomically (inside, juce::String ("old"));
            locked.setReadOnly (true);
            // Root ignores folder permissions (CI's Linux container runs as root), so check first.
            const auto probe = locked.getChildFile ("probe");
            if (probe.create().wasOk())
            {
                probe.deleteFile();
                std::cout << "SKIP a read-only folder: this user can write into one anyway" << std::endl;
            }
            else
            {
                const auto refused = writeFileAtomically (inside, juce::String ("new"));
                results.expect (refused.failed(), "a write into a read-only folder fails");
                results.expect (contentsOf (inside) == "old", "and leaves the old file whole");
                results.expect (temporariesBeside (inside) == 0, "and leaves no temporary file");
            }
            locked.setReadOnly (false);

            // A save keeps the file's permissions.
            const auto shared = folder.getChildFile ("shared.collabgod");
            writeFileAtomically (shared, juce::String ("a"));
            chmod (shared.getFullPathName().toRawUTF8(), 0640);
            writeFileAtomically (shared, juce::String ("b"));
            struct stat info {};
            stat (shared.getFullPathName().toRawUTF8(), &info);
            results.expect ((info.st_mode & 0777) == 0640 && contentsOf (shared) == "b", "a save keeps the file's permissions");
           #endif
        }

        void crashes (Results& results, const juce::File& folder)
        {
            const auto target = folder.getChildFile ("crash.collabgod");
            const auto first = generation (0);
            results.expect (writeFileAtomically (target, first.getData(), first.getSize()).wasOk(), "the crash test's first file is written");

            const auto self = juce::File::getSpecialLocation (juce::File::currentExecutableFile);
            auto& random = juce::Random::getSystemRandom();
            constexpr int rounds = 20;
            int last = 0;
            int interrupted = 0;
            int leftOver = 0;

            for (int round = 0; round < rounds; ++round)
            {
                juce::ChildProcess writer;
                if (! results.expect (writer.start (juce::StringArray { self.getFullPathName(), "--project-file-crash-writer",
                                                                        target.getFullPathName() },
                                                    juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr),
                                      "the crash writer starts"))
                    return;

                // Wait until it is writing: the file holds a whole generation newer than the last.
                const auto deadline = juce::Time::getMillisecondCounter() + 30000;
                while (wholeGenerationIn (target) <= last && writer.isRunning() && juce::Time::getMillisecondCounter() < deadline)
                    juce::Thread::sleep (5);

                juce::Thread::sleep (random.nextInt (40));

                // It only stops by itself if a save failed, which it reports.
                if (! writer.isRunning())
                {
                    results.expect (false, "the crash writer stopped by itself: " + writer.readAllProcessOutput().trim());
                    return;
                }

                writer.kill();
                writer.waitForProcessToFinish (10000);

                const auto now = wholeGenerationIn (target);
                results.expect (now >= 0, "after kill " + juce::String (round + 1) + " the file holds one whole generation");
                results.expect (now > last, "after kill " + juce::String (round + 1) + " the writer had saved");

                // A kill in the middle of a write leaves that write's temporary file behind.
                const auto temporaries = temporariesBeside (target);
                if (temporaries > leftOver)
                    ++interrupted;
                leftOver = temporaries;
                last = now;
            }

            // If no kill ever landed in the middle of a write, the test proved nothing.
            std::cout << interrupted << " of " << rounds << " kills landed in the middle of a write" << std::endl;
            results.expect (interrupted > 0, "at least one kill landed in the middle of a write");
        }
    }

    int runCrashWriter (const juce::File& target)
    {
        // Start after whatever the file holds, so the test can see this writer has begun.
        juce::MemoryBlock block;
        target.loadFileAsData (block);
        const auto header = block.toString().upToFirstOccurrenceOf ("\n", false, false);
        auto number = juce::StringArray::fromTokens (header, " ", "")[1].getIntValue();

        for (;;)
        {
            const auto next = generation (++number);
            const auto result = writeFileAtomically (target, next.getData(), next.getSize());
            if (result.failed())
            {
                std::cout << "generation " << number << ": " << result.getErrorMessage() << std::endl;
                return 1;
            }
        }
    }

    int runProjectFileTests()
    {
        const auto folder = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("collabgod-project-file-test-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        folder.createDirectory();

        Results results;
        basics (results, folder);
        crashes (results, folder);

        folder.deleteRecursively();
        std::cout << results.passed << " passed, " << results.failed << " failed" << std::endl;
        return results.failed == 0 ? 0 : 1;
    }
}
