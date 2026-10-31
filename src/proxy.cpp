// The guest's web proxy (see proxy.h). Adapted from LightTouch's Portable/network/Network.cpp,
// which buffers whole responses; this one relays bytes as they arrive, so downloads stream and
// HTTPS connections stay open for as many requests as iOS sends.
#include "proxy.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <afunix.h>
#include <wincrypt.h>   // before OpenSSL, which undefines the names the two headers share

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <condition_variable>
#include <mutex>
#include <set>
#include <thread>

#pragma comment(lib, "crypt32.lib")

namespace {

using namespace std::chrono_literals;

constexpr size_t kMaxConnections = 128;   // Safari opens about six per website
constexpr DWORD kHandshakeMs = 30000;     // reading a request, connecting, TLS handshakes
constexpr int kIdleMs = 5 * 60 * 1000;    // a relayed connection with no traffic either way ends

// A request the proxy turns down, with the HTTP status iOS gets.
struct Refusal : std::runtime_error {
    int status;
    Refusal(int code, const std::string &message) : std::runtime_error(message), status(code) {}
};

std::string Lower(std::string text) {
    for (char &c : text)
        c = char(tolower(static_cast<unsigned char>(c)));
    return text;
}

// The latest OpenSSL error, as text.
std::string TlsError() {
    char text[256] = "unknown error";
    if (unsigned long code = ERR_get_error())
        ERR_error_string_n(code, text, sizeof text);
    ERR_clear_error();
    return text;
}

// ---- Sockets ---------------------------------------------------------------------------------

// A socket closed when it goes out of scope.
struct Socket {
    SOCKET s = INVALID_SOCKET;
    Socket() = default;
    explicit Socket(SOCKET socket) : s(socket) {}
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;
    ~Socket() {
        if (s != INVALID_SOCKET)
            closesocket(s);
    }
};

void SetBlocking(SOCKET s, bool blocking) {
    u_long nonBlocking = blocking ? 0 : 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
}

void SetTimeouts(SOCKET s, DWORD ms) {
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char *>(&ms), sizeof ms);
}

// Waits until `s` can be read, or written. False on timeout or error.
bool WaitFor(SOCKET s, bool write, int ms) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s, &set);
    timeval limit{ms / 1000, (ms % 1000) * 1000};
    return select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &limit) == 1;
}

// Sends all of `data` on a blocking socket.
bool SendText(SOCKET s, const std::string &data) {
    for (size_t sent = 0; sent < data.size();) {
        const int n = send(s, data.data() + sent, int(data.size() - sent), 0);
        if (n <= 0)
            return false;
        sent += size_t(n);
    }
    return true;
}

// A short plain-text answer for iOS; the connection ends after it.
void Answer(SOCKET s, int status, const std::string &text) {
    const char *reason = status == 400 ? "Bad Request" : status == 403 ? "Forbidden" : status == 503 ? "Service Unavailable"
                                                                                                     : "Bad Gateway";
    const std::string body = text + "\n";
    SendText(s, "HTTP/1.1 " + std::to_string(status) + " " + reason +
                    "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) +
                    "\r\nConnection: close\r\n\r\n" + body);
}

// Public internet addresses only: never this computer, never the local network (the rule the
// engine's NAT follows too, with lan=off). IPv4 inside IPv6 (mapped, compatible, NAT64) counts as IPv4.
bool PublicAddress(const sockaddr *address) {
    const unsigned char *b;
    if (address->sa_family == AF_INET) {
        b = reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in *>(address)->sin_addr);
    } else if (address->sa_family == AF_INET6) {
        const unsigned char *v6 =
            reinterpret_cast<const unsigned char *>(&reinterpret_cast<const sockaddr_in6 *>(address)->sin6_addr);
        static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        static const unsigned char zero[12] = {};
        static const unsigned char nat64[12] = {0, 0x64, 0xff, 0x9b};
        if (memcmp(v6, mapped, 12) && memcmp(v6, zero, 12) && memcmp(v6, nat64, 12))
            return (v6[0] & 0xfe) != 0xfc && !(v6[0] == 0xfe && (v6[1] & 0x80)) && v6[0] != 0xff;
        b = v6 + 12;
    } else {
        return false;
    }
    return b[0] && b[0] != 127 && b[0] != 10 && !(b[0] == 172 && (b[1] & 0xf0) == 16) && !(b[0] == 192 && b[1] == 168) &&
           !(b[0] == 169 && b[1] == 254) && !(b[0] == 100 && (b[1] & 0xc0) == 64) && !(b[0] == 198 && (b[1] & 0xfe) == 18) &&
           b[0] < 224;
}

