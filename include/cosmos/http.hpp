#pragma once

// Minimal HTTP/1.1 client for a workload driver talking to an in-process REST application over
// Cosmos virtual sockets. This is deliberately not a general HTTP stack: it sends one request per
// connection, reads one Content-Length-framed response (or EOF-framed when the server does not send
// Content-Length), and returns every protocol-level failure as an Error.
//
// The caller is responsible for opening the fd:
//   const int fd = connect_to_app();
//   const auto response = cosmos::http::put(fd, "/kv/key", value);
//   close(fd);
//
// send/recv must be interposed by the simulation binary for the request to travel through the
// virtual network (the usual -Wl,--wrap=send/--wrap=recv link flags). The descriptor must be
// blocking: EAGAIN is reported as SendFailed/RecvFailed rather than retried.

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <utility>
#include <vector>

namespace cosmos::http {

struct Header {
    std::string name;
    std::string value;
};

struct Request {
    std::string method = "GET";
    std::string target = "/";
    std::vector<Header> headers{};
    std::string body{};
};

struct Response {
    int status = 0;
    std::string reason{};
    std::vector<Header> headers{};
    std::string body{};
};

enum class Error : uint8_t {
    SendFailed,
    RecvFailed,
    MalformedRequest,
    MalformedStatus,
    MalformedHeaders,
    UnsupportedTransferEncoding,
    UnsupportedResponse,
    TooLarge,
    Truncated,
};

// The client is a workload tool, not a proxy: these bounds keep a buggy or hostile peer from
// turning a simulation into an unbounded allocation. They are deliberately generous.
inline constexpr std::size_t kMaxHeadBytes = 64 * 1024;
inline constexpr std::size_t kMaxBodyBytes = 64 * 1024 * 1024;
inline constexpr std::size_t kMaxHeaderCount = 256;
inline constexpr std::size_t kMaxInformationalResponses = 5;

namespace detail {

inline char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

inline bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (lower(a[i]) != lower(b[i])) return false;
    }
    return true;
}

inline std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// RFC 7230's tchar set. Used for the method and header names, which must not contain spaces,
// colons, or CRLF.
inline bool is_token_char(char c) {
    const unsigned char byte = static_cast<unsigned char>(c);
    if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
        (byte >= '0' && byte <= '9')) {
        return true;
    }
    switch (c) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

inline bool is_token(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!is_token_char(c)) return false;
    }
    return true;
}

// Request-targets are visible ASCII with no spaces or control characters. Non-ASCII bytes are
// rejected rather than passed through unescaped.
inline bool is_valid_target(std::string_view text) {
    if (text.empty()) return false;
    for (char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte <= 0x20 || byte >= 0x7f) return false;
    }
    return true;
}

inline bool is_valid_header_value(std::string_view text) {
    for (char c : text) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (c == '\t') continue;
        if (byte < 0x20 || byte == 0x7f) return false;
    }
    return true;
}

inline bool send_all(int fd, std::string_view bytes) {
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    // Cosmos targets Linux; without MSG_NOSIGNAL a closed peer can raise SIGPIPE.
    constexpr int kSendFlags = 0;
#endif
    // Virtual Net::send waits for room for the whole call, so a body larger than the 64 KiB
    // socket buffer would deadlock the universe if it were sent in one call. Chunk the write.
    constexpr std::size_t kChunk = 4096;
    while (!bytes.empty()) {
        const std::size_t chunk = std::min(bytes.size(), kChunk);
        const ssize_t sent = ::send(fd, bytes.data(), chunk, kSendFlags);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        bytes.remove_prefix(static_cast<std::size_t>(sent));
    }
    return true;
}

inline ssize_t recv_some(int fd, char* buffer, std::size_t count) {
    for (;;) {
        const ssize_t got = ::recv(fd, buffer, count, 0);
        if (got >= 0) return got;
        if (errno != EINTR) return -1;
    }
}

