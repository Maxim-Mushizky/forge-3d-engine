#pragma once

#include "McpProtocol.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <future>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
} // namespace httplib

namespace forge {

// Embedded MCP Streamable-HTTP endpoint. RAII: construction binds
// 127.0.0.1:<port> and starts the listener thread, destruction stops it.
// Threading follows the Epic/UE-5.8 pattern: HTTP worker threads only enqueue
// raw JSON-RPC messages and block on a response future; the GL main thread
// drains the queue serially once per frame (ProcessMainThread), so tool
// handlers get full Scene/Renderer access with no overlapping calls (#75).
class McpServer {
public:
    McpServer(McpProtocol& protocol, int port);
    ~McpServer();

    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    bool Running() const { return m_Running; }
    int Port() const { return m_Port; }

    // Call once per frame on the GL main thread.
    void ProcessMainThread();

private:
    struct Pending {
        std::string body;                    // raw JSON-RPC message
        std::promise<std::string> response;  // empty string = notification (202)
    };

    McpProtocol& m_Protocol; // non-owning; EditorApp owns both and this first
    int m_Port = 0;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Accepting{false}; // gate: false during shutdown

    std::unique_ptr<httplib::Server> m_Http;
    std::thread m_Thread;

    std::mutex m_QueueMutex;
    std::vector<std::unique_ptr<Pending>> m_Queue;
};

} // namespace forge