// Opens a TCP connection to a website, giving each of its addresses 10 seconds.
std::unique_ptr<Socket> ConnectTo(const std::string &host, const std::string &port) {
    const std::string name = Lower(host);
    if (name == "localhost" || name.ends_with(".localhost"))
        throw Refusal(403, "The web proxy doesn't reach this computer");
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &found) != 0)
        throw Refusal(502, "Couldn't find " + host);
    std::unique_ptr<addrinfo, decltype(&freeaddrinfo)> addresses(found, freeaddrinfo);
    for (addrinfo *a = found; a; a = a->ai_next)
        if (!PublicAddress(a->ai_addr))
            throw Refusal(403, host + " is on this computer or your local network");
    for (addrinfo *a = found; a; a = a->ai_next) {
        auto s = std::make_unique<Socket>(socket(a->ai_family, a->ai_socktype, a->ai_protocol));
        if (s->s == INVALID_SOCKET)
            continue;
        SetBlocking(s->s, false);
        bool connected = connect(s->s, a->ai_addr, int(a->ai_addrlen)) == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            // Windows reports a failed connect in the exception set, a finished one as writable.
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s->s, &writable);
            FD_SET(s->s, &failed);
            timeval limit{10, 0};
            connected = select(0, nullptr, &writable, &failed, &limit) == 1 && FD_ISSET(s->s, &writable);
        }
        if (connected) {
            SetBlocking(s->s, true);
            SetTimeouts(s->s, kHandshakeMs);
            return s;
        }
    }
    throw Refusal(502, "Couldn't connect to " + host);
}

// ---- Relaying --------------------------------------------------------------------------------

// One end of a relayed connection: its socket, and its TLS session when the proxy speaks TLS there.
struct Side {
    SOCKET socket;
    SSL *tls = nullptr;
};

// Reads what has arrived: the byte count, 0 when nothing is ready yet, -1 once the side closed.
int ReadSome(Side &side, char *buffer, int size) {
    if (!side.tls) {
        const int n = recv(side.socket, buffer, size, 0);
        if (n > 0)
            return n;
        return n == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK ? 0 : -1;
    }
    const int n = SSL_read(side.tls, buffer, size);
    if (n > 0)
        return n;
    const int error = SSL_get_error(side.tls, n);
    return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE ? 0 : -1;
}