inline std::expected<std::string, Error> build_request(const Request& request) {
    if (!is_token(request.method) || !is_valid_target(request.target)) {
        return std::unexpected(Error::MalformedRequest);
    }

    bool has_host = false;
    bool has_length = false;
    bool has_connection = false;

    std::string wire;
    wire.reserve(request.method.size() + request.target.size() + request.body.size() + 128);
    wire += request.method;
    wire += ' ';
    wire += request.target;
    wire += " HTTP/1.1\r\n";
    for (const Header& header : request.headers) {
        if (!is_token(header.name) || !is_valid_header_value(header.value)) {
            return std::unexpected(Error::MalformedRequest);
        }
        if (iequals(header.name, "transfer-encoding")) {
            // This client does not encode chunked bodies, so a caller-supplied TE would describe
            // bytes the client is not actually sending.
            return std::unexpected(Error::UnsupportedTransferEncoding);
        }
        if (iequals(header.name, "content-length")) {
            if (has_length) return std::unexpected(Error::MalformedRequest);
            const std::string_view value = trim(header.value);
            uint64_t declared = 0;
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), declared);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                declared != request.body.size()) {
                return std::unexpected(Error::MalformedRequest);
            }
            has_length = true;
        }
        if (iequals(header.name, "host")) {
            if (has_host) return std::unexpected(Error::MalformedRequest);
            has_host = true;
        }
        if (iequals(header.name, "connection")) {
            // The client is one-request-per-connection. A caller-supplied keep-alive would make
            // an EOF-framed response wait forever for a close that never comes.
            if (!iequals(trim(header.value), "close")) {
                return std::unexpected(Error::MalformedRequest);
            }
            has_connection = true;
        }
        wire += header.name;
        wire += ": ";
        wire += header.value;
        wire += "\r\n";
    }
    if (!has_host) wire += "Host: cosmos.local\r\n";
    if (!has_length) {
        wire += "Content-Length: ";
        wire += std::to_string(request.body.size());
        wire += "\r\n";
    }
    if (!has_connection) wire += "Connection: close\r\n";
    wire += "\r\n";
    wire += request.body;
    return wire;
}

struct ParsedHead {
    int status = 0;
    std::string reason{};
    std::vector<Header> headers{};
    std::size_t content_length = 0;
    bool has_content_length = false;
    bool has_transfer_encoding = false;
    bool transfer_encoding_identity = false;
};

inline bool is_identity_coding(std::string_view value) {
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t comma = value.find(',', start);
        const std::string_view token = trim(value.substr(
            start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!iequals(token, "identity")) return false;
        if (comma == std::string_view::npos) return true;
        start = comma + 1;
    }
    return true;
}

