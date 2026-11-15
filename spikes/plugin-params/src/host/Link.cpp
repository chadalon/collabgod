// SPDX-License-Identifier: GPL-3.0-or-later
// Spike for issue #12. Throwaway code: product code never includes this.

#include "Link.h"

namespace spike
{

namespace
{
constexpr juce::uint32 magic = 0xc011a6d0;   // frames from anything else are refused
}

class Link::Connection final : public juce::InterprocessConnection
{
public:
    explicit Connection (Link& l) : juce::InterprocessConnection (true, magic), link (l) {}
    ~Connection() override { disconnect(); }

    void connectionMade() override        { link.connectionChanged(); }
    void connectionLost() override        { link.connectionChanged(); }
    void messageReceived (const juce::MemoryBlock& m) override { link.received (*this, m); }

private:
    Link& link;
};

class Link::Server final : public juce::InterprocessConnectionServer
{
public:
    explicit Server (Link& l) : link (l) {}
    ~Server() override { stop(); }

    juce::InterprocessConnection* createConnectionObject() override
    {
        // Called on the server's thread; the connection is used on the message thread.
        auto* c = new Connection (link);
        juce::MessageManager::callAsync ([weak = juce::WeakReference<Link> (&link), c]
        {
            if (auto* l = weak.get())
            {
                l->connections.add (c);
                l->connectionChanged();
            }
            else
            {
                delete c;
            }
        });
        return c;
    }

private:
    Link& link;
};

Link::Link() = default;

Link::~Link()
{
    close();
}

int Link::host (int port)
{
    close();
    server = std::make_unique<Server> (*this);

    if (! server->beginWaitingForSocket (port))
    {
        server.reset();
        return 0;
    }

    return server->getBoundPort();
}

bool Link::join (const juce::String& address, int port)
{
    close();
    auto c = std::make_unique<Connection> (*this);

    if (! c->connectToSocket (address, port, 3000))
        return false;

    connections.add (c.release());
    connectionChanged();
    return true;
}

void Link::close()
{
    if (server != nullptr)
        server->stop();

    connections.clear();
    server.reset();
}

int Link::numConnections() const
{
    int n = 0;
    for (auto* c : connections)
        if (c->isConnected())
            ++n;
    return n;
}

void Link::send (const juce::String& message)
{
    const juce::MemoryBlock block (message.toRawUTF8(), message.getNumBytesAsUTF8());

    for (auto* c : connections)
    {
        if (c->isConnected() && c->sendMessage (block))
        {
            sentBytes += block.getSize();
            ++sentMessages;
        }
    }
}

void Link::received (Connection& from, const juce::MemoryBlock& block)
{
    receivedBytes += block.getSize();
    ++receivedMessages;

    // The host passes everything on to the other members, as the relay does.
    if (isHost())
        for (auto* c : connections)
            if (c != &from && c->isConnected())
                c->sendMessage (block);

    if (onMessage)
        onMessage (block.toString());
}

void Link::connectionChanged()
{
    if (onPeersChanged)
        onPeersChanged();
}

} // namespace spike