// Writes all of `data` to a non-blocking side, waiting while its socket is full.
bool WriteAllTo(Side &side, const char *data, int size) {
    while (size > 0) {
        bool waitToWrite = true;
        int n;
        if (side.tls) {
            n = SSL_write(side.tls, data, size);
            if (n <= 0) {
                const int error = SSL_get_error(side.tls, n);
                if (error != SSL_ERROR_WANT_WRITE && error != SSL_ERROR_WANT_READ)
                    return false;
                waitToWrite = error == SSL_ERROR_WANT_WRITE;
            }
        } else {
            n = send(side.socket, data, size, 0);
            if (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
        }
        if (n > 0) {
            data += n;
            size -= n;
        } else if (!WaitFor(side.socket, waitToWrite, kIdleMs)) {
            return false;
        }
    }
    return true;
}

// Copies bytes both ways until either side closes, or nothing moves for kIdleMs. The sockets are
// non-blocking so that a TLS record that carries no data (a session ticket) can never stall it.
void Relay(Side &a, Side &b) {
    SetBlocking(a.socket, false);
    SetBlocking(b.socket, false);
    char buffer[16384];
    for (;;) {
        bool moved = false;
        for (auto [from, to] : {std::pair{&a, &b}, std::pair{&b, &a}}) {
            for (;;) {   // everything readable now, TLS-buffered bytes included
                const int n = ReadSome(*from, buffer, int(sizeof buffer));
                if (n < 0)
                    return;
                if (n == 0)
                    break;
                if (!WriteAllTo(*to, buffer, n))
                    return;
                moved = true;
            }
        }
        if (moved)
            continue;
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(a.socket, &readable);
        FD_SET(b.socket, &readable);
        timeval limit{kIdleMs / 1000, 0};
        if (select(0, &readable, nullptr, nullptr, &limit) <= 0)
            return;
    }
}

// ---- Certificates ----------------------------------------------------------------------------

using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using Cert = std::unique_ptr<X509, decltype(&X509_free)>;
using Tls = std::unique_ptr<SSL, decltype(&SSL_free)>;

void Require(bool ok, const char *what) {
    if (!ok)
        throw std::runtime_error(std::string("Web proxy: ") + what + " failed (" + TlsError() + ")");
}

Key NewKey() {
    Key key(EVP_RSA_gen(2048), EVP_PKEY_free);
    Require(bool(key), "making a key");
    return key;
}

bool NumericHost(const std::string &host) {
    unsigned char address[16];
    return inet_pton(AF_INET, host.c_str(), address) == 1 || inet_pton(AF_INET6, host.c_str(), address) == 1;
}

void AddExtension(X509 *certificate, X509 *issuer, int nid, const std::string &value) {
    X509V3_CTX context{};
    X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
    X509_EXTENSION *extension = X509V3_EXT_conf_nid(nullptr, &context, nid, value.c_str());
    Require(extension && X509_add_ext(certificate, extension, -1) == 1, "encoding a certificate");
    X509_EXTENSION_free(extension);
}

// The CA (no issuer), or a website's certificate signed by it. These are only ever shown to iOS,
// so they suit it: SHA-1 signatures, and a start in 2000 for guests that keep a historical clock.
Cert MakeCertificate(EVP_PKEY *key, EVP_PKEY *issuerKey, X509 *issuer, const std::string &name) {
    Cert certificate(X509_new(), X509_free);
    Require(bool(certificate), "making a certificate");
    X509_set_version(certificate.get(), 2);
    unsigned char serial[16];
    Require(RAND_bytes(serial, sizeof serial) == 1, "making a serial number");
    serial[0] &= 0x7f;
    BIGNUM *number = BN_bin2bn(serial, sizeof serial, nullptr);
    ASN1_INTEGER *integer = BN_to_ASN1_INTEGER(number, nullptr);
    X509_set_serialNumber(certificate.get(), integer);
    ASN1_INTEGER_free(integer);
    BN_free(number);
    X509_set_pubkey(certificate.get(), key);
    ASN1_TIME_set_string(X509_getm_notBefore(certificate.get()), "20000101000000Z");
    X509_gmtime_adj(X509_getm_notAfter(certificate.get()), issuer ? 30L * 86400 : 3650L * 86400);
    std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)> subject(X509_NAME_new(), X509_NAME_free);
    Require(subject && X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_UTF8,
                                                  reinterpret_cast<const unsigned char *>(name.c_str()), -1, -1, 0) == 1,
            "naming a certificate");
    X509_set_subject_name(certificate.get(), subject.get());
    X509_set_issuer_name(certificate.get(), issuer ? X509_get_subject_name(issuer) : subject.get());
    X509 *signer = issuer ? issuer : certificate.get();
    AddExtension(certificate.get(), signer, NID_basic_constraints, issuer ? "critical,CA:FALSE" : "critical,CA:TRUE,pathlen:0");
    AddExtension(certificate.get(), signer, NID_key_usage,
                 issuer ? "critical,digitalSignature,keyEncipherment" : "critical,keyCertSign,cRLSign");
    if (issuer) {
        AddExtension(certificate.get(), signer, NID_subject_alt_name, (NumericHost(name) ? "IP:" : "DNS:") + name);
        AddExtension(certificate.get(), signer, NID_ext_key_usage, "serverAuth");
    }
    Require(X509_sign(certificate.get(), issuerKey, EVP_sha1()) > 0, "signing a certificate");
    return certificate;
}

