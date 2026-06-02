#ifndef JSONRPC_H
#define JSONRPC_H

/*
 * JSON-RPC 2.0 transport for the MCP server. Handles message framing on
 * stdin/stdout (both Content-Length headers and bare single-line JSON),
 * response/error serialization, and dispatch of the MCP protocol methods
 * (initialize, tools/list, tools/call, ping).
 *
 * IMPORTANT: stdout is the protocol channel. Nothing else must write to it;
 * diagnostics belong on stderr.
 */

/* Describes the server to the dispatcher. All pointers are borrowed and must
 * outlive the jsonrpc_run() call. */
typedef struct {
    const char *server_name;        /* serverInfo.name */
    const char *server_version;     /* serverInfo.version */
    const char *tools_list_result;  /* JSON object returned for tools/list */
    /* Invoked for tools/call. The handler owns the response and must call one
     * of the jsonrpc_send_* functions exactly once with the supplied id. */
    void (*on_tools_call)(const char *id, const char *json);
} jsonrpc_server_t;

/* Write a raw JSON body followed by a newline, then flush stdout. */
void send_body(const char *body);

/* Send {"jsonrpc":"2.0","id":id,"result":result}. result is raw JSON. */
void send_simple_result(const char *id, const char *result);

/* Send a JSON-RPC error object with the given code and message. */
void send_error(const char *id, int code, const char *message);

/* Send a tools/call success result wrapping text in a single text content
 * block. text is escaped before transmission. */
void send_tool_text_result(const char *id, const char *text);

/* Read and dispatch messages from stdin until EOF. */
void jsonrpc_run(const jsonrpc_server_t *server);

#endif /* JSONRPC_H */
