#include "McpServer.h"

#include "forge/core/Log.h"

#include <httplib.h>

#include <chrono>
#include <utility>

namespace forge {

namespace {

// Non-browser clients (Claude Code) send no Origin; browsers must come from
// this machine. Anything else is a DNS-rebinding attempt -> 403 (MCP spec MUST).
bool OriginAllowed(const httplib::Request& req)
{
    if (!req.has_header("Origin"))
        return true;
    const std::string origin = req.get_header_value("Origin");
    return origin.rfind("http://localhost", 0) == 0 ||
           origin.rfind("http://127.0.0.1", 0) == 0;
}

} // namespace

McpServer::McpServer(McpProtocol& protocol, int port)
    : m_Protocol(protocol), m_Port(port), m_Http(std::make_unique<httplib::Server>())
{
    m_Http->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
        if (!OriginAllowed(req)) {
            res.status = 403;
            return;
        }
        if (!m_Accepting) {
            res.status = 503;
            return;
        }

        auto pending = std::make_unique<Pending>();
        pending->body = req.body;
        std::future<std::string> reply = pending->response.get_future();
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_Queue.push_back(std::move(pending));
        }

        // Poll rather than wait unbounded: if the editor is shutting down (or
        // stuck in a modal), m_Accepting drops and we bail instead of pinning
        // the listener thread forever.
        while (reply.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready) {
            if (!m_Accepting) {
                res.status = 503;
                return;
            }
        }

        const std::string body = reply.get();
        if (body.empty())
            res.status = 202; // notification: accepted, no response owed
        else
            res.set_content(body, "application/json");
    });

    // Minimal Streamable-HTTP profile: no server-initiated SSE stream and no
    // sessions, so GET and DELETE get the spec-sanctioned 405.
    m_Http->Get("/mcp", [](const httplib::Request&, httplib::Response& res) { res.status = 405; });
    m_Http->Delete("/mcp",
                   [](const httplib::Request&, httplib::Response& res) { res.status = 405; });

    if (!m_Http->bind_to_port("127.0.0.1", m_Port)) {
        FORGE_ERROR("MCP server failed to bind 127.0.0.1:%d (port in use?)", m_Port);
        return;
    }

    m_Accepting = true;
    m_Running = true;
    m_Thread = std::thread([this]() { m_Http->listen_after_bind(); });
    FORGE_INFO("MCP server listening on http://127.0.0.1:%d/mcp", m_Port);
}

McpServer::~McpServer()
{
    m_Accepting = false; // in-flight handlers exit their poll loop within 50ms
    if (m_Running) {
        m_Http->stop();
        m_Running = false;
    }
    if (m_Thread.joinable())
        m_Thread.join();
    // Unprocessed queue entries die with us; their promises break, but every
    // waiting handler has already returned 503 via the m_Accepting gate.
}

void McpServer::ProcessMainThread()
{
    if (!m_Running)
        return;

    // Swap out under the lock, then work locally: handlers may take a while
    // and must not hold the queue mutex against the listener threads.
    std::vector<std::unique_ptr<Pending>> batch;
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        batch.swap(m_Queue);
    }
    for (std::unique_ptr<Pending>& p : batch)
        p->response.set_value(m_Protocol.HandleMessage(p->body));
}

} // namespace forge