// Checks a website's certificate chain the way Windows checks one for a browser: a trusted root,
// valid dates, and the host name. Windows also fetches roots it doesn't hold yet.
bool TrustedByWindows(SSL *tls, const std::string &host) {
    STACK_OF(X509) *chain = SSL_get_peer_cert_chain(tls);   // the website's certificate first
    if (!chain || sk_X509_num(chain) == 0)
        return false;
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
    if (!store)
        return false;
    PCCERT_CONTEXT leaf = nullptr;
    for (int i = 0; i < sk_X509_num(chain); i++) {
        unsigned char *der = nullptr;
        const int size = i2d_X509(sk_X509_value(chain, i), &der);
        if (size > 0)
            CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der, DWORD(size), CERT_STORE_ADD_ALWAYS,
                                             i == 0 ? &leaf : nullptr);
        OPENSSL_free(der);
    }
    bool trusted = false;
    if (leaf) {
        LPSTR usage[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_CHAIN_PARA request{sizeof request};
        request.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        request.RequestedUsage.Usage.cUsageIdentifier = 1;
        request.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
        PCCERT_CHAIN_CONTEXT built = nullptr;
        if (CertGetCertificateChain(nullptr, leaf, nullptr, store, &request, 0, nullptr, &built)) {
            std::wstring serverName = Widen(host);
            SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
            ssl.cbSize = sizeof ssl;
            ssl.dwAuthType = AUTHTYPE_SERVER;
            ssl.pwszServerName = serverName.data();
            CERT_CHAIN_POLICY_PARA policy{sizeof policy};
            policy.pvExtraPolicyPara = &ssl;
            CERT_CHAIN_POLICY_STATUS status{sizeof status};
            trusted = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, built, &policy, &status) && status.dwError == 0;
            CertFreeCertificateChain(built);
        }
        CertFreeCertificateContext(leaf);
    }
    CertCloseStore(store, 0);
    return trusted;
}

// ---- Requests --------------------------------------------------------------------------------

// The start of a request iOS sent the proxy: "CONNECT host:443 HTTP/1.1", or a plain HTTP request
// with an absolute URL ("GET http://host/path HTTP/1.1"), and its header lines.
struct Request {
    std::string method, target, version;
    std::vector<std::string> headers;   // "Name: value"
    std::string rest;                   // bytes that came after the headers (a request body)
};

Request ReadRequest(SOCKET s) {
    std::string data;
    char buffer[4096];
    size_t end;
    while ((end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > 65536)
            throw Refusal(400, "The request's headers are too long");
        const int n = recv(s, buffer, sizeof buffer, 0);
        if (n <= 0)
            throw std::runtime_error("iOS closed the connection before sending a request");
        data.append(buffer, size_t(n));
    }
    Request request;
    request.rest = data.substr(end + 4);
    data.resize(end);
    size_t lineEnd = data.find("\r\n");
    const std::string first = data.substr(0, lineEnd);
    const size_t space1 = first.find(' '), space2 = first.rfind(' ');
    if (space1 == std::string::npos || space2 == space1)
        throw Refusal(400, "That isn't an HTTP request");
    request.method = first.substr(0, space1);
    request.target = first.substr(space1 + 1, space2 - space1 - 1);
    request.version = first.substr(space2 + 1);
    while (lineEnd != std::string::npos) {
        const size_t start = lineEnd + 2;
        lineEnd = data.find("\r\n", start);
        request.headers.push_back(data.substr(start, lineEnd == std::string::npos ? std::string::npos : lineEnd - start));
    }
    return request;
}

