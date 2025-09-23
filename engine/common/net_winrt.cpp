#include "quakedef.h"
#include "netinc.h"

#ifdef WINRT

#include <windows.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.Sockets.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Networking;
using namespace Windows::Networking::Connectivity;
using namespace Windows::Networking::Sockets;
using namespace Windows::Storage::Streams;

netadr_t        net_local_cl_ipadr;
struct ftenet_generic_connection_s *net_from_connection = nullptr;
netadr_t        net_from;
sizebuf_t       net_message = {0};
FTE_ALIGN(4) qbyte net_message_buffer[MAX_OVERALLMSGLEN];

cvar_t net_enabled = CVARD("net_enabled", "1", "Enables the WinRT networking backend.");
cvar_t timeout = CVARD("timeout", "65", "Connections will time out if no packets are received for this duration of time.");
cvar_t net_fakeloss = CVARFD("net_fakeloss", "0", CVAR_CHEAT, "Simulates packetloss in both receiving and sending, on a scale from 0 to 1.");
cvar_t net_fakemtu = CVARFD("net_fakemtu", "0", CVAR_CHEAT, "Cripples packet reception sizes.");
cvar_t net_ice_broker = CVARFD("net_ice_broker", "", CVAR_NOTFROMSERVER, "ICE broker unavailable in WinRT builds.");
cvar_t net_hybriddualstack = CVARD("net_hybriddualstack", "0", "Hybrid sockets are unavailable in WinRT builds.");
static cvar_t net_dns_ipv4 = CVARD("net_dns_ipv4", "1", "Enable IPv4 DNS resolution.");
static cvar_t net_dns_ipv6 = CVARD("net_dns_ipv6", "1", "Enable IPv6 DNS resolution.");

#if defined(HAVE_SSL)
cvar_t tls_ignorecertificateerrors = CVARFD("tls_ignorecertificateerrors", "0", CVAR_NOTFROMSERVER|CVAR_NOSAVE|CVAR_NOUNSAFEEXPAND|CVAR_NOSET, "TLS certificate errors cannot be ignored on WinRT builds.");
#endif

#if defined(TCPCONNECT) && (defined(HAVE_SERVER) || defined(HAVE_HTTPSV))
#ifdef HAVE_SERVER
cvar_t net_enable_qizmo = CVARD("net_enable_qizmo", "0", "WinRT builds do not expose TCP listener support.");
#endif
#ifdef MVD_RECORDING
cvar_t net_enable_qtv = CVARD("net_enable_qtv", "0", "WinRT builds do not expose TCP listener support.");
#endif
#if defined(HAVE_SSL)
cvar_t net_enable_tls = CVARD("net_enable_tls", "0", "TLS listeners are unavailable in WinRT builds.");
#endif
#ifdef HAVE_HTTPSV
cvar_t net_enable_http = CVARD("net_enable_http", "0", "HTTP listeners are unavailable in WinRT builds.");
cvar_t net_enable_rtcbroker = CVARD("net_enable_rtcbroker", "0", "WebRTC broker listeners are unavailable in WinRT builds.");
cvar_t net_enable_websockets = CVARD("net_enable_websockets", "0", "WebSocket listeners are unavailable in WinRT builds.");
#endif
#endif

#if defined(HAVE_DTLS)
cvar_t net_enable_dtls = CVARAFCD("net_enable_dtls", "0", "sv_listen_dtls", 0, NULL, "DTLS is unavailable on WinRT builds.");
cvar_t dtls_psk_hint = CVARFD("dtls_psk_hint", "", CVAR_NOUNSAFEEXPAND, "");
cvar_t dtls_psk_user = CVARFD("dtls_psk_user", "", CVAR_NOUNSAFEEXPAND, "");
cvar_t dtls_psk_key = CVARFD("dtls_psk_key", "", CVAR_NOUNSAFEEXPAND, "");
#endif

#ifdef HAVE_CLIENT
static void QDECL cl_delay_packets_Announce(cvar_t *var, char *oldval) { (void)var; (void)oldval; }
static cvar_t cl_delay_packets = CVARCD("cl_delay_packets", "0", cl_delay_packets_Announce, "Extra latency unsupported on WinRT builds.");
#endif

cvar_t net_ice_servers = CVAR("net_ice_servers", "");
cvar_t net_ice_relayonly = CVAR("net_ice_relayonly", "0");

static std::wstring WinRT_FromUtf8(const char *text)
{
        if (!text || !*text)
                return std::wstring();
        int count = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
        if (count <= 0)
                return std::wstring();
        std::wstring wide(static_cast<size_t>(count - 1), L'\0');
        if (!wide.empty())
                MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), count);
        return wide;
}

static std::wstring WinRT_FromUtf8(const std::string &text)
{
        return WinRT_FromUtf8(text.c_str());
}

static std::string WinRT_ToUtf8(const std::wstring &wide)
{
        if (wide.empty())
                return std::string();
        int count = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0)
                return std::string();
        std::string utf8(static_cast<size_t>(count), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), count, nullptr, nullptr);
        return utf8;
}

static bool WinRT_ParseIPv4String(std::wstring_view text, qbyte (&out)[4])
{
        int part = 0;
        unsigned int value = 0;
        bool digit = false;
        for (wchar_t ch : text)
        {
                if (ch >= L'0' && ch <= L'9')
                {
                        value = value * 10 + static_cast<unsigned int>(ch - L'0');
                        if (value > 255)
                                return false;
                        digit = true;
                }
                else if (ch == L'.')
                {
                        if (!digit || part >= 3)
                                return false;
                        out[part++] = static_cast<qbyte>(value);
                        value = 0;
                        digit = false;
                }
                else
                        return false;
        }
        if (part != 3 || !digit)
                return false;
        out[3] = static_cast<qbyte>(value);
        return true;
}