inline std::expected<ParsedHead, Error> parse_head(std::string_view head) {
    const std::size_t status_end = head.find("\r\n");
    const std::string_view status_line = head.substr(0, status_end);
    const std::size_t first_space = status_line.find(' ');
    if (first_space == std::string_view::npos ||
        (status_line.substr(0, first_space) != "HTTP/1.0" &&
         status_line.substr(0, first_space) != "HTTP/1.1")) {
        return std::unexpected(Error::MalformedStatus);
    }
    const std::size_t second_space = status_line.find(' ', first_space + 1);
    const std::size_t code_end =
        second_space == std::string_view::npos ? status_line.size() : second_space;
    int status = 0;
    const char* code_begin = status_line.data() + first_space + 1;
    const char* code_finish = status_line.data() + code_end;
    if (code_finish - code_begin != 3) return std::unexpected(Error::MalformedStatus);
    const auto parsed = std::from_chars(code_begin, code_finish, status);
    if (parsed.ec != std::errc{} || parsed.ptr != code_finish || status < 100 || status > 599) {
        return std::unexpected(Error::MalformedStatus);
    }

    ParsedHead parsed_head;
    parsed_head.status = status;
    if (second_space != std::string_view::npos) {
        const std::string_view reason = status_line.substr(second_space + 1);
        if (!is_valid_header_value(reason)) return std::unexpected(Error::MalformedStatus);
        parsed_head.reason = std::string(reason);
    }

    bool saw_content_length = false;
    bool saw_transfer_encoding = false;
    bool transfer_encoding_identity = false;
    std::size_t header_count = 0;

    std::size_t position = status_end == std::string_view::npos ? head.size() : status_end + 2;
    while (position < head.size()) {
        if (++header_count > kMaxHeaderCount) return std::unexpected(Error::TooLarge);
        const std::size_t line_end = head.find("\r\n", position);
        const std::size_t end = line_end == std::string_view::npos ? head.size() : line_end;
        const std::string_view line = head.substr(position, end - position);
        if (!line.empty()) {
            const std::size_t colon = line.find(':');
            if (colon == std::string_view::npos) {
                return std::unexpected(Error::MalformedHeaders);
            }
            const std::string_view name = line.substr(0, colon);
            const std::string_view value = trim(line.substr(colon + 1));
            // No trim on the field name: whitespace before ':' and obs-fold lines are framing
            // ambiguities a strict peer rejects, not alternative spellings of the same header.
            if (!is_token(name) || !is_valid_header_value(value)) {
                return std::unexpected(Error::MalformedHeaders);
            }
            parsed_head.headers.push_back(Header{std::string(name), std::string(value)});

            if (iequals(name, "content-length")) {
                if (saw_content_length) return std::unexpected(Error::MalformedHeaders);
                if (value.size() > 1 && value.front() == '0') {
                    return std::unexpected(Error::MalformedHeaders);
                }
                uint64_t length = 0;
                const auto length_parse =
                    std::from_chars(value.data(), value.data() + value.size(), length);
                if (length_parse.ec != std::errc{} ||
                    length_parse.ptr != value.data() + value.size()) {
                    return std::unexpected(Error::MalformedHeaders);
                }
                if (length > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
                    return std::unexpected(Error::MalformedHeaders);
                }
                // The size cap is applied by exchange after the no-body statuses are known:
                // HEAD/204/205/304 legitimately carry a Content-Length with no body.
                parsed_head.content_length = static_cast<std::size_t>(length);
                parsed_head.has_content_length = true;
                saw_content_length = true;
            } else if (iequals(name, "transfer-encoding")) {
                if (saw_transfer_encoding) return std::unexpected(Error::MalformedHeaders);
                saw_transfer_encoding = true;
                transfer_encoding_identity = is_identity_coding(value);
                parsed_head.has_transfer_encoding = true;
                parsed_head.transfer_encoding_identity = transfer_encoding_identity;
            }
        }
        if (line_end == std::string_view::npos) break;
        position = line_end + 2;
    }

    return parsed_head;
}

} // namespace detail

namespace detail {

inline const Header* find_header(const std::vector<Header>& headers, std::string_view name) {
    for (const Header& header : headers) {
        if (iequals(header.name, name)) return &header;
    }
    return nullptr;
}

} // namespace detail