// Splits "host:port" or "[v6 address]:port".
std::pair<std::string, std::string> SplitHostPort(const std::string &authority, const std::string &defaultPort) {
    std::string host = authority, port = defaultPort;
    if (!host.empty() && host[0] == '[') {
        const size_t close = host.find(']');
        if (close == std::string::npos)
            throw Refusal(400, "Invalid address " + authority);
        if (close + 1 < host.size() && host[close + 1] == ':')
            port = host.substr(close + 2);
        host = host.substr(1, close - 1);
    } else if (const size_t colon = host.rfind(':'); colon != std::string::npos) {
        port = host.substr(colon + 1);
        host.resize(colon);
    }
    if (host.empty() || port.empty() || port.find_first_not_of("0123456789") != std::string::npos)
        throw Refusal(400, "Invalid address " + authority);
    return {host, port};
}

}  // namespace

// ---- The proxy -------------------------------------------------------------------------------

struct WebProxy::State {
    std::atomic<Mode> mode{Mode::Off};
    Key caKey{nullptr, EVP_PKEY_free}, leafKey{nullptr, EVP_PKEY_free};
    Cert ca{nullptr, X509_free};
    std::string der;                   // the CA certificate, DER
    SSL_CTX *client = nullptr;         // modern TLS, to websites
    SSL_CTX *server = nullptr;         // old TLS, to iOS

    std::mutex leavesMutex;            // websites' certificates, made once per host
    std::map<std::string, Cert> leaves;

    SOCKET listener = INVALID_SOCKET;
    fs::path socketPath;
    std::thread acceptor;
    std::mutex activeMutex;            // the connections being served
    std::condition_variable idle;
    std::set<SOCKET> active;
    bool stopping = false;

    ~State() {
        SSL_CTX_free(client);
        SSL_CTX_free(server);
    }

    // Puts the website's certificate (made on first use) on iOS's side of a bridged connection.
    bool UseLeaf(SSL *tls, const std::string &host) {
        std::lock_guard<std::mutex> lock(leavesMutex);
        auto found = leaves.find(host);
        if (found == leaves.end()) {
            if (leaves.size() >= 1024)
                leaves.clear();
            found = leaves.emplace(host, MakeCertificate(leafKey.get(), caKey.get(), ca.get(), host)).first;
        }
        return SSL_use_certificate(tls, found->second.get()) == 1 && SSL_use_PrivateKey(tls, leafKey.get()) == 1;
    }

    void Serve(SOCKET guest);
    void Tunnel(SOCKET guest, const Request &request, Mode mode);
    void Forward(SOCKET guest, const Request &request);
};

void WebProxy::State::Serve(SOCKET guest) {
    Request request = ReadRequest(guest);
    try {
        const Mode now = mode.load();
        if (now == Mode::Off)
            throw Refusal(503, "The internet is off for this device. Turn it on in iOS Emulator.");
        if (request.method == "CONNECT")
            Tunnel(guest, request, now);
        else if (Lower(request.target).starts_with("http://"))
            Forward(guest, request);
        else
            throw Refusal(400, "The web proxy takes CONNECT and http:// requests");
    } catch (const Refusal &refusal) {
        Log("Web proxy: %s %s: %s", request.method.c_str(), request.target.c_str(), refusal.what());
        Answer(guest, refusal.status, refusal.what());
    }
}

