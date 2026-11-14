// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.
//
// Connects instances over TCP. One instance hosts; the others join it. The host
// passes each member's messages on to every other member, as the relay would, so
// a lobby of three works the same as one of two. Callbacks arrive on the message
// thread.
//
// The product sends this through the relay (spikes/relay) instead; this exists
// so the spike runs with no relay, on one machine or two on the same network.

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <functional>

namespace spike
{

class Link
{
public:
    Link();
    ~Link();

    /** Listens on `port` (0 picks a free one). Returns the port, or 0 if it failed. */
    int host (int port);
    /** Connects to a host. Returns false if it could not. */
    bool join (const juce::String& address, int port);
    void close();

    bool isHost() const noexcept       { return server != nullptr; }
    int numConnections() const;

    /** Sends to every other member. */
    void send (const juce::String& message);

    std::function<void (const juce::String&)> onMessage;
    std::function<void()> onPeersChanged;

    std::uint64_t bytesOut() const noexcept    { return sentBytes.load(); }
    std::uint64_t bytesIn() const noexcept     { return receivedBytes.load(); }
    std::uint64_t messagesOut() const noexcept { return sentMessages.load(); }
    std::uint64_t messagesIn() const noexcept  { return receivedMessages.load(); }

private:
    class Connection;
    class Server;

    void received (Connection& from, const juce::MemoryBlock&);
    void connectionChanged();

    std::unique_ptr<Server> server;
    juce::OwnedArray<Connection> connections;
    std::atomic<std::uint64_t> sentBytes { 0 }, receivedBytes { 0 }, sentMessages { 0 }, receivedMessages { 0 };

    JUCE_DECLARE_WEAK_REFERENCEABLE (Link)
    JUCE_DECLARE_NON_COPYABLE (Link)
};

} // namespace spike