static std::wstring WinRT_FormatIPv4(const qbyte (&ip)[4])
{
        wchar_t buffer[16];
        swprintf(buffer, 16, L"%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
        return buffer;
}

static bool WinRT_ParseIPv6String(std::wstring_view text, qbyte (&out)[16], unsigned int &scopeId)
{
        scopeId = 0;
        if (text.empty())
                return false;
        std::wstring address(text);
        size_t percent = address.find(L'%');
        if (percent != std::wstring::npos)
        {
                std::wstring scope = address.substr(percent + 1);
                address.resize(percent);
                if (!scope.empty())
                {
                        wchar_t *endptr = nullptr;
                        unsigned long parsed = wcstoul(scope.c_str(), &endptr, 10);
                        if (endptr && *endptr == L'\0')
                                scopeId = static_cast<unsigned int>(parsed);
                }
        }
        IN6_ADDR addr6{};
        if (InetPtonW(AF_INET6, address.c_str(), &addr6) != 1)
                return false;
        memcpy(out, &addr6, sizeof(out));
        return true;
}

static std::wstring WinRT_FormatIPv6(const qbyte (&ip)[16], unsigned int scopeId)
{
        wchar_t buffer[65];
        wchar_t *result = InetNtopW(AF_INET6, const_cast<qbyte *>(ip), buffer, sizeof(buffer) / sizeof(buffer[0]));
        if (!result)
                return std::wstring();
        std::wstring formatted(result);
        if (scopeId)
        {
                formatted.push_back(L'%');
                formatted.append(std::to_wstring(scopeId));
        }
        return formatted;
}

static std::wstring WinRT_ServiceFromPort(unsigned short port)
{
        wchar_t buffer[6];
        swprintf(buffer, 6, L"%u", static_cast<unsigned int>(port));
        return buffer;
}

static unsigned short WinRT_ParseService(hstring const &service)
{
        try
        {
                std::wstring text = service.c_str();
                if (text.empty())
                        return 0;
                unsigned long value = std::stoul(text);
                if (value > 65535)
                        value = 0;
                return static_cast<unsigned short>(value);
        }
        catch (...)
        {
                return 0;
        }
}

static bool WinRT_ParseEndpoint(const HostName &host, hstring const &service, netadr_t &adr)
{
        if (!host)
                return false;
        std::wstring raw = host.RawName().c_str();
        netadr_t out{};
        unsigned short port = WinRT_ParseService(service);
        out.port = BigShort(port);
        out.prot = NP_DGRAM;
        unsigned int scope = 0;
        if (WinRT_ParseIPv4String(raw, out.address.ip))
        {
                out.type = NA_IP;
                adr = out;
                return true;
        }
        if (WinRT_ParseIPv6String(raw, out.address.ip6, scope))
        {
                out.type = NA_IPV6;
                out.scopeid = scope;
                adr = out;
                return true;
        }
        return false;
}

static bool WinRT_FormatAddress(const netadr_t &adr, std::wstring &host, std::wstring &service)
{
        unsigned short port = BigShort(adr.port);
        if (adr.type == NA_LOOPBACK)
        {
                host = L"127.0.0.1";
                service = WinRT_ServiceFromPort(port);
                return true;
        }
        if (adr.type == NA_IP)
        {
                host = WinRT_FormatIPv4(adr.address.ip);
                service = WinRT_ServiceFromPort(port);
                return true;
        }
        if (adr.type == NA_IPV6)
        {
                host = WinRT_FormatIPv6(adr.address.ip6, adr.scopeid);
                service = WinRT_ServiceFromPort(port);
                return true;
        }
        return false;
}

static unsigned short WinRT_StringToPort(const char *address, std::wstring &hostOut)
{
        hostOut.clear();
        if (!address || !*address)
                return 0;
        std::string input(address);
        std::string hostPart;
        int port = 0;
        WinRT_SplitHostPort(input, hostPart, port, 0);
        if (!hostPart.empty())
                hostOut = WinRT_FromUtf8(hostPart);
        if (port <= 0 || port > 65535)
                port = 0;
        return static_cast<unsigned short>(port);
}

static void WinRT_SplitHostPort(const std::string &input, std::string &host, int &port, int defaultPort)
{
        host.clear();
        port = defaultPort;
        if (input.empty())
                return;
        if (input.front() == '[')
        {
                size_t close = input.find(']');
                if (close != std::string::npos)
                {
                        host = input.substr(1, close - 1);
                        size_t colon = input.find(':', close + 1);
                        if (colon != std::string::npos)
                        {
                                int parsed = atoi(input.substr(colon + 1).c_str());
                                if (parsed > 0 && parsed <= 65535)
                                        port = parsed;
                        }
                        if (host.empty())
                                host = input.substr(0, close);
                        return;
                }
        }
        size_t firstColon = input.find(':');
        size_t lastColon = input.rfind(':');
        if (firstColon != std::string::npos && lastColon != firstColon)
        {
                        host = input;
                        return;
        }
        if (lastColon != std::string::npos)
        {
                host = input.substr(0, lastColon);
                int parsed = atoi(input.substr(lastColon + 1).c_str());
                if (parsed > 0 && parsed <= 65535)
                        port = parsed;
        }
        else
        {
                host = input;
        }
        if (host.empty())
                host = input;
}

static bool WinRT_ParseLiteralIPv4(const std::string &input, int port, netadr_t &out)
{
        std::wstring wide = WinRT_FromUtf8(input);
        if (wide.empty())
                return false;
        if (!WinRT_ParseIPv4String(wide, out.address.ip))
                return false;
        out.type = NA_IP;
        out.port = BigShort(static_cast<unsigned short>(port));
        out.prot = NP_DGRAM;
        out.scopeid = 0;
        return true;
}

static bool WinRT_ParseLiteralIPv6(const std::string &input, int port, netadr_t &out)
{
        std::wstring wide = WinRT_FromUtf8(input);
        if (wide.empty())
                return false;
        unsigned int scope = 0;
        if (!WinRT_ParseIPv6String(wide, out.address.ip6, scope))
                return false;
        out.type = NA_IPV6;
        out.port = BigShort(static_cast<unsigned short>(port));
        out.prot = NP_DGRAM;
        out.scopeid = scope;
        return true;
}

struct WinRTPendingDatagram
{
        netadr_t from{};
        std::vector<uint8_t> payload;
};

struct WinRTDatagramConnection : ftenet_generic_connection_s
{
        DatagramSocket socket{nullptr};
        winrt::event_token token{};
        std::mutex mutex;
        std::deque<WinRTPendingDatagram> pending;
        unsigned short boundPort{0};
        bool shuttingDown{false};

        void OnMessageReceived(DatagramSocket const &, DatagramSocketMessageReceivedEventArgs const &args)
        {
                WinRTPendingDatagram packet;
                if (!WinRT_ParseEndpoint(args.RemoteAddress(), args.RemotePort(), packet.from))
                        return;
                auto reader = args.GetDataReader();
                uint32_t length = reader.UnconsumedBufferLength();
                packet.payload.resize(length);
                if (length)
                {
                        reader.ReadBytes(winrt::array_view<uint8_t>(packet.payload.data(), packet.payload.data() + packet.payload.size()));
                }
                reader.DetachStream();
                std::scoped_lock lock(mutex);
                pending.emplace_back(std::move(packet));
        }
};

#ifdef HAVE_WEBSOCKCL
struct WinRTWebSocketConnection : ftenet_generic_connection_s
{
        MessageWebSocket socket{nullptr};
        IOutputStream output{nullptr};
        winrt::event_token messageToken{};
        winrt::event_token closedToken{};
        std::mutex mutex;
        std::deque<std::vector<uint8_t>> pending;
        bool failed{false};
        netadr_t remote{};
        std::string url;
        netproto_t protocol{NP_WS};

        void OnMessageReceived(MessageWebSocket const &, MessageWebSocketMessageReceivedEventArgs const &args)
        {
                if (args.MessageType() != SocketMessageType::Binary)
                        return;
                auto reader = args.GetDataReader();
                uint32_t length = reader.UnconsumedBufferLength();
                std::vector<uint8_t> data(length);
                if (length)
                        reader.ReadBytes(winrt::array_view<uint8_t>(data.data(), data.data() + data.size()));
                reader.DetachStream();
                std::scoped_lock lock(mutex);
                pending.emplace_back(std::move(data));
        }

        void OnClosed(MessageWebSocket const &, MessageWebSocketClosedEventArgs const &)
        {
                std::scoped_lock lock(mutex);
                failed = true;
        }
};
#endif

struct WinRTStreamHandle
{
        StreamSocket socket{nullptr};
        std::wstring host;
        unsigned short port{0};
        int id{0};
};

struct WinRTTCPFile
{
        vfsfile_s funcs;
        WinRTStreamHandle *handle;
        bool readClosed;
        bool writeClosed;
        bool closed;
        char peer[128];
};

static std::mutex g_streamTableMutex;
static std::unordered_map<int, WinRTStreamHandle *> g_streamTable;
static int g_nextStreamId = 1;

static SOCKET WinRT_StreamHandleToSocket(WinRTStreamHandle *handle)
{
        if (!handle)
                return INVALID_SOCKET;
        std::scoped_lock lock(g_streamTableMutex);
        if (!handle->id)
        {
                do
                {
                        handle->id = g_nextStreamId++;
                        if (g_nextStreamId <= 0)
                                g_nextStreamId = 1;
                }
                while (g_streamTable.find(handle->id) != g_streamTable.end());
        }
        g_streamTable[handle->id] = handle;
        return (SOCKET)handle->id;
}

static WinRTStreamHandle *WinRT_StreamHandleFromSocket(SOCKET sock)
{
        if (sock == INVALID_SOCKET)
                return nullptr;
        std::scoped_lock lock(g_streamTableMutex);
        auto it = g_streamTable.find((int)sock);
        if (it == g_streamTable.end())
                return nullptr;
        WinRTStreamHandle *handle = it->second;
        g_streamTable.erase(it);
        return handle;
}

static void WinRT_StreamDestroy(WinRTStreamHandle *handle)
{
        if (!handle)
                return;
        {
                std::scoped_lock lock(g_streamTableMutex);
                if (handle->id)
                {
                        auto it = g_streamTable.find(handle->id);
                        if (it != g_streamTable.end() && it->second == handle)
                                g_streamTable.erase(it);
                }
        }
        try
        {
                if (handle->socket)
                        handle->socket.Close();
        }
        catch (const winrt::hresult_error &)
        {
        }
        delete handle;
}

static int QDECL WinRT_TCP_ReadBytes(struct vfsfile_s *file, void *buffer, int bytestoread)
{
        auto *tcp = reinterpret_cast<WinRTTCPFile *>(file);
        if (!tcp || tcp->closed || !tcp->handle)
                return VFS_ERROR_UNSPECIFIED;
        if (bytestoread <= 0)
                return 0;
        if (tcp->readClosed)
                return VFS_ERROR_EOF;
        try
        {
                DataReader reader(tcp->handle->socket.InputStream());
                reader.InputStreamOptions(InputStreamOptions::Partial);
                uint32_t request = static_cast<uint32_t>(bytestoread);
                if (!request)
                        request = 1;
                uint32_t loaded = reader.LoadAsync(request).get();
                if (!loaded)
                {
                        reader.DetachStream();
                        tcp->readClosed = true;
                        return VFS_ERROR_EOF;
                }
                reader.ReadBytes({reinterpret_cast<uint8_t *>(buffer), reinterpret_cast<uint8_t *>(buffer) + loaded});
                reader.DetachStream();
                return static_cast<int>(loaded);
        }
        catch (const winrt::hresult_error &)
        {
                tcp->readClosed = true;
                return VFS_ERROR_UNSPECIFIED;
        }
}

static int QDECL WinRT_TCP_WriteBytes(struct vfsfile_s *file, const void *buffer, int bytestowrite)
{
        auto *tcp = reinterpret_cast<WinRTTCPFile *>(file);
        if (!tcp || tcp->closed || !tcp->handle)
                return VFS_ERROR_UNSPECIFIED;
        if (bytestowrite <= 0)
                return 0;
        if (tcp->writeClosed)
                return VFS_ERROR_UNSPECIFIED;
        try
        {
                DataWriter writer(tcp->handle->socket.OutputStream());
                writer.WriteBytes({reinterpret_cast<const uint8_t *>(buffer), reinterpret_cast<const uint8_t *>(buffer) + bytestowrite});
                writer.StoreAsync().get();
                writer.DetachStream();
                return bytestowrite;
        }
        catch (const winrt::hresult_error &)
        {
                tcp->writeClosed = true;
                return VFS_ERROR_NORESPONSE;
        }
}

static qboolean QDECL WinRT_TCP_Seek(struct vfsfile_s *file, qofs_t pos)
{
        (void)file;
        (void)pos;
        return false;
}

static qofs_t QDECL WinRT_TCP_Tell(struct vfsfile_s *file)
{
        (void)file;
        return 0;
}

static qofs_t QDECL WinRT_TCP_GetLen(struct vfsfile_s *file)
{
        (void)file;
        return 0;
}

static void QDECL WinRT_TCP_Flush(struct vfsfile_s *file)
{
        (void)file;
}

static qboolean QDECL WinRT_TCP_Close(struct vfsfile_s *file)
{
        auto *tcp = reinterpret_cast<WinRTTCPFile *>(file);
        if (!tcp)
                return false;
        if (!tcp->closed)
        {
                tcp->closed = true;
                WinRT_StreamDestroy(tcp->handle);
                tcp->handle = nullptr;
        }
        Z_Free(tcp);
        return true;
}

extern "C" vfsfile_t *FS_WrapTCPSocket(SOCKET sock, qboolean conpending, const char *peername)
{
        (void)conpending;
        WinRTStreamHandle *handle = WinRT_StreamHandleFromSocket(sock);
        if (!handle)
                return NULL;
        WinRTTCPFile *file = (WinRTTCPFile *)Z_Malloc(sizeof(WinRTTCPFile));
        if (!file)
        {
                WinRT_StreamDestroy(handle);
                return NULL;
        }
        memset(file, 0, sizeof(WinRTTCPFile));
        file->handle = handle;
        file->readClosed = false;
        file->writeClosed = false;
        file->closed = false;
        if (peername)
                Q_strncpyz(file->peer, peername, sizeof(file->peer));
        else
                file->peer[0] = '\0';
        file->funcs.ReadBytes = WinRT_TCP_ReadBytes;
        file->funcs.WriteBytes = WinRT_TCP_WriteBytes;
        file->funcs.Seek = WinRT_TCP_Seek;
        file->funcs.Tell = WinRT_TCP_Tell;
        file->funcs.GetLen = WinRT_TCP_GetLen;
        file->funcs.Close = WinRT_TCP_Close;
        file->funcs.Flush = WinRT_TCP_Flush;
        return &file->funcs;
}

#if defined(HAVE_CLIENT) || defined(HAVE_SERVER)
#define MAX_LOOPBACK    64
typedef struct
{
        qbyte   *data;
        int             datalen;
        int             datamax;
} loopmsg_t;

typedef struct
{
        qboolean        inited;
        loopmsg_t       msgs[MAX_LOOPBACK];
        int                     get, send;
} loopback_t;

static loopback_t       loopbacks[2];

static qboolean NET_GetLoopPacket (int sock, netadr_t *from, sizebuf_t *message)
{
        int             i;
        loopback_t      *loop;

        sock &= 1;

        loop = &loopbacks[sock];

        if (loop->send - loop->get > MAX_LOOPBACK)
        {
                loop->get = loop->send - MAX_LOOPBACK;
        }

        if (loop->get >= loop->send)
                return false;

        i = loop->get & (MAX_LOOPBACK-1);
        loop->get++;

        if (message->maxsize < loop->msgs[i].datalen)
                return false;

        memcpy (message->data, loop->msgs[i].data, loop->msgs[i].datalen);
        message->cursize = loop->msgs[i].datalen;
        memset (from, 0, sizeof(*from));
        from->type = NA_LOOPBACK;
        message->packing = SZ_RAWBYTES;
        message->currentbit = 0;
        loop->msgs[i].datalen = 0;
        return true;
}

static neterr_t NET_SendLoopPacket (int sock, int length, const void *data, netadr_t *to)
{
        int             i;
        loopback_t      *loop;
        if (!length && !data)
                return NETERR_SENT;
        if (length > sizeof(net_message_buffer))
                return NETERR_MTU;
        if (net_fakemtu.ival)
                if (length > abs(net_fakemtu.ival))
                        return (net_fakemtu.ival < 0)?NETERR_MTU:NETERR_SENT;

        sock &= 1;

        loop = &loopbacks[sock^1];
        if (!loop->inited)
                return NETERR_NOROUTE;

        i = loop->send & (MAX_LOOPBACK-1);
        if (length > loop->msgs[i].datamax)
        {
                loop->msgs[i].datamax = length + 1024;
                BZ_Free(loop->msgs[i].data);
                loop->msgs[i].data = BZ_Malloc(loop->msgs[i].datamax);
        }

        loop->send++;

        memcpy (loop->msgs[i].data, data, length);
        loop->msgs[i].datalen = length;
        return NETERR_SENT;
}

static int WinRT_Datagram_GetLocalAddresses(struct ftenet_generic_connection_s *base, unsigned int *adrflags, netadr_t *addresses, const char **adrparams, int maxaddresses)
{
        if (!base || !addresses || maxaddresses <= 0)
                return 0;
        auto *con = static_cast<WinRTDatagramConnection *>(base);
        try
        {
                auto info = con->socket.Information();
                if (!info)
                        return 0;
                netadr_t local{};
                if (!WinRT_ParseEndpoint(info.LocalAddress(), info.LocalPort(), local))
                        return 0;
                local.port = BigShort(con->boundPort);
                local.prot = NP_DGRAM;
                addresses[0] = local;
                if (adrflags)
                        *adrflags = 0;
                if (adrparams)
                        *adrparams = NULL;
                return 1;
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static qboolean WinRT_Datagram_GetPacket(ftenet_generic_connection_t *base)
{
        auto *con = static_cast<WinRTDatagramConnection *>(base);
        if (!con)
                return false;
        std::scoped_lock lock(con->mutex);
        if (con->pending.empty())
                return false;
        WinRTPendingDatagram packet = std::move(con->pending.front());
        con->pending.pop_front();
        if (packet.payload.size() > net_message.maxsize)
                return false;
        memcpy(net_message.data, packet.payload.data(), packet.payload.size());
        net_message.cursize = static_cast<int>(packet.payload.size());
        net_message.packing = SZ_RAWBYTES;
        net_message.currentbit = 0;
        net_from = packet.from;
        return true;
}

static neterr_t WinRT_Datagram_SendPacket(ftenet_generic_connection_t *base, int length, const void *data, netadr_t *to)
{
        auto *con = static_cast<WinRTDatagramConnection *>(base);
        if (!con || !data || length <= 0 || !to)
                return NETERR_NOROUTE;
        if (net_fakemtu.ival && length > abs(net_fakemtu.ival))
                return (net_fakemtu.ival < 0) ? NETERR_MTU : NETERR_SENT;
        std::wstring host;
        std::wstring service;
        if (!WinRT_FormatAddress(*to, host, service))
                return NETERR_NOROUTE;
        try
        {
                HostName remoteHost(host);
                IOutputStream stream = con->socket.GetOutputStreamAsync(remoteHost, service).get();
                DataWriter writer(stream);
                writer.WriteBytes({reinterpret_cast<const uint8_t *>(data), reinterpret_cast<const uint8_t *>(data) + length});
                writer.StoreAsync().get();
                writer.DetachStream();
                return NETERR_SENT;
        }
        catch (const winrt::hresult_error &)
        {
                return NETERR_DISCONNECTED;
        }
}

static void WinRT_Datagram_Close(ftenet_generic_connection_t *base)
{
        auto *con = static_cast<WinRTDatagramConnection *>(base);
        if (!con)
                return;
        {
                std::scoped_lock lock(con->mutex);
                con->pending.clear();
        }
        try
        {
                if (con->token.value)
                        con->socket.MessageReceived(con->token);
        }
        catch (const winrt::hresult_error &)
        {
        }
        try
        {
                con->socket.Close();
        }
        catch (const winrt::hresult_error &)
        {
        }
        Z_Free(con);
}

#ifdef HAVE_WEBSOCKCL
static int WinRT_WebSocket_GetLocalAddresses(struct ftenet_generic_connection_s *base, unsigned int *adrflags, netadr_t *addresses, const char **adrparams, int maxaddresses)
{
        auto *con = static_cast<WinRTWebSocketConnection *>(base);
        if (!con || !addresses || maxaddresses <= 0)
                return 0;
        try
        {
                auto info = con->socket.Information();
                if (!info)
                        return 0;
                netadr_t local{};
                if (!WinRT_ParseEndpoint(info.LocalAddress(), info.LocalPort(), local))
                        return 0;
                local.prot = con->protocol;
                addresses[0] = local;
                if (adrflags)
                        *adrflags = 0;
                if (adrparams)
                        *adrparams = con->url.c_str();
                return 1;
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

static qboolean WinRT_WebSocket_GetPacket(ftenet_generic_connection_t *base)
{
        auto *con = static_cast<WinRTWebSocketConnection *>(base);
        if (!con)
                return false;
        std::vector<uint8_t> payload;
        {
                std::scoped_lock lock(con->mutex);
                if (con->pending.empty())
                        return false;
                payload = std::move(con->pending.front());
                con->pending.pop_front();
        }
        if (payload.size() > net_message.maxsize)
                return false;
        if (!payload.empty())
                memcpy(net_message.data, payload.data(), payload.size());
        net_message.cursize = static_cast<int>(payload.size());
        net_message.packing = SZ_RAWBYTES;
        net_message.currentbit = 0;
        net_from = con->remote;
        return true;
}

static neterr_t WinRT_WebSocket_SendPacket(ftenet_generic_connection_t *base, int length, const void *data, netadr_t *to)
{
        auto *con = static_cast<WinRTWebSocketConnection *>(base);
        if (!con || !data || length < 0)
                return NETERR_NOROUTE;
        if (con->failed)
                return NETERR_DISCONNECTED;
        if (!to || to->type != con->remote.type || to->prot != con->remote.prot || strcmp(to->address.websocketurl, con->remote.address.websocketurl))
                return NETERR_NOROUTE;
        try
        {
                DataWriter writer(con->socket.OutputStream());
                writer.WriteBytes({reinterpret_cast<const uint8_t *>(data), reinterpret_cast<const uint8_t *>(data) + length});
                writer.StoreAsync().get();
                writer.DetachStream();
                return NETERR_SENT;
        }
        catch (const winrt::hresult_error &)
        {
                con->failed = true;
                return NETERR_DISCONNECTED;
        }
}

static void WinRT_WebSocket_Close(ftenet_generic_connection_t *base)
{
        auto *con = static_cast<WinRTWebSocketConnection *>(base);
        if (!con)
                return;
        try
        {
                if (con->messageToken.value)
                        con->socket.MessageReceived(con->messageToken);
        }
        catch (const winrt::hresult_error &)
        {
        }
        try
        {
                if (con->closedToken.value)
                        con->socket.Closed(con->closedToken);
        }
        catch (const winrt::hresult_error &)
        {
        }
        try
        {
                if (con->socket)
                        con->socket.Close();
        }
        catch (const winrt::hresult_error &)
        {
        }
        Z_Free(con);
}

static WinRTWebSocketConnection *WinRT_WebSocket_Establish(ftenet_connections_t *col, const char *name, const char *address, netproto_t protocol)
{
        WinRTWebSocketConnection *con = (WinRTWebSocketConnection *)Z_Malloc(sizeof(WinRTWebSocketConnection));
        if (!con)
                return NULL;
        memset(con, 0, sizeof(*con));
        try
        {
                std::string url = address ? address : "";
                if (url.empty())
                {
                        delete con;
                        return NULL;
                }
                if (url.find("://") == std::string::npos)
                {
                        if (protocol == NP_WSS)
                                url = "wss://" + url;
                        else
                                url = "ws://" + url;
                }
                Uri uri(WinRT_FromUtf8(url));
                con->socket = MessageWebSocket();
                con->socket.Control().MessageType(SocketMessageType::Binary);
                con->messageToken = con->socket.MessageReceived({con, &WinRTWebSocketConnection::OnMessageReceived});
                con->closedToken = con->socket.Closed({con, &WinRTWebSocketConnection::OnClosed});
                con->socket.ConnectAsync(uri).get();
                con->output = con->socket.OutputStream();
                con->protocol = protocol;
                con->remote.type = NA_WEBSOCKET;
                con->remote.prot = protocol;
                int port = uri.Port();
                if (port <= 0)
                        port = (protocol == NP_WSS) ? 443 : 80;
                con->remote.port = BigShort(static_cast<unsigned short>(port));
                con->url = url;
                strlcpy(con->remote.address.websocketurl, url.c_str(), sizeof(con->remote.address.websocketurl));
                con->GetPacket = WinRT_WebSocket_GetPacket;
                con->SendPacket = WinRT_WebSocket_SendPacket;
                con->Close = WinRT_WebSocket_Close;
                con->GetLocalAddresses = WinRT_WebSocket_GetLocalAddresses;
                con->addrtype[0] = NA_WEBSOCKET;
                con->addrtype[1] = NA_INVALID;
                con->prot = protocol;
                con->owner = col;
                con->islisten = false;
                strlcpy(con->name, name ? name : "WebSocket", sizeof(con->name));
                return con;
        }
        catch (const winrt::hresult_error &)
        {
        }
        catch (...)
        {
        }
        WinRT_WebSocket_Close(con);
        return NULL;
}
#endif

static WinRTDatagramConnection *WinRT_Datagram_Establish(ftenet_connections_t *col, const char *name, const char *address)
{
        WinRTDatagramConnection *con = (WinRTDatagramConnection *)Z_Malloc(sizeof(WinRTDatagramConnection));
        if (!con)
                return NULL;
        memset(con, 0, sizeof(*con));
        try
        {
                con->socket = DatagramSocket();
                std::wstring host;
                unsigned short port = WinRT_StringToPort(address, host);
                std::wstring service = WinRT_ServiceFromPort(port);
                if (host.empty())
                        con->socket.BindServiceNameAsync(service).get();
                else
                        con->socket.BindEndpointAsync(HostName(host), service).get();
                auto info = con->socket.Information();
                con->boundPort = WinRT_ParseService(info.LocalPort());
                con->token = con->socket.MessageReceived({con, &WinRTDatagramConnection::OnMessageReceived});
                con->owner = col;
                con->GetLocalAddresses = WinRT_Datagram_GetLocalAddresses;
                con->GetPacket = WinRT_Datagram_GetPacket;
                con->SendPacket = WinRT_Datagram_SendPacket;
                con->Close = WinRT_Datagram_Close;
                con->islisten = col->islisten;
                con->prot = NP_DGRAM;
                con->addrtype[0] = NA_IP;
                con->addrtype[1] = NA_INVALID;
                if (info)
                {
                        netadr_t resolved{};
                        if (WinRT_ParseEndpoint(info.LocalAddress(), info.LocalPort(), resolved))
                        {
                                con->addrtype[0] = resolved.type;
                                con->addrtype[1] = NA_INVALID;
                        }
                }
                con->thesocket = con->boundPort;
                strlcpy(con->name, name ? name : "UDP", sizeof(con->name));
                return con;
        }
        catch (const winrt::hresult_error &)
        {
                try
                {
                        con->socket.Close();
                }
                catch (const winrt::hresult_error &)
                {
                }
                Z_Free(con);
                return NULL;
        }
}

static int FTENET_Loop_GetLocalAddresses(struct ftenet_generic_connection_s *con, unsigned int *adrflags, netadr_t *addresses, const char **adrparams, int maxaddresses)
{
        if (maxaddresses)
        {
                addresses->type = NA_LOOPBACK;
                addresses->port = con->thesocket+1;
                *adrflags = 0;
                *adrparams = NULL;
                return 1;
        }
        return 0;
}

static qboolean FTENET_Loop_GetPacket(ftenet_generic_connection_t *con)
{
        return NET_GetLoopPacket(con->thesocket, &net_from, &net_message);
}

static neterr_t FTENET_Loop_SendPacket(ftenet_generic_connection_t *con, int length, const void *data, netadr_t *to)
{
        if (to->type == NA_LOOPBACK)
        {
                return NET_SendLoopPacket(con->thesocket, length, data, to);
        }
        return NETERR_NOROUTE;
}

static void FTENET_Loop_Close(ftenet_generic_connection_t *con)
{
        int i;
        int sock = con->thesocket;
        sock &= 1;
        loopbacks[sock].inited = false;
        loopbacks[sock].get = loopbacks[sock].send = 0;
        for (i = 0; i < MAX_LOOPBACK; i++)
        {
                BZ_Free(loopbacks[sock].msgs[i].data);
                loopbacks[sock].msgs[i].data = NULL;
                loopbacks[sock].msgs[i].datalen = 0;
                loopbacks[sock].msgs[i].datamax = 0;
        }
        Z_Free(con);
}

static ftenet_generic_connection_t *FTENET_Loop_EstablishConnection(ftenet_connections_t *col)
{
        ftenet_generic_connection_t *newcon;
        int sock;
        for (sock = 0; sock < countof(loopbacks); sock++)
                if (!loopbacks[sock].inited)
                        break;
        if (sock == countof(loopbacks))
                return NULL;
        newcon = (ftenet_generic_connection_t*)Z_Malloc(sizeof(*newcon));
        if (newcon)
        {
                loopbacks[sock].inited = true;
                loopbacks[sock].get = loopbacks[sock].send = 0;

                newcon->GetLocalAddresses = FTENET_Loop_GetLocalAddresses;
                newcon->GetPacket = FTENET_Loop_GetPacket;
                newcon->SendPacket = FTENET_Loop_SendPacket;
                newcon->Close = FTENET_Loop_Close;
                newcon->islisten = col->islisten;
                newcon->prot = NP_DGRAM;
                newcon->addrtype[0] = NA_LOOPBACK;
                newcon->thesocket = sock;
        }
        return newcon;
}
#endif

ftenet_connections_t *FTENET_CreateCollection(qboolean listen, void(*ReadPacket)(void))
{
        ftenet_connections_t *col = (ftenet_connections_t*)Z_Malloc(sizeof(*col));
        if (!col)
                return NULL;
        memset(col, 0, sizeof(*col));
        col->islisten = listen;
        col->ReadGamePacket = ReadPacket;
        return col;
}

void FTENET_CloseCollection(ftenet_connections_t *col)
{
        int i;
        if (!col)
                return;
        for (i = 0; i < MAX_CONNECTIONS; i++)
        {
                if (col->conn[i])
                {
                        col->conn[i]->Close(col->conn[i]);
                        col->conn[i] = NULL;
                }
        }
        Z_Free(col);
}

qboolean FTENET_AddToCollection(ftenet_connections_t *col, const char *name, const char *address, netadrtype_t addrtype, netproto_t addrprot)
{
        int i;
        if (!col)
                return false;
        if (addrtype == NA_LOOPBACK)
        {
#if defined(HAVE_SERVER) && defined(HAVE_CLIENT)
                ftenet_generic_connection_t *con = FTENET_Loop_EstablishConnection(col);
                if (!con)
                        return false;
                strlcpy(con->name, name ? name : "Loop", sizeof(con->name));
                con->owner = col;
                for (i = 0; i < MAX_CONNECTIONS; i++)
                {
                        if (!col->conn[i])
                        {
                                col->conn[i] = con;
                                con->connum = i+1;
                                return true;
                        }
                }
                con->Close(con);
#endif
                return false;
        }

        if (!net_enabled.ival)
        {
                Con_Printf("%sNetworking disabled (net_enabled=0).\n", CON_WARNING);
                return false;
        }

        if (addrprot == NP_DGRAM && (addrtype == NA_IP || addrtype == NA_IPV6 || addrtype == NA_INVALID))
        {
                WinRTDatagramConnection *con = WinRT_Datagram_Establish(col, name, address);
                if (!con)
                {
                        Con_Printf("%sFailed to bind UDP socket for %s.\n", CON_WARNING, name ? name : "");
                        return false;
                }
                con->owner = col;
                for (i = 0; i < MAX_CONNECTIONS; i++)
                {
                        if (!col->conn[i])
                        {
                                col->conn[i] = con;
                                con->connum = i+1;
                                return true;
                        }
                }
                con->Close(con);
                return false;
        }

#ifdef HAVE_WEBSOCKCL
        if (addrprot == NP_WS || addrprot == NP_WSS)
        {
                WinRTWebSocketConnection *ws = WinRT_WebSocket_Establish(col, name, address, addrprot);
                if (!ws)
                {
                        Con_Printf("%sFailed to open WinRT WebSocket for %s.\n", CON_WARNING, name ? name : "");
                        return false;
                }
                ws->owner = col;
                for (i = 0; i < MAX_CONNECTIONS; i++)
                {
                        if (!col->conn[i])
                        {
                                col->conn[i] = ws;
                                ws->connum = i + 1;
                                return true;
                        }
                }
                ws->Close(ws);
                return false;
        }
#endif

        Con_Printf("%sWinRT backend does not support %s (%d/%d).\n", CON_WARNING, name ? name : "socket", addrtype, addrprot);
        return false;
}

void NET_Init (void)
{
        net_message.maxsize = sizeof(net_message_buffer);
        net_message.data = net_message_buffer;

        Cvar_Register(&net_enabled, "networking");
        Cvar_Register(&timeout, "networking");
        Cvar_Register(&net_fakeloss, "networking");
        Cvar_Register(&net_fakemtu, "networking");
        Cvar_Register(&net_ice_broker, "networking");
        Cvar_Register(&net_hybriddualstack, "networking");
        Cvar_Register(&net_dns_ipv4, "networking");
        Cvar_Register(&net_dns_ipv6, "networking");
#if defined(HAVE_SSL)
        Cvar_Register(&tls_ignorecertificateerrors, "networking");
#if defined(TCPCONNECT) && (defined(HAVE_SERVER) || defined(HAVE_HTTPSV))
        Cvar_Register(&net_enable_tls, "networking");
#ifdef HAVE_HTTPSV
        Cvar_Register(&net_enable_http, "networking");
        Cvar_Register(&net_enable_rtcbroker, "networking");
        Cvar_Register(&net_enable_websockets, "networking");
#endif
#endif
#ifdef HAVE_SERVER
        Cvar_Register(&net_enable_qizmo, "networking");
#endif
#ifdef MVD_RECORDING
        Cvar_Register(&net_enable_qtv, "networking");
#endif
#endif
#if defined(HAVE_DTLS)
        Cvar_Register(&net_enable_dtls, "networking");
        Cvar_Register(&dtls_psk_hint, "networking");
        Cvar_Register(&dtls_psk_user, "networking");
        Cvar_Register(&dtls_psk_key, "networking");
#endif
#ifdef HAVE_CLIENT
        Cvar_Register(&cl_delay_packets, "networking");
#endif
        Cvar_Register(&net_ice_servers, "networking");
        Cvar_Register(&net_ice_relayonly, "networking");

        Con_Printf("%sWinRT networking backend initialized (IPv4/IPv6 UDP, TCP/TLS, WebSockets).\n", CON_INFO);
}

void NET_Shutdown (void)
{
        NET_CloseClient();
#ifdef HAVE_SERVER
        NET_CloseServer();
#endif
}

void NET_ReadPackets (ftenet_connections_t *collection)
{
        int i;
        if (!collection)
                return;
        for (i = 0; i < MAX_CONNECTIONS; i++)
        {
                ftenet_generic_connection_t *con = collection->conn[i];
                if (!con || !con->GetPacket)
                        continue;
                if (con->GetPacket(con))
                {
                        NET_UpdateRates(collection, true, net_message.cursize);
                        net_from_connection = con;
                        if (collection->ReadGamePacket)
                                collection->ReadGamePacket();
                }
        }
}

neterr_t NET_SendPacket (ftenet_connections_t *collection, int length, const void *data, netadr_t *to)
{
        neterr_t status = NETERR_NOROUTE;
        if (!collection || !to || length <= 0 || !data)
                return status;
        for (int i = 0; i < MAX_CONNECTIONS; ++i)
        {
                ftenet_generic_connection_t *con = collection->conn[i];
                if (!con || !con->SendPacket)
                        continue;
                neterr_t result = con->SendPacket(con, length, data, to);
                if (result == NETERR_SENT)
                {
                        NET_UpdateRates(collection, false, (size_t)length);
                        return result;
                }
                if (result != NETERR_NOROUTE)
                        status = result;
        }
        return status;
}

qboolean NET_UpdateRates(ftenet_connections_t *collection, qboolean inbound, size_t size)
{
        if (!collection)
                return false;
        if (inbound)
        {
                collection->bytesin += (unsigned int)size;
                collection->packetsin++;
        }
        else
        {
                collection->bytesout += (unsigned int)size;
                collection->packetsout++;
        }
        return true;
}

qboolean NET_GetRates(ftenet_connections_t *collection, float *pi, float *po, float *bi, float *bo)
{
        if (!collection)
                return false;
        if (pi) *pi = collection->packetsinrate;
        if (po) *po = collection->packetsoutrate;
        if (bi) *bi = collection->bytesinrate;
        if (bo) *bo = collection->bytesoutrate;
        return true;
}

void NET_Tick(void)
{
        /* no-op */
}

void NET_GetLocalAddress (int socket, netadr_t *out)
{
        (void)socket;
        if (!out)
                return;
        memset(out, 0, sizeof(*out));
        try
        {
                auto hostNames = NetworkInformation::GetHostNames();
                netadr_t fallback{};
                for (auto const &name : hostNames)
                {
                        std::wstring raw = name.RawName().c_str();
                        if (raw.empty())
                                continue;
                        netadr_t local{};
                        unsigned int scope = 0;
                        if (name.Type() == HostNameType::Ipv4 && WinRT_ParseIPv4String(raw, local.address.ip))
                        {
                                local.type = NA_IP;
                                local.port = 0;
                                local.prot = NP_DGRAM;
                                *out = local;
                                return;
                        }
                        if (name.Type() == HostNameType::Ipv6 && WinRT_ParseIPv6String(raw, local.address.ip6, scope))
                        {
                                local.type = NA_IPV6;
                                local.scopeid = scope;
                                local.port = 0;
                                local.prot = NP_DGRAM;
                                fallback = local;
                        }
                }
                if (fallback.type != NA_INVALID)
                {
                        *out = fallback;
                        return;
                }
        }
        catch (const winrt::hresult_error &)
        {
        }
        out->type = NA_LOOPBACK;
}

qboolean NET_StringToAdr_NoDNS(const char *address, int port, netadr_t *out)
{
        if (!address || !out)
                return false;
        if (!strcmp(address, "loopback"))
        {
                memset(out, 0, sizeof(*out));
                out->type = NA_LOOPBACK;
                out->port = BigShort((short)port);
                return true;
        }
        std::string host = address;
        int usePort = port;
        WinRT_SplitHostPort(host, host, usePort, port);
        if (WinRT_ParseLiteralIPv4(host, usePort, *out))
                return true;
        return WinRT_ParseLiteralIPv6(host, usePort, *out);
}

size_t NET_StringToAdr2 (const char *s, int defaultport, netadr_t *a, size_t addrcount, const char **pathstart)
{
        if (!a || !addrcount)
                return 0;
        if (!s)
                return 0;
        if (!strcmp(s, "loopback"))
        {
                memset(a, 0, sizeof(*a));
                a->type = NA_LOOPBACK;
                a->port = BigShort((short)defaultport);
                return 1;
        }
        std::string input = s;
        int port = defaultport;
        WinRT_SplitHostPort(input, input, port, defaultport);
        netadr_t literal;
        if (WinRT_ParseLiteralIPv4(input, port, literal) || WinRT_ParseLiteralIPv6(input, port, literal))
        {
                a[0] = literal;
                return 1;
        }
        if (!net_dns_ipv4.ival && !net_dns_ipv6.ival)
                return 0;
        try
        {
                HostName hostName(WinRT_FromUtf8(input));
                hstring service = hstring(WinRT_ServiceFromPort(static_cast<unsigned short>(port)));
                auto endpoints = DatagramSocket::GetEndpointPairsAsync(hostName, service).get();
                size_t count = 0;
                for (auto const &ep : endpoints)
                {
                        netadr_t entry{};
                        if (!WinRT_ParseEndpoint(ep.RemoteHostName(), ep.RemoteServiceName(), entry))
                                continue;
                        entry.port = BigShort(static_cast<unsigned short>(port));
                        entry.prot = NP_DGRAM;
                        if (entry.type == NA_IP && !net_dns_ipv4.ival)
                                continue;
                        if (entry.type == NA_IPV6 && !net_dns_ipv6.ival)
                                continue;
                        a[count++] = entry;
                        if (count >= addrcount)
                                break;
                }
                return count;
        }
        catch (const winrt::hresult_error &)
        {
                return 0;
        }
}

char *NET_AdrToString (char *s, int len, netadr_t *a)
{
        if (!s || !len)
                return s;
        if (!a)
        {
                strlcpy(s, "<null>", len);
                return s;
        }
        if (a->type == NA_LOOPBACK)
        {
                strlcpy(s, "loopback", len);
                return s;
        }
        if (a->type == NA_IP)
        {
                Q_snprintfz(s, len, "%u.%u.%u.%u:%u", a->address.ip[0], a->address.ip[1], a->address.ip[2], a->address.ip[3], BigShort(a->port));
                return s;
        }
        if (a->type == NA_IPV6)
        {
                std::string text = WinRT_ToUtf8(WinRT_FormatIPv6(a->address.ip6, a->scopeid));
                Q_snprintfz(s, len, "[%s]:%u", text.c_str(), BigShort(a->port));
                return s;
        }
#ifdef HAVE_WEBSOCKCL
        if (a->type == NA_WEBSOCKET)
        {
                Q_snprintfz(s, len, "%s", a->address.websocketurl);
                return s;
        }
#endif
        strlcpy(s, "unknown", len);
        return s;
}

char *NET_BaseAdrToString (char *s, int len, netadr_t *a)
{
        if (!s || !len)
                return s;
        if (!a)
        {
                strlcpy(s, "<null>", len);
                return s;
        }
        if (a->type == NA_IP)
        {
                Q_snprintfz(s, len, "%u.%u.%u.%u", a->address.ip[0], a->address.ip[1], a->address.ip[2], a->address.ip[3]);
                return s;
        }
        if (a->type == NA_IPV6)
        {
                std::string text = WinRT_ToUtf8(WinRT_FormatIPv6(a->address.ip6, a->scopeid));
                Q_snprintfz(s, len, "%s", text.c_str());
                return s;
        }
#ifdef HAVE_WEBSOCKCL
        if (a->type == NA_WEBSOCKET)
        {
                Q_snprintfz(s, len, "%s", a->address.websocketurl);
                return s;
        }
#endif
        return NET_AdrToString(s, len, a);
}

qboolean NET_CompareAdr (netadr_t *a, netadr_t *b)
{
        if (!a || !b)
                return false;
        if (a->type != b->type)
                return false;
        if (a->type == NA_LOOPBACK)
                return true;
        if (a->type == NA_IP)
                return a->port == b->port && !memcmp(a->address.ip, b->address.ip, sizeof(a->address.ip));
        if (a->type == NA_IPV6)
                return a->port == b->port && !memcmp(a->address.ip6, b->address.ip6, sizeof(a->address.ip6)) && a->scopeid == b->scopeid;
#ifdef HAVE_WEBSOCKCL
        if (a->type == NA_WEBSOCKET)
                return a->port == b->port && !strcmp(a->address.websocketurl, b->address.websocketurl);
#endif
        return false;
}

qboolean NET_CompareBaseAdr (netadr_t *a, netadr_t *b)
{
        if (!a || !b)
                return false;
        if (a->type != b->type)
                return false;
        if (a->type == NA_LOOPBACK)
                return true;
        if (a->type == NA_IP)
                return !memcmp(a->address.ip, b->address.ip, sizeof(a->address.ip));
        if (a->type == NA_IPV6)
                return !memcmp(a->address.ip6, b->address.ip6, sizeof(a->address.ip6));
#ifdef HAVE_WEBSOCKCL
        if (a->type == NA_WEBSOCKET)
                return !strcmp(a->address.websocketurl, b->address.websocketurl);
#endif
        return false;
}

qboolean NET_IsLoopBackAddress (netadr_t *adr)
{
        return adr && adr->type == NA_LOOPBACK;
}

qboolean NET_AddressSmellsFunny(netadr_t *a)
{
        return false;
}

enum addressscope_e NET_ClassifyAddress(netadr_t *adr, const char **outdesc)
{
        if (!adr)
        {
                if (outdesc)
                        *outdesc = "unknown";
                return ASCOPE_PROCESS;
        }
        if (adr->type == NA_LOOPBACK)
        {
                if (outdesc)
                        *outdesc = "loopback";
                return ASCOPE_HOST;
        }
        if (adr->type == NA_IP)
        {
                qbyte first = adr->address.ip[0];
                qbyte second = adr->address.ip[1];
                if (first == 10 || (first == 192 && second == 168) || (first == 172 && second >= 16 && second <= 31))
                {
                        if (outdesc)
                                *outdesc = "lan";
                        return ASCOPE_LAN;
                }
                if (first == 127)
                {
                        if (outdesc)
                                *outdesc = "loopback";
                        return ASCOPE_HOST;
                }
                if (outdesc)
                        *outdesc = "global";
                return ASCOPE_NET;
        }
        if (adr->type == NA_IPV6)
        {
                const qbyte *ip6 = adr->address.ip6;
                if (!memcmp(ip6, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\1", 16))
                {
                        if (outdesc)
                                *outdesc = "loopback";
                        return ASCOPE_HOST;
                }
                if ((ip6[0] & 0xfe) == 0xfc)
                {
                        if (outdesc)
                                *outdesc = "lan";
                        return ASCOPE_LAN;
                }
                if (ip6[0] == 0xfe && (ip6[1] & 0xc0) == 0x80)
                {
                        if (outdesc)
                                *outdesc = "link-local";
                        return ASCOPE_LINK;
                }
                if (outdesc)
                        *outdesc = "global";
                return ASCOPE_NET;
        }
        if (outdesc)
                *outdesc = "unknown";
        return ASCOPE_PROCESS;
}

qboolean NET_EnsureRoute(ftenet_connections_t *collection, char *routename, const struct dtlspeercred_s *peerinfo, const char *adrstring, netadr_t *adr, qboolean outgoing)
{
        return false;
}

void NET_TerminateRoute(ftenet_connections_t *collection, netadr_t *adr)
{
}

void NET_PrintConnectionsStatus(ftenet_connections_t *collection)
{
        if (!collection)
        {
                Con_Printf("%sNo WinRT network sockets active.\n", CON_WARNING);
                return;
        }
        Con_Printf("%sWinRT sockets:\n", CON_INFO);
        for (int i = 0; i < MAX_CONNECTIONS; ++i)
        {
                ftenet_generic_connection_t *con = collection->conn[i];
                if (!con)
                        continue;
                netadr_t addresses[4];
                unsigned int flags = 0;
                int count = con->GetLocalAddresses ? con->GetLocalAddresses(con, &flags, addresses, NULL, (int)(sizeof(addresses)/sizeof(addresses[0]))) : 0;
                if (!count)
                {
                        Con_Printf("  %s (no address info)\n", con->name);
                        continue;
                }
                for (int j = 0; j < count; ++j)
                {
                        char text[64];
                        NET_AdrToString(text, sizeof(text), &addresses[j]);
                        Con_Printf("  %s -> %s\n", con->name, text);
                }
        }
}

int NET_LocalAddressForRemote(ftenet_connections_t *collection, netadr_t *remote, netadr_t *local, int idx)
{
        if (!collection || !local)
                return 0;
        (void)remote;
        (void)idx;
        NET_GetLocalAddress(0, local);
        return (local->type != NA_INVALID);
}

void NET_PrintAddresses(ftenet_connections_t *collection)
{
        if (!collection)
        {
                Con_Printf("No sockets\n");
                return;
        }
        char text[64];
        netadr_t local;
        NET_GetLocalAddress(0, &local);
        NET_AdrToString(text, sizeof(text), &local);
        Con_Printf("Local address: %s\n", text);
}

qboolean NET_WasSpecialPacket(ftenet_connections_t *collection)
{
        return false;
}

void NET_CloseClient(void)
{
        extern client_static_t cls;
        if (!cls.sockets)
                return;
        FTENET_CloseCollection(cls.sockets);
        cls.sockets = NULL;
}

void NET_InitClient(qboolean loopbackonly)
{
        extern client_static_t cls;

        if (!cls.sockets)
                cls.sockets = FTENET_CreateCollection(false, CL_ReadPacket);
        if (cls.sockets)
        {
                FTENET_AddToCollection(cls.sockets, "CLLoopback", "1", NA_LOOPBACK, NP_DGRAM);
                if (!loopbackonly)
                {
                        const char *udpPort = "0";
                        cvar_t *clport = Cvar_FindVar("cl_port");
                        if (clport && clport->string && *clport->string)
                                udpPort = clport->string;
                        if (!FTENET_AddToCollection(cls.sockets, "CLUDP", udpPort, NA_IP, NP_DGRAM))
                                Con_Printf("%sFailed to open WinRT UDP socket for client traffic.\n", CON_WARNING);
                        else
                        {
                                for (int i = 0; i < MAX_CONNECTIONS; ++i)
                                {
                                        ftenet_generic_connection_t *c = cls.sockets->conn[i];
                                        if (!c || strcmp(c->name, "CLUDP"))
                                                continue;
                                        netadr_t local;
                                        unsigned int flags = 0;
                                        if (c->GetLocalAddresses && c->GetLocalAddresses(c, &flags, &local, NULL, 1) > 0)
                                                net_local_cl_ipadr = local;
                                        break;
                                }
                        }
                        char ipv6Port[64];
                        Q_snprintfz(ipv6Port, sizeof(ipv6Port), "[::]:%s", udpPort);
                        if (!FTENET_AddToCollection(cls.sockets, "CLUDP6", ipv6Port, NA_IPV6, NP_DGRAM))
                                Con_DPrintf("%sIPv6 UDP socket unavailable for client traffic.\n", CON_INFO);
                }
        }
        Con_Printf(loopbackonly ? "%sWinRT client running in loopback-only mode.\n" : "%sWinRT client networking ready.\n", CON_INFO);
}

#ifdef HAVE_SERVER
void NET_CloseServer(void)
{
        if (!svs.sockets)
                return;
        FTENET_CloseCollection(svs.sockets);
        svs.sockets = NULL;
}

void NET_InitServer(void)
{
        if (!svs.sockets)
                svs.sockets = FTENET_CreateCollection(true, SV_ReadPacket);
        if (svs.sockets)
        {
                FTENET_AddToCollection(svs.sockets, "SVLoopback", "1", NA_LOOPBACK, NP_DGRAM);
                cvar_t *svport = Cvar_FindVar("sv_port");
                const char *udpPort = (svport && svport->string && *svport->string) ? svport->string : STRINGIFY(PORT_DEFAULTSERVER);
                if (!FTENET_AddToCollection(svs.sockets, "SVUDP", udpPort, NA_IP, NP_DGRAM))
                        Con_Printf("%sFailed to bind WinRT server UDP socket on %s.\n", CON_WARNING, udpPort);
                else
                {
                        char ipv6Port[64];
                        Q_snprintfz(ipv6Port, sizeof(ipv6Port), "[::]:%s", udpPort);
                        if (!FTENET_AddToCollection(svs.sockets, "SVUDP6", ipv6Port, NA_IPV6, NP_DGRAM))
                                Con_DPrintf("%sIPv6 UDP socket unavailable for server on %s.\n", CON_INFO, udpPort);
                }
        }
        Con_Printf("%sWinRT server networking ready.\n", CON_INFO);
}

void SVNET_RegisterCvars(void)
{
        /* networking enabled via WinRT backend */
}
#endif

int TCP_OpenStream(netadr_t *remoteaddr, const char *remotename)
{
        std::wstring host;
        unsigned short port = 0;
        bool wantTls = false;
        if (remoteaddr)
        {
                if (remoteaddr->type == NA_LOOPBACK)
                {
                        host = L"127.0.0.1";
                        port = BigShort(remoteaddr->port);
                }
                else if (remoteaddr->type == NA_IP)
                {
                        host = WinRT_FormatIPv4(remoteaddr->address.ip);
                        port = BigShort(remoteaddr->port);
                }
                else if (remoteaddr->type == NA_IPV6)
                {
                        host = WinRT_FormatIPv6(remoteaddr->address.ip6, remoteaddr->scopeid);
                        port = BigShort(remoteaddr->port);
                }
                if (remoteaddr->prot == NP_TLS || remoteaddr->prot == NP_WSS)
                        wantTls = true;
        }
        if (!port && remoteaddr)
                port = BigShort(remoteaddr->port);
        if (host.empty() && remotename)
        {
                std::string display = remotename;
                size_t scheme = display.find("://");
                if (scheme != std::string::npos)
                        display = display.substr(scheme + 3);
                if (scheme != std::string::npos)
                {
                        std::string prefix = display.substr(0, scheme);
                        std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
                        if (prefix == "https" || prefix == "wss" || prefix == "tls")
                                wantTls = true;
                        display = display.substr(scheme + 3);
                }
                size_t slash = display.find('/');
                if (slash != std::string::npos)
                        display = display.substr(0, slash);
                int parsedPort = port;
                WinRT_SplitHostPort(display, display, parsedPort, parsedPort);
                if (parsedPort > 0 && parsedPort <= 65535)
                        port = static_cast<unsigned short>(parsedPort);
                if (!display.empty())
                        host = WinRT_FromUtf8(display);
        }
        if (port == 0)
        {
                Con_Printf("%sTCP_OpenStream: missing port for %s\n", CON_WARNING, remotename ? remotename : "<unknown>");
                return (int)INVALID_SOCKET;
        }
        if (host.empty())
        {
                Con_Printf("%sTCP_OpenStream: missing host for %s\n", CON_WARNING, remotename ? remotename : "<unknown>");
                return (int)INVALID_SOCKET;
        }
        WinRTStreamHandle *handle = new (std::nothrow) WinRTStreamHandle();
        if (!handle)
                return (int)INVALID_SOCKET;
        try
        {
                handle->socket = StreamSocket();
                handle->socket.Control().NoDelay(true);
                HostName hostName(host);
                hstring service(WinRT_ServiceFromPort(port));
                SocketProtectionLevel level = wantTls ? SocketProtectionLevel::Tls12 : SocketProtectionLevel::PlainSocket;
                handle->socket.ConnectAsync(hostName, service, level).get();
                handle->host = host;
                handle->port = port;
                if (remoteaddr)
                {
                        netadr_t resolved{};
                        try
                        {
                                auto info = handle->socket.Information();
                                if (info)
                                {
                                        if (WinRT_ParseEndpoint(info.RemoteAddress(), info.RemotePort(), resolved))
                                        {
                                                resolved.port = BigShort(port);
                                                resolved.prot = wantTls ? NP_TLS : NP_STREAM;
                                                *remoteaddr = resolved;
                                        }
                                }
                        }
                        catch (const winrt::hresult_error &)
                        {
                        }
                        remoteaddr->port = BigShort(port);
                        remoteaddr->prot = wantTls ? NP_TLS : NP_STREAM;
                        if (remoteaddr->type != NA_IP && remoteaddr->type != NA_IPV6)
                        {
                                std::string utf8 = WinRT_ToUtf8(host);
                                netadr_t literal{};
                                if (WinRT_ParseLiteralIPv4(utf8, port, literal) || WinRT_ParseLiteralIPv6(utf8, port, literal))
                                {
                                        remoteaddr->type = literal.type;
                                        if (literal.type == NA_IP)
                                                memcpy(remoteaddr->address.ip, literal.address.ip, sizeof(literal.address.ip));
                                        else if (literal.type == NA_IPV6)
                                        {
                                                memcpy(remoteaddr->address.ip6, literal.address.ip6, sizeof(literal.address.ip6));
                                                remoteaddr->scopeid = literal.scopeid;
                                        }
                                }
                        }
                }
                return (int)WinRT_StreamHandleToSocket(handle);
        }
        catch (const winrt::hresult_error &err)
        {
                Con_Printf("%sTCP_OpenStream: connect failed for %s (0x%08x)\n", CON_WARNING, remotename ? remotename : "<unknown>", err.code().value);
        }
        catch (...)
        {
        }
        WinRT_StreamDestroy(handle);
        return (int)INVALID_SOCKET;
}

void UDP_CloseSocket (int socket)
{
        (void)socket;
}

qboolean NET_AddrIsReliable(netadr_t *adr)
{
        return adr && adr->type == NA_LOOPBACK;
}

qboolean NET_IsEncrypted(netadr_t *adr)
{
        return adr && (adr->prot == NP_TLS || adr->prot == NP_WSS);
}

int NET_GetConnectionCertificate(struct ftenet_connections_s *col, netadr_t *a, enum certprops_e prop, char *out, size_t outsize)
{
        (void)col; (void)a; (void)prop; (void)out; (void)outsize;
        return 0;
}

const struct urischeme_s *NET_IsURIScheme(const char *possible)
{
        (void)possible;
        return NULL;
}

char *NET_AdrToStringMasked (char *s, int len, netadr_t *a, netadr_t *amask)
{
        return NET_AdrToString(s, len, a);
}

void NET_IntegerToMask (netadr_t *a, netadr_t *amask, int bits)
{
        (void)a; (void)amask; (void)bits;
}

qboolean NET_StringToAdrMasked (const char *s, qboolean allowdns, netadr_t *a, netadr_t *amask)
{
        (void)allowdns; (void)amask;
        return NET_StringToAdr_NoDNS(s, 0, a);
}

qboolean NET_CompareAdrMasked(netadr_t *a, netadr_t *b, netadr_t *mask)
{
        (void)mask;
        return NET_CompareAdr(a, b);
}

qboolean NET_IsClientLegal(netadr_t *adr)
{
        return adr && (adr->type == NA_LOOPBACK || adr->type == NA_IP || adr->type == NA_IPV6
#ifdef HAVE_WEBSOCKCL
                || adr->type == NA_WEBSOCKET
#endif
        );
}

void NET_AdrToStringResolve (netadr_t *adr, void (*resolved)(void *ctx, void *data, size_t a, size_t b), void *ctx, size_t a, size_t b)
{
        char buffer[32];
        NET_AdrToString(buffer, sizeof(buffer), adr);
        if (resolved)
                resolved(ctx, buffer, strlen(buffer)+1, 0);
}

size_t NET_StringToSockaddr2 (const char *s, int defaultport, netadrtype_t afhint, struct sockaddr_qstorage *sadr, int *addrfamily, int *addrsize, size_t addresses)
{
        if (!s || !sadr || !addresses)
                return 0;
        std::string input = s;
        int port = defaultport;
        WinRT_SplitHostPort(input, input, port, defaultport);
        netadr_t adr{};
        bool parsed = false;
        if (!strcmp(input.c_str(), "loopback"))
        {
                adr.type = NA_LOOPBACK;
                adr.port = BigShort((unsigned short)port);
                adr.prot = NP_DGRAM;
                adr.address.ip[0] = 127;
                adr.address.ip[3] = 1;
                parsed = true;
        }
        if (!parsed)
        {
                if (afhint == NA_IP)
                        parsed = WinRT_ParseLiteralIPv4(input, port, adr);
                else if (afhint == NA_IPV6)
                        parsed = WinRT_ParseLiteralIPv6(input, port, adr);
                else
                        parsed = WinRT_ParseLiteralIPv4(input, port, adr) || WinRT_ParseLiteralIPv6(input, port, adr);
        }
        if (!parsed)
                return 0;

        memset(sadr, 0, sizeof(*sadr));
        if (adr.type == NA_IP || adr.type == NA_LOOPBACK)
        {
                sockaddr_in *sa = reinterpret_cast<sockaddr_in *>(sadr);
                sa->sin_family = AF_INET;
                sa->sin_port = adr.port;
                memcpy(&sa->sin_addr, adr.address.ip, sizeof(adr.address.ip));
                if (addrfamily)
                        *addrfamily = AF_INET;
                if (addrsize)
                        *addrsize = sizeof(sockaddr_in);
                return 1;
        }
        if (adr.type == NA_IPV6)
        {
                sockaddr_in6 *sa6 = reinterpret_cast<sockaddr_in6 *>(sadr);
                sa6->sin6_family = AF_INET6;
                sa6->sin6_port = adr.port;
                sa6->sin6_scope_id = adr.scopeid;
                memcpy(&sa6->sin6_addr, adr.address.ip6, sizeof(adr.address.ip6));
                if (addrfamily)
                        *addrfamily = AF_INET6;
                if (addrsize)
                        *addrsize = sizeof(sockaddr_in6);
                return 1;
        }
        return 0;
}

int NET_EnumerateAddresses(ftenet_connections_t *collection, struct ftenet_generic_connection_s **con, unsigned int *adrflags, netadr_t *addresses, const char **adrparams, int maxaddresses)
{
        if (!collection || maxaddresses <= 0)
                return 0;
        int total = 0;
        for (int i = 0; i < MAX_CONNECTIONS && total < maxaddresses; ++i)
        {
                ftenet_generic_connection_t *c = collection->conn[i];
                if (!c || !c->GetLocalAddresses)
                        continue;
                unsigned int flags = 0;
                int count = c->GetLocalAddresses(c, &flags, addresses ? addresses + total : NULL, adrparams, maxaddresses - total);
                if (count <= 0)
                        continue;
                if (adrflags)
                        *adrflags = flags;
                if (con)
                        *con = c;
                total += count;
        }
        return total;
}

qboolean NET_Sleep(float seconds, qboolean stdinissocket)
{
        (void)seconds; (void)stdinissocket;
        return false;
}

#endif /* WINRT */