// HTTPS (and anything else iOS tunnels): "CONNECT host:port".
void WebProxy::State::Tunnel(SOCKET guest, const Request &request, Mode now) {
    const auto [host, port] = SplitHostPort(request.target, "443");
    auto origin = ConnectTo(host, port);
    const std::string established = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (now != Mode::Bridge || port != "443") {   // pass the bytes through untouched
        if (!SendText(guest, established))
            return;
        Side a{guest}, b{origin->s};
        if (!request.rest.empty() && !SendText(origin->s, request.rest))
            return;
        Relay(a, b);
        return;
    }

    // The bridge: modern TLS to the website first, so one Windows doesn't trust is refused before
    // iOS starts its own handshake.
    Tls website(SSL_new(client), SSL_free);
    Require(bool(website), "starting TLS");
    SSL_set_fd(website.get(), int(origin->s));
    if (!NumericHost(host))
        SSL_set_tlsext_host_name(website.get(), host.c_str());
    if (SSL_connect(website.get()) != 1)
        throw Refusal(502, "Couldn't open a secure connection to " + host + " (" + TlsError() + ")");
    if (!TrustedByWindows(website.get(), host))
        throw Refusal(502, host + "'s certificate isn't trusted by Windows");
    if (!SendText(guest, established))
        return;
    Tls ios(SSL_new(server), SSL_free);
    Require(bool(ios), "starting TLS");
    if (!UseLeaf(ios.get(), host))
        throw std::runtime_error("Web proxy: couldn't use the certificate for " + host);
    SSL_set_fd(ios.get(), int(guest));
    if (SSL_accept(ios.get()) != 1) {
        Log("Web proxy: iOS's TLS handshake for %s failed (%s). Is the proxy's certificate installed?", host.c_str(),
            TlsError().c_str());
        return;
    }
    Side a{guest, ios.get()}, b{origin->s, website.get()};
    Relay(a, b);
}

// Plain HTTP: "GET http://host/path HTTP/1.1". The request goes on to the website in its usual
// form, then the bytes are relayed. One request per connection keeps the proxy simple.
void WebProxy::State::Forward(SOCKET guest, const Request &request) {
    const std::string url = request.target.substr(7);   // after "http://"
    const size_t slash = url.find('/');
    const std::string authority = url.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : url.substr(slash);
    const auto [host, port] = SplitHostPort(authority, "80");
    auto origin = ConnectTo(host, port);

    std::string forwarded = request.method + " " + path + " " + request.version + "\r\n";
    bool hasHost = false;
    for (const auto &line : request.headers) {
        const std::string name = Lower(line.substr(0, line.find(':')));
        if (name == "connection" || name == "proxy-connection" || name == "keep-alive")
            continue;
        hasHost |= name == "host";
        forwarded += line + "\r\n";
    }
    if (!hasHost)
        forwarded += "Host: " + authority + "\r\n";
    forwarded += "Connection: close\r\n\r\n" + request.rest;
    if (!SendText(origin->s, forwarded))
        throw Refusal(502, host + " closed the connection");
    Side a{guest}, b{origin->s};
    Relay(a, b);
}

WebProxy::WebProxy(const fs::path &identityDir) : state_(std::make_shared<State>()) {
    State &s = *state_;
    // The identity: the CA's key, then its certificate, in one PEM file (LightTouch's layout).
    // It goes through memory: these OpenSSL DLLs use a different C runtime, so no FILE* may cross.
    fs::create_directories(identityDir);
    const fs::path pem = identityDir / "proxy-ca.pem";
    if (fs::exists(pem)) {
        const std::string text = LoadFile(pem);
        std::unique_ptr<BIO, decltype(&BIO_free)> in(BIO_new_mem_buf(text.data(), int(text.size())), BIO_free);
        s.caKey.reset(PEM_read_bio_PrivateKey(in.get(), nullptr, nullptr, nullptr));
        s.ca.reset(PEM_read_bio_X509(in.get(), nullptr, nullptr, nullptr));
        Require(s.caKey && s.ca && X509_check_private_key(s.ca.get(), s.caKey.get()) == 1,
                ("reading " + PathText(pem)).c_str());
    } else {
        s.caKey = NewKey();
        s.ca = MakeCertificate(s.caKey.get(), s.caKey.get(), nullptr, "iOS Emulator Web Proxy");
        std::unique_ptr<BIO, decltype(&BIO_free)> out(BIO_new(BIO_s_mem()), BIO_free);
        Require(PEM_write_bio_PrivateKey(out.get(), s.caKey.get(), nullptr, nullptr, 0, nullptr, nullptr) == 1 &&
                    PEM_write_bio_X509(out.get(), s.ca.get()) == 1,
                "saving the proxy's identity");
        char *bytes = nullptr;
        const long size = BIO_get_mem_data(out.get(), &bytes);
        SaveFile(pem, std::string(bytes, size_t(size)));
    }
    unsigned char *der = nullptr;
    const int size = i2d_X509(s.ca.get(), &der);
    Require(size > 0, "encoding the proxy's certificate");
    s.der.assign(reinterpret_cast<char *>(der), size_t(size));
    OPENSSL_free(der);
    s.leafKey = NewKey();   // one key for every website's certificate this session

    // Websites: TLS 1.2 or newer. Their certificates are checked by TrustedByWindows.
    s.client = SSL_CTX_new(TLS_client_method());
    Require(s.client != nullptr, "setting up TLS");
    SSL_CTX_set_min_proto_version(s.client, TLS1_2_VERSION);
    // iOS: whatever its old TLS speaks (TLS 1.0 and SHA-1 included). Only the proxy hears it.
    s.server = SSL_CTX_new(TLS_server_method());
    Require(s.server != nullptr, "setting up TLS");
    SSL_CTX_set_security_level(s.server, 0);
    SSL_CTX_set_min_proto_version(s.server, TLS1_VERSION);
    SSL_CTX_set_cipher_list(s.server, "DEFAULT:@SECLEVEL=0");
}

