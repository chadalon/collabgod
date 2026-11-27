# Ideas (parking lot)

These are ideas only. None is designed, estimated or scheduled, and none should shape v1 decisions beyond not blocking them. Each has an issue labelled `idea` for discussion.

- **Bounce toggle for unshared plugins.** The owner bounces the track to a file so the other person can hear it. Switching back to the live plugin for tweaks mutes it for the other person until the next bounce. (PLG-9)
- **Low-latency live playing.** Live playing tight enough to play in time together, from two cities. (LIV-4, #121)
- **Live audio input.** A voice or guitar on an armed track heard by the lobby as it is played. Waits with recording. (LIV-5, #122)
- **Try someone's playing through your own sound.** While one person plays, another tries presets and effects on that playing, heard only by themselves, then applies what works. (LIV-6, #123)
- **Controller profiles.** Known controllers load a ready-made mapping with pad lights and displays, from a profile format anyone can add to. (HW-4, #128)
- **Pad step sequencer and drum layout.** Pads play and select drum slots, and steps are entered and lit on the pad grid. Pairs with the first-party sequencer below. (HW-5, #129)
- **Lobby-aware controllers.** Pad lights and displays show other members' edits and live playing in their colours. (HW-6, #130)
- **Clip launching from a pad grid.** Launch clips and scenes from pads, which needs a clip-launching view the app does not have. (#131)
- **Bigger lobbies.** Three or more people working at once. The protocol and session format do not assume two (ADR-0006), but plugin sharing, sample transfer and the interface are designed and tested for two.
- **Accounts.** Being the same person across devices, with a login, a profile and friends. v1 ties identity to one install of the app (ADR-0006).
- **Own instruments and effects.** First-party synths, effects and a signature sequencer, with workflows of their own.
- **Browser DAW.** A version that runs in a tab.
- **Web hub.** Manage songs, sample library and friends online, and post finished tracks.
- **Voice chat** inside the lobby.
- **Cosmetic skins.** Looks for the things other people in a lobby see, such as playheads, pointers, effects and themes. Cosmetic only, and no paid random items. Ownership would be checked by a hosted service, which needs accounts. (#106)
- **Achievements.** Earned for making music together. Everyone earns them, and paid users claim the rewards. Needs accounts and a hosted service. (#116)
- **More formats and platforms.** AU, CLAP and Linux.
- **Version history.** Earlier versions of a project to look through and return to. A save keeps one backup of the version it replaced (#161), which covers a damaged save but not a mistake saved twice. (#163)
- **Studio One import.** Bring existing projects across, either through the open DAWproject format or by reusing the `.song` parsing in [Studio-One-File-Finder](https://github.com/chadalon/Studio-One-File-Finder).