inline std::expected<Response, Error> exchange(int fd, const Request& request) {
    const auto wire = detail::build_request(request);
    if (!wire) return std::unexpected(wire.error());
    if (!detail::send_all(fd, *wire)) return std::unexpected(Error::SendFailed);

    std::string received;
    std::size_t informational = 0;
    for (;;) {
        std::size_t header_end = std::string::npos;
        while ((header_end = received.find("\r\n\r\n")) == std::string::npos) {
            if (received.size() > kMaxHeadBytes) {
                return std::unexpected(Error::TooLarge);
            }
            char buffer[4096];
            const ssize_t got = detail::recv_some(fd, buffer, sizeof(buffer));
            if (got < 0) return std::unexpected(Error::RecvFailed);
            if (got == 0) return std::unexpected(Error::Truncated);
            received.append(buffer, static_cast<std::size_t>(got));
        }
        if (header_end > kMaxHeadBytes) return std::unexpected(Error::TooLarge);

        auto parsed = detail::parse_head(std::string_view(received).substr(0, header_end));
        if (!parsed) return std::unexpected(parsed.error());

        const std::size_t body_begin = header_end + 4;
        if (parsed->status >= 100 && parsed->status < 200) {
            // Informational response: no body; the final response follows in the same message.
            if (parsed->status == 101) return std::unexpected(Error::UnsupportedResponse);
            if (++informational > kMaxInformationalResponses) {
                return std::unexpected(Error::TooLarge);
            }
            // Drop the interim response from the buffer so a long chain cannot accumulate.
            received.erase(0, body_begin);
            continue;
        }
        if (request.method == "CONNECT" && parsed->status >= 200 && parsed->status < 300) {
            // A successful CONNECT turns the connection into a tunnel; this client does not speak
            // the tunnelled protocol and would otherwise parse tunnel bytes as HTTP.
            return std::unexpected(Error::UnsupportedResponse);
        }

        Response response;
        response.status = parsed->status;
        response.reason = std::move(parsed->reason);
        response.headers = std::move(parsed->headers);

        // 205 is bodyless (RFC 7231 §6.3.6), as are HEAD/204/304.
        const bool no_body = request.method == "HEAD" || parsed->status == 204 ||
                             parsed->status == 205 || parsed->status == 304;
        if (no_body) {
            // A HEAD/304 may describe the transfer coding a GET would use; there is no body to
            // decode. Any coding is acceptable here because it is never applied.
            response.body.clear();
            return response;
        }
        if (parsed->has_transfer_encoding) {
            if (!parsed->transfer_encoding_identity) {
                return std::unexpected(Error::UnsupportedTransferEncoding);
            }
            if (parsed->has_content_length) {
                // TE and Content-Length together are the canonical ambiguous framing.
                return std::unexpected(Error::MalformedHeaders);
            }
        }

        // Drop the head before accumulating the body: the response then owns one buffer, not a
        // head+body string plus a second copy.
        received.erase(0, body_begin);

        if (parsed->has_content_length) {
            if (parsed->content_length > kMaxBodyBytes) {
                return std::unexpected(Error::TooLarge);
            }
            while (received.size() < parsed->content_length) {
                char buffer[4096];
                const ssize_t got = detail::recv_some(fd, buffer, sizeof(buffer));
                if (got < 0) return std::unexpected(Error::RecvFailed);
                if (got == 0) return std::unexpected(Error::Truncated);
                received.append(buffer, static_cast<std::size_t>(got));
            }
            received.resize(parsed->content_length);
            response.body = std::move(received);
            return response;
        }

        // No Content-Length: the response is framed by connection close. An explicit keep-alive
        // would wait forever for a close the peer does not intend to send.
        const Header* connection = detail::find_header(response.headers, "connection");
        if (connection != nullptr && !detail::iequals(detail::trim(connection->value), "close")) {
            return std::unexpected(Error::UnsupportedResponse);
        }
        if (received.size() > kMaxBodyBytes) return std::unexpected(Error::TooLarge);
        for (;;) {
            char buffer[4096];
            const ssize_t got = detail::recv_some(fd, buffer, sizeof(buffer));
            if (got < 0) return std::unexpected(Error::RecvFailed);
            if (got == 0) {
                response.body = std::move(received);
                return response;
            }
            received.append(buffer, static_cast<std::size_t>(got));
            if (received.size() > kMaxBodyBytes) {
                return std::unexpected(Error::TooLarge);
            }
        }
    }
}

inline const Header* find_header(const Response& response, std::string_view name) {
    return detail::find_header(response.headers, name);
}

inline std::string_view name_of(Error error) {
    switch (error) {
    case Error::SendFailed:
        return "send_failed";
    case Error::RecvFailed:
        return "recv_failed";
    case Error::MalformedRequest:
        return "malformed_request";
    case Error::MalformedStatus:
        return "malformed_status";
    case Error::MalformedHeaders:
        return "malformed_headers";
    case Error::UnsupportedTransferEncoding:
        return "unsupported_transfer_encoding";
    case Error::UnsupportedResponse:
        return "unsupported_response";
    case Error::TooLarge:
        return "too_large";
    case Error::Truncated:
        return "truncated";
    }
    return "?";
}

inline std::expected<Response, Error> get(int fd, std::string_view target) {
    Request request;
    request.method = "GET";
    request.target = std::string(target);
    return exchange(fd, request);
}

inline std::expected<Response, Error> post(int fd, std::string_view target, std::string body) {
    Request request;
    request.method = "POST";
    request.target = std::string(target);
    request.body = std::move(body);
    return exchange(fd, request);
}

inline std::expected<Response, Error> put(int fd, std::string_view target, std::string body) {
    Request request;
    request.method = "PUT";
    request.target = std::string(target);
    request.body = std::move(body);
    return exchange(fd, request);
}

} // namespace cosmos::http