WebProxy::~WebProxy() {
    State &s = *state_;
    {
        std::lock_guard<std::mutex> lock(s.activeMutex);
        s.stopping = true;
        for (SOCKET guest : s.active)
            shutdown(guest, SD_BOTH);   // ends their relays
    }
    if (s.listener != INVALID_SOCKET)
        closesocket(s.listener);   // ends the accept loop
    if (s.acceptor.joinable())
        s.acceptor.join();
    std::unique_lock<std::mutex> lock(s.activeMutex);
    s.idle.wait_for(lock, 5s, [&] { return s.active.empty(); });   // threads still out keep the state alive
    std::error_code ignored;
    fs::remove(s.socketPath, ignored);
}

void WebProxy::Listen(const fs::path &socketPath) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    const std::string path = PathText(socketPath);
    sockaddr_un address{};
    if (path.size() >= sizeof address.sun_path)
        throw std::runtime_error("Web proxy: the socket path is too long: " + path);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.c_str(), path.size() + 1);
    std::error_code ignored;
    fs::remove(socketPath, ignored);   // a stale socket file blocks bind
    SOCKET s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET || bind(s, reinterpret_cast<sockaddr *>(&address), sizeof address) != 0 || listen(s, 32) != 0) {
        const int error = WSAGetLastError();
        if (s != INVALID_SOCKET)
            closesocket(s);
        throw std::runtime_error("Web proxy: couldn't listen on " + path + " (error " + std::to_string(error) + ")");
    }
    state_->listener = s;
    state_->socketPath = socketPath;
    state_->acceptor = std::thread([state = state_, s] {
        for (;;) {
            SOCKET guest = accept(s, nullptr, nullptr);
            if (guest == INVALID_SOCKET)
                return;   // the listener closed
            {
                std::lock_guard<std::mutex> lock(state->activeMutex);
                if (state->stopping || state->active.size() >= kMaxConnections) {
                    closesocket(guest);
                    continue;
                }
                state->active.insert(guest);
            }
            std::thread([state, guest] {
                SetTimeouts(guest, kHandshakeMs);
                try {
                    state->Serve(guest);
                } catch (const std::exception &e) {
                    Log("Web proxy: %s", e.what());
                }
                {
                    std::lock_guard<std::mutex> lock(state->activeMutex);
                    state->active.erase(guest);
                }
                closesocket(guest);
                state->idle.notify_all();
            }).detach();
        }
    });
}

void WebProxy::SetMode(Mode mode) { state_->mode = mode; }

std::string WebProxy::Certificate() const { return state_->der; }
