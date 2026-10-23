// Declaration-only headers for tools/check_windows_syntax.sh on non-Windows hosts.
// g++ checks source structure and UIA overrides, not Win32 API signatures or ABI layouts.
// These headers cannot build the app. Variadic API declarations only support syntax checks.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <ctime>
#include <type_traits>

#define WINAPI
#define CALLBACK
#define APIENTRY
#define STDMETHODCALLTYPE
#define NETIOAPI_API_
#define __stdcall
#define __declspec(x)
#define _In_
#define _Out_
#define _In_opt_
#define _Out_opt_
#define _Inout_
#define IFACEMETHODIMP HRESULT
#define IFACEMETHODIMP_(type) type
#define STDMETHODIMP HRESULT
#define STDMETHODIMP_(type) type
#define WINMOCK_FN(R, name) template <class... A> inline R name(A&&...) { return ::winmock::ret<R>(); }
namespace winmock {
template <class R> inline R ret() { if constexpr (!std::is_void_v<R>) return R{}; }
}

using BOOL = int; using BOOLEAN = unsigned char; using BYTE = unsigned char; using UCHAR = unsigned char; using UINT8 = unsigned char;
using WORD = unsigned short; using USHORT = unsigned short; using ATOM = WORD; using SHORT = short; using VARTYPE = unsigned short;
using DWORD = unsigned long; using ULONG = unsigned long; using LONG = long; using INT = int; using UINT = unsigned int;
using UINT32 = std::uint32_t; using INT32 = std::int32_t; using UINT64 = std::uint64_t; using ULONG64 = std::uint64_t; using DWORD64 = std::uint64_t;
using LONG_PTR = std::intptr_t; using ULONG_PTR = std::uintptr_t; using INT_PTR = std::intptr_t; using UINT_PTR = std::uintptr_t; using DWORD_PTR = ULONG_PTR;
using SIZE_T = std::size_t; using WPARAM = UINT_PTR; using LPARAM = LONG_PTR; using LRESULT = LONG_PTR; using HRESULT = long; using LSTATUS = long;
using WCHAR = wchar_t; using LPWSTR = wchar_t*; using PWSTR = wchar_t*; using LPCWSTR = const wchar_t*; using PCWSTR = const wchar_t*;
using LPSTR = char*; using LPCSTR = const char*; using PSTR = char*; using PCSTR = const char*; using CHAR = char;
using PVOID = void*; using LPVOID = void*; using LPCVOID = const void*; using LPBYTE = BYTE*; using LPDWORD = DWORD*; using PULONG = ULONG*;
using PUCHAR = UCHAR*;
using HANDLE = void*; using HINTERNET = void*; using COLORREF = DWORD; using BSTR = wchar_t*; using VARIANT_BOOL = short; using DOUBLE = double;
using FLOAT = float; using NET_IFINDEX = ULONG; using ADDRESS_FAMILY = USHORT; using REGSAM = DWORD; using INTERNET_PORT = WORD;
struct HWND__ {}; using HWND = HWND__*; struct HINSTANCE__ {}; using HINSTANCE = HINSTANCE__*; using HMODULE = HINSTANCE;
struct HICON__ {}; using HICON = HICON__*; using HCURSOR = HICON; struct HMENU__ {}; using HMENU = HMENU__*;
struct HGDIOBJ__ {}; using HGDIOBJ = HGDIOBJ__*; struct HFONT__ : HGDIOBJ__ {}; using HFONT = HFONT__*; struct HBRUSH__ : HGDIOBJ__ {}; using HBRUSH = HBRUSH__*;
struct HDC__ {}; using HDC = HDC__*; struct HKEY__ {}; using HKEY = HKEY__*; using PHKEY = HKEY*; using HGLOBAL = void*; using HLOCAL = void*;
struct HPOWERNOTIFY__ {}; using HPOWERNOTIFY = HPOWERNOTIFY__*; struct HMONITOR__ {}; using HMONITOR = HMONITOR__*;
struct DPI_AWARENESS_CONTEXT__ {}; using DPI_AWARENESS_CONTEXT = DPI_AWARENESS_CONTEXT__*;

namespace winmock {
struct IID { unsigned long d1 = 0; bool operator==(const IID&) const { return true; } };
}
using IID = winmock::IID; using GUID = winmock::IID; using CLSID = winmock::IID; using KNOWNFOLDERID = winmock::IID;
using REFIID = const IID&; using REFCLSID = const CLSID&; using REFGUID = const GUID&; using REFKNOWNFOLDERID = const KNOWNFOLDERID&;
#define __uuidof(x) ::winmock::IID{}
#define IID_PPV_ARGS(pp) ::winmock::IID{}, reinterpret_cast<void**>(pp)
inline const IID IID_IUnknown{}; inline const CLSID CLSID_FileSaveDialog{}; inline const CLSID CLSID_AccPropServices{};
inline const KNOWNFOLDERID FOLDERID_LocalAppData{}; inline const GUID PROPID_ACC_NAME{};

// ---- MSVC CRT secure functions (MSVC parameter order) ----
using errno_t = int;
constexpr std::size_t _TRUNCATE = static_cast<std::size_t>(-1);
inline errno_t localtime_s(std::tm* out, const std::time_t* time) { (void)out; (void)time; return 0; }
inline errno_t gmtime_s(std::tm* out, const std::time_t* time) { (void)out; (void)time; return 0; }
inline errno_t wcsncpy_s(wchar_t* dest, std::size_t size, const wchar_t* src, std::size_t count) { (void)dest; (void)size; (void)src; (void)count; return 0; }
template <std::size_t N> inline errno_t wcsncpy_s(wchar_t (&dest)[N], const wchar_t* src, std::size_t count) { (void)dest; (void)src; (void)count; return 0; }
inline errno_t wcscpy_s(wchar_t* dest, std::size_t size, const wchar_t* src) { (void)dest; (void)size; (void)src; return 0; }
template <std::size_t N> inline errno_t wcscpy_s(wchar_t (&dest)[N], const wchar_t* src) { (void)dest; (void)src; return 0; }
inline errno_t memcpy_s(void* dest, std::size_t size, const void* src, std::size_t count) { (void)dest; (void)size; (void)src; (void)count; return 0; }
inline int _wcsicmp(const wchar_t* a, const wchar_t* b) { (void)a; (void)b; return 0; }
inline int _wcsnicmp(const wchar_t* a, const wchar_t* b, std::size_t n) { (void)a; (void)b; (void)n; return 0; }
inline PVOID SecureZeroMemory(PVOID ptr, SIZE_T count) { (void)count; return ptr; }
WINMOCK_FN(std::size_t, wcsnlen_s)
WINMOCK_FN(int, swscanf_s)

constexpr BOOL TRUE = 1, FALSE = 0;
constexpr HRESULT S_OK = 0, S_FALSE = 1, E_FAIL = static_cast<HRESULT>(0x80004005L), E_NOINTERFACE = static_cast<HRESULT>(0x80004002L),
                  E_POINTER = static_cast<HRESULT>(0x80004003L), E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000EL),
                  E_INVALIDARG = static_cast<HRESULT>(0x80070057L), E_NOTIMPL = static_cast<HRESULT>(0x80004001L),
                  E_ABORT = static_cast<HRESULT>(0x80004004L), E_ACCESSDENIED = static_cast<HRESULT>(0x80070005L),
                  D2DERR_RECREATE_TARGET = static_cast<HRESULT>(0x8899000CL),
                  UIA_E_ELEMENTNOTAVAILABLE = static_cast<HRESULT>(0x80040201L), UIA_E_ELEMENTNOTENABLED = static_cast<HRESULT>(0x80040200L),
                  UIA_E_INVALIDOPERATION = static_cast<HRESULT>(0x80131509L);
inline HRESULT HRESULT_FROM_WIN32(unsigned long x) { return static_cast<HRESULT>(x); }
#define FAILED(hr) (static_cast<HRESULT>(hr) < 0)
#define SUCCEEDED(hr) (static_cast<HRESULT>(hr) >= 0)
constexpr DWORD ERROR_SUCCESS = 0, NO_ERROR = 0, ERROR_FILE_NOT_FOUND = 2, ERROR_PATH_NOT_FOUND = 3, ERROR_ACCESS_DENIED = 5, ERROR_INVALID_PARAMETER = 87,
                ERROR_INSUFFICIENT_BUFFER = 122, ERROR_MORE_DATA = 234, ERROR_ALREADY_EXISTS = 183, ERROR_NOT_FOUND = 1168,
                ERROR_BUFFER_OVERFLOW = 111, ERROR_NO_DATA = 232, ERROR_CANCELLED = 1223, ERROR_OPERATION_ABORTED = 995,
                ERROR_WINHTTP_TIMEOUT = 12002, ERROR_WINHTTP_NAME_NOT_RESOLVED = 12007, ERROR_WINHTTP_OPERATION_CANCELLED = 12017,
                ERROR_WINHTTP_CANNOT_CONNECT = 12029, ERROR_WINHTTP_CONNECTION_ERROR = 12030, ERROR_WINHTTP_REDIRECT_FAILED = 12156,
                ERROR_WINHTTP_SECURE_FAILURE = 12175, ERROR_WINHTTP_SECURE_CERT_DATE_INVALID = 12037, ERROR_WINHTTP_SECURE_CERT_CN_INVALID = 12038,
                ERROR_WINHTTP_SECURE_INVALID_CA = 12045, ERROR_WINHTTP_SECURE_CERT_REV_FAILED = 12057, ERROR_WINHTTP_SECURE_CHANNEL_ERROR = 12157,
                ERROR_WINHTTP_SECURE_INVALID_CERT = 12169, ERROR_WINHTTP_SECURE_CERT_REVOKED = 12170, ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE = 12179,
                ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED = 12044, ERROR_WINHTTP_HEADER_NOT_FOUND = 12150, ERROR_WINHTTP_INVALID_URL = 12005,
                ERROR_WINHTTP_UNRECOGNIZED_SCHEME = 12006, WAIT_OBJECT_0 = 0, WAIT_TIMEOUT = 258, WAIT_FAILED = 0xFFFFFFFF, INFINITE = 0xFFFFFFFF;
inline const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1));
constexpr DWORD ERROR_FILE_EXISTS = 80, ERROR_NO_MORE_ITEMS = 259, ERROR_TIMEOUT = 1460;
constexpr int CSTR_EQUAL = 2;
WINMOCK_FN(int, CompareStringOrdinal)

struct RECT { LONG left = 0, top = 0, right = 0, bottom = 0; }; using LPRECT = RECT*;
struct POINT { LONG x = 0, y = 0; }; using LPPOINT = POINT*;
struct SIZE { LONG cx = 0, cy = 0; };
struct MSG { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; };
struct PAINTSTRUCT { HDC hdc; BOOL fErase; RECT rcPaint; BOOL fRestore; BOOL fIncUpdate; BYTE rgbReserved[32]; };
struct MINMAXINFO { POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize; };
struct CREATESTRUCTW { LPVOID lpCreateParams; HINSTANCE hInstance; HMENU hMenu; HWND hwndParent; int cy, cx, y, x; LONG style; LPCWSTR lpszName, lpszClass; DWORD dwExStyle; };
using WNDPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
using SUBCLASSPROC = LRESULT (*)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
struct WNDCLASSEXW { UINT cbSize, style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; HBRUSH hbrBackground; LPCWSTR lpszMenuName, lpszClassName; HICON hIconSm; };
struct TRACKMOUSEEVENT { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; };
struct NOTIFYICONDATAW { DWORD cbSize; HWND hWnd; UINT uID, uFlags, uCallbackMessage; HICON hIcon; WCHAR szTip[128]; DWORD dwState, dwStateMask; WCHAR szInfo[256];
                         union { UINT uTimeout; UINT uVersion; }; WCHAR szInfoTitle[64]; DWORD dwInfoFlags; GUID guidItem; HICON hBalloonIcon; };
struct HIGHCONTRASTW { UINT cbSize; DWORD dwFlags; LPWSTR lpszDefaultScheme; };
struct INITCOMMONCONTROLSEX { DWORD dwSize, dwICC; };
struct SYSTEMTIME { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; };
struct FILETIME { DWORD dwLowDateTime, dwHighDateTime; };
WINMOCK_FN(BOOL, SystemTimeToFileTime)
union LARGE_INTEGER { struct { DWORD LowPart; LONG HighPart; } u; long long QuadPart; };
struct SECURITY_ATTRIBUTES { DWORD nLength; LPVOID lpSecurityDescriptor; BOOL bInheritHandle; };
struct CREDENTIALW { DWORD Flags, Type; LPWSTR TargetName, Comment; FILETIME LastWritten; DWORD CredentialBlobSize; LPBYTE CredentialBlob;
                     DWORD Persist, AttributeCount; void* Attributes; LPWSTR TargetAlias, UserName; };
using PCREDENTIALW = CREDENTIALW*;
struct WINHTTP_CURRENT_USER_IE_PROXY_CONFIG { BOOL fAutoDetect; LPWSTR lpszAutoConfigUrl, lpszProxy, lpszProxyBypass; };
struct COMDLG_FILTERSPEC { LPCWSTR pszName, pszSpec; };

// ---- sockets / IP helper ----
struct IN_ADDR { union { struct { UCHAR s_b1, s_b2, s_b3, s_b4; } S_un_b; ULONG S_addr; } S_un; };
struct IN6_ADDR { union { UCHAR Byte[16]; USHORT Word[8]; } u; };
struct SOCKADDR { ADDRESS_FAMILY sa_family; char sa_data[14]; }; using LPSOCKADDR = SOCKADDR*;
using sockaddr = SOCKADDR;
struct SOCKADDR_IN { ADDRESS_FAMILY sin_family; USHORT sin_port; IN_ADDR sin_addr; char sin_zero[8]; }; using sockaddr_in = SOCKADDR_IN;
struct SOCKADDR_IN6 { ADDRESS_FAMILY sin6_family; USHORT sin6_port; ULONG sin6_flowinfo; IN6_ADDR sin6_addr; ULONG sin6_scope_id; }; using sockaddr_in6 = SOCKADDR_IN6;
union SOCKADDR_INET { SOCKADDR_IN Ipv4; SOCKADDR_IN6 Ipv6; ADDRESS_FAMILY si_family; };
struct SOCKET_ADDRESS { LPSOCKADDR lpSockaddr; INT iSockaddrLength; };
union NET_LUID { ULONG64 Value; struct { ULONG64 Reserved : 24, NetLuidIndex : 24, IfType : 16; } Info; };
struct IP_ADDRESS_PREFIX { SOCKADDR_INET Prefix; UINT8 PrefixLength; };
enum NL_ROUTE_PROTOCOL { MIB_IPPROTO_OTHER = 1, MIB_IPPROTO_LOCAL = 2, MIB_IPPROTO_NETMGMT = 3 };
enum NL_ROUTE_ORIGIN { NlroManual, NlroWellKnown, NlroDHCP, NlroRouterAdvertisement, Nlro6to4 };
struct MIB_IPFORWARD_ROW2 { NET_LUID InterfaceLuid; NET_IFINDEX InterfaceIndex; IP_ADDRESS_PREFIX DestinationPrefix; SOCKADDR_INET NextHop;
                            UCHAR SitePrefixLength; ULONG ValidLifetime, PreferredLifetime, Metric; NL_ROUTE_PROTOCOL Protocol;
                            BOOLEAN Loopback, AutoconfigureAddress, Publish, Immortal; ULONG Age; NL_ROUTE_ORIGIN Origin; };
using PMIB_IPFORWARD_ROW2 = MIB_IPFORWARD_ROW2*;
struct MIB_IPFORWARD_TABLE2 { ULONG NumEntries; MIB_IPFORWARD_ROW2 Table[1]; }; using PMIB_IPFORWARD_TABLE2 = MIB_IPFORWARD_TABLE2*;
struct MIB_IPINTERFACE_ROW { ADDRESS_FAMILY Family; NET_LUID InterfaceLuid; NET_IFINDEX InterfaceIndex; ULONG Metric; }; using PMIB_IPINTERFACE_ROW = MIB_IPINTERFACE_ROW*;
enum MIB_NOTIFICATION_TYPE { MibParameterNotification, MibAddInstance, MibDeleteInstance, MibInitialNotification };
enum IF_OPER_STATUS { IfOperStatusUp = 1, IfOperStatusDown, IfOperStatusTesting, IfOperStatusUnknown, IfOperStatusDormant, IfOperStatusNotPresent, IfOperStatusLowerLayerDown };
enum TUNNEL_TYPE { TUNNEL_TYPE_NONE = 0, TUNNEL_TYPE_OTHER = 1, TUNNEL_TYPE_DIRECT = 2, TUNNEL_TYPE_6TO4 = 11, TUNNEL_TYPE_ISATAP = 13, TUNNEL_TYPE_TEREDO = 14, TUNNEL_TYPE_IPHTTPS = 15 };
enum NL_DAD_STATE { IpDadStateInvalid = 0, IpDadStateTentative, IpDadStateDuplicate, IpDadStateDeprecated, IpDadStatePreferred };
struct IP_ADAPTER_UNICAST_ADDRESS { ULONG Length; DWORD Flags; IP_ADAPTER_UNICAST_ADDRESS* Next; SOCKET_ADDRESS Address;
    NL_DAD_STATE DadState; ULONG ValidLifetime, PreferredLifetime, LeaseLifetime; UINT8 OnLinkPrefixLength; };
struct IP_ADAPTER_GATEWAY_ADDRESS { ULONG Length; DWORD Reserved; IP_ADAPTER_GATEWAY_ADDRESS* Next; SOCKET_ADDRESS Address; };
struct IP_ADAPTER_DNS_SERVER_ADDRESS { ULONG Length; DWORD Reserved; IP_ADAPTER_DNS_SERVER_ADDRESS* Next; SOCKET_ADDRESS Address; };
struct IP_ADAPTER_ADDRESSES { ULONG Length; DWORD IfIndex; IP_ADAPTER_ADDRESSES* Next; char* AdapterName; IP_ADAPTER_UNICAST_ADDRESS* FirstUnicastAddress;
    void* FirstAnycastAddress; void* FirstMulticastAddress; IP_ADAPTER_DNS_SERVER_ADDRESS* FirstDnsServerAddress; wchar_t* DnsSuffix; wchar_t* Description;
    wchar_t* FriendlyName; BYTE PhysicalAddress[8]; ULONG PhysicalAddressLength; ULONG Flags; ULONG Mtu; DWORD IfType; IF_OPER_STATUS OperStatus;
    ULONG Ipv6IfIndex; ULONG ZoneIndices[16]; void* FirstPrefix; ULONG64 TransmitLinkSpeed, ReceiveLinkSpeed; void* FirstWinsServerAddress;
    IP_ADAPTER_GATEWAY_ADDRESS* FirstGatewayAddress; ULONG Ipv4Metric, Ipv6Metric; NET_LUID Luid; SOCKET_ADDRESS Dhcpv4Server; ULONG CompartmentId;
    GUID NetworkGuid; int ConnectionType; TUNNEL_TYPE TunnelType; };
using PIP_ADAPTER_ADDRESSES = IP_ADAPTER_ADDRESSES*;
enum NL_NEIGHBOR_STATE { NlnsUnreachable = 0, NlnsIncomplete, NlnsProbe, NlnsDelay, NlnsStale, NlnsReachable, NlnsPermanent, NlnsMaximum };
struct MIB_IPNET_ROW2 { SOCKADDR_INET Address; NET_IFINDEX InterfaceIndex; NET_LUID InterfaceLuid; UCHAR PhysicalAddress[32]; ULONG PhysicalAddressLength; NL_NEIGHBOR_STATE State; };
struct MIB_IPNET_TABLE2 { ULONG NumEntries; MIB_IPNET_ROW2 Table[1]; }; using PMIB_IPNET_TABLE2 = MIB_IPNET_TABLE2*;
constexpr ULONG BCRYPT_USE_SYSTEM_PREFERRED_RNG = 2;
constexpr ADDRESS_FAMILY AF_UNSPEC = 0, AF_INET = 2, AF_INET6 = 23;
constexpr ULONG GAA_FLAG_SKIP_ANYCAST = 0x2, GAA_FLAG_SKIP_MULTICAST = 0x4, GAA_FLAG_SKIP_DNS_SERVER = 0x8, GAA_FLAG_INCLUDE_GATEWAYS = 0x80, GAA_FLAG_SKIP_FRIENDLY_NAME = 0x20;
constexpr DWORD IF_TYPE_OTHER = 1, IF_TYPE_ETHERNET_CSMACD = 6, IF_TYPE_PPP = 23, IF_TYPE_SOFTWARE_LOOPBACK = 24, IF_TYPE_PROP_VIRTUAL = 53, IF_TYPE_TUNNEL = 131, IF_TYPE_IEEE80211 = 71;
constexpr int INET_ADDRSTRLEN = 22, INET6_ADDRSTRLEN = 65;
inline bool IN6_IS_ADDR_UNSPECIFIED(const IN6_ADDR*) { return false; }
bool IN6_IS_ADDR_LOOPBACK(const IN6_ADDR*);
bool IN6_IS_ADDR_LINKLOCAL(const IN6_ADDR*);
bool IN6_IS_ADDR_MULTICAST(const IN6_ADDR*);
bool IN6_IS_ADDR_V4MAPPED(const IN6_ADDR*);

// Partial member declarations, not ABI layouts. New API prototypes preserve the
// documented parameter types; this header remains syntax-only and cannot prove
// the actual Windows SDK declarations or any networking behaviour.
using SOCKET = UINT_PTR;
using u_long = unsigned long;
using u_short = unsigned short;
constexpr SOCKET INVALID_SOCKET = static_cast<SOCKET>(~SOCKET(0));
constexpr int SOCKET_ERROR = -1;
constexpr int IPPROTO_IP = 0, IPPROTO_TCP = 6, IPPROTO_UDP = 17, SOCK_STREAM = 1, SOCK_DGRAM = 2;
constexpr int IP_UNICAST_IF = 31;
constexpr LONG FIONBIO = static_cast<LONG>(0x8004667eUL);
constexpr int WSAEINTR = 10004, WSAEWOULDBLOCK = 10035;
constexpr SHORT POLLRDNORM = 0x0100, POLLERR = 0x0001, POLLHUP = 0x0002, POLLNVAL = 0x0004;
struct WSADATA { WORD wVersion, wHighVersion; char szDescription[257], szSystemStatus[129];
    USHORT iMaxSockets, iMaxUdpDg; char* lpVendorInfo; };
using LPWSADATA = WSADATA*;
struct WSAPOLLFD { SOCKET fd; SHORT events, revents; }; using LPWSAPOLLFD = WSAPOLLFD*;
constexpr WORD MAKEWORD(BYTE a, BYTE b) { return static_cast<WORD>(a | (static_cast<WORD>(b) << 8)); }
int WSAStartup(WORD, LPWSADATA);
int WSACleanup();
int WSAGetLastError();
SOCKET socket(int, int, int);
int closesocket(SOCKET);
int setsockopt(SOCKET, int, int, const char*, int);
int getsockopt(SOCKET, int, int, char*, int*);
int ioctlsocket(SOCKET, long, u_long*);
int bind(SOCKET, const sockaddr*, int);
int connect(SOCKET, const sockaddr*, int);
int getsockname(SOCKET, sockaddr*, int*);
int getpeername(SOCKET, sockaddr*, int*);
int send(SOCKET, const char*, int, int);
int recv(SOCKET, char*, int, int);
int WSAPoll(LPWSAPOLLFD, ULONG, INT);
#ifndef htonl
u_long htonl(u_long);
#endif
#ifndef ntohl
u_long ntohl(u_long);
#endif
#ifndef htons
u_short htons(u_short);
#endif
#ifndef ntohs
u_short ntohs(u_short);
#endif
int InetPtonA(INT, PCSTR, PVOID);

enum NET_IF_MEDIA_CONNECT_STATE { MediaConnectStateUnknown = 0, MediaConnectStateConnected, MediaConnectStateDisconnected };
enum NET_IF_ACCESS_TYPE { NET_IF_ACCESS_LOOPBACK = 1, NET_IF_ACCESS_BROADCAST, NET_IF_ACCESS_POINT_TO_POINT,
    NET_IF_ACCESS_POINT_TO_MULTI_POINT, NET_IF_ACCESS_MAXIMUM };
enum NDIS_MEDIUM { NdisMedium802_3 = 0, NdisMediumTunnel = 15, NdisMediumNative802_11 = 16, NdisMediumLoopback = 17 };
struct MIB_IF_ROW2 {
    NET_LUID InterfaceLuid; NET_IFINDEX InterfaceIndex;
    struct { BOOLEAN HardwareInterface : 1, FilterInterface : 1, ConnectorPresent : 1, NotAuthenticated : 1,
        NotMediaConnected : 1, Paused : 1, LowPower : 1, EndPointInterface : 1; } InterfaceAndOperStatusFlags;
    IF_OPER_STATUS OperStatus; NET_IF_MEDIA_CONNECT_STATE MediaConnectState; NET_IF_ACCESS_TYPE AccessType; NDIS_MEDIUM MediaType;
};
using PMIB_IF_ROW2 = MIB_IF_ROW2*;
struct MIB_UNICASTIPADDRESS_ROW { SOCKADDR_INET Address; NET_LUID InterfaceLuid; NET_IFINDEX InterfaceIndex;
    ULONG ValidLifetime, PreferredLifetime; UINT8 OnLinkPrefixLength; BOOLEAN SkipAsSource; NL_DAD_STATE DadState; };
using PMIB_UNICASTIPADDRESS_ROW = MIB_UNICASTIPADDRESS_ROW*;
struct MIB_UNICASTIPADDRESS_TABLE { ULONG NumEntries; MIB_UNICASTIPADDRESS_ROW Table[1]; };
using PMIB_UNICASTIPADDRESS_TABLE = MIB_UNICASTIPADDRESS_TABLE*;
using NETIO_STATUS = ULONG;
NETIO_STATUS GetIfEntry2(PMIB_IF_ROW2);
void InitializeIpInterfaceEntry(PMIB_IPINTERFACE_ROW);
NETIO_STATUS GetIpInterfaceEntry(PMIB_IPINTERFACE_ROW);
void InitializeUnicastIpAddressEntry(PMIB_UNICASTIPADDRESS_ROW);
NETIO_STATUS GetUnicastIpAddressEntry(PMIB_UNICASTIPADDRESS_ROW);
NETIO_STATUS GetUnicastIpAddressTable(ADDRESS_FAMILY, PMIB_UNICASTIPADDRESS_TABLE*);
NETIO_STATUS ConvertInterfaceLuidToIndex(const NET_LUID*, NET_IFINDEX*);
NETIO_STATUS GetBestRoute2(const NET_LUID*, NET_IFINDEX, const SOCKADDR_INET*, const SOCKADDR_INET*, ULONG,
                          PMIB_IPFORWARD_ROW2, SOCKADDR_INET*);
using BCRYPT_ALG_HANDLE = PVOID;
LONG BCryptGenRandom(BCRYPT_ALG_HANDLE, PUCHAR, ULONG, ULONG);
WINMOCK_FN(ULONG, GetAdaptersAddresses) WINMOCK_FN(DWORD, GetIpForwardTable2) WINMOCK_FN(DWORD, GetIpNetTable2) WINMOCK_FN(void, FreeMibTable) WINMOCK_FN(DWORD, NotifyIpInterfaceChange)
WINMOCK_FN(DWORD, NotifyRouteChange2) WINMOCK_FN(DWORD, CancelMibChangeNotify2) WINMOCK_FN(PCSTR, InetNtopA) WINMOCK_FN(PCWSTR, InetNtopW)

// ---- kernel / user ----
constexpr UINT WM_NULL = 0x0000, WM_CREATE = 0x0001, WM_DESTROY = 0x0002, WM_MOVE = 0x0003, WM_SIZE = 0x0005, WM_ACTIVATE = 0x0006, WM_SETFOCUS = 0x0007,
               WM_KILLFOCUS = 0x0008, WM_ENABLE = 0x000A, WM_SETTEXT = 0x000C, WM_PAINT = 0x000F, WM_CLOSE = 0x0010, WM_QUERYENDSESSION = 0x0011,
               WM_QUIT = 0x0012, WM_ERASEBKGND = 0x0014, WM_SYSCOLORCHANGE = 0x0015, WM_ENDSESSION = 0x0016, WM_SHOWWINDOW = 0x0018,
               WM_SETTINGCHANGE = 0x001A, WM_SETCURSOR = 0x0020, WM_GETMINMAXINFO = 0x0024, WM_SETFONT = 0x0030, WM_GETOBJECT = 0x003D,
               WM_WINDOWPOSCHANGED = 0x0047, WM_CONTEXTMENU = 0x007B, WM_NCCREATE = 0x0081, WM_NCDESTROY = 0x0082, WM_NCHITTEST = 0x0084,
               WM_KEYDOWN = 0x0100, WM_KEYUP = 0x0101, WM_CHAR = 0x0102, WM_SYSKEYDOWN = 0x0104, WM_COMMAND = 0x0111, WM_SYSCOMMAND = 0x0112,
               WM_TIMER = 0x0113, WM_CTLCOLOREDIT = 0x0133, WM_CTLCOLORSTATIC = 0x0138, WM_MOUSEMOVE = 0x0200, WM_LBUTTONDOWN = 0x0201,
               WM_LBUTTONUP = 0x0202, WM_LBUTTONDBLCLK = 0x0203, WM_RBUTTONDOWN = 0x0204, WM_RBUTTONUP = 0x0205, WM_MOUSEWHEEL = 0x020A,
               WM_POWERBROADCAST = 0x0218, WM_MOUSELEAVE = 0x02A3, WM_DPICHANGED = 0x02E0, WM_THEMECHANGED = 0x031A, WM_USER = 0x0400, WM_APP = 0x8000;
constexpr UINT EM_SETLIMITTEXT = 0x00C5, EM_SETPASSWORDCHAR = 0x00CC, EM_GETPASSWORDCHAR = 0x00D2, EM_SETSEL = 0x00B1, EM_SETCUEBANNER = 0x1501, EN_CHANGE = 0x0300, EN_SETFOCUS = 0x0100, EN_KILLFOCUS = 0x0200;
constexpr int VK_BACK = 0x08, VK_TAB = 0x09, VK_RETURN = 0x0D, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_ESCAPE = 0x1B, VK_SPACE = 0x20,
              VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26, VK_RIGHT = 0x27, VK_DOWN = 0x28,
              VK_DELETE = 0x2E, VK_F5 = 0x74, VK_APPS = 0x5D;
constexpr DWORD WS_OVERLAPPED = 0, WS_POPUP = 0x80000000, WS_CHILD = 0x40000000, WS_VISIBLE = 0x10000000, WS_CLIPCHILDREN = 0x02000000,
                WS_CLIPSIBLINGS = 0x04000000, WS_BORDER = 0x00800000, WS_TABSTOP = 0x00010000, WS_OVERLAPPEDWINDOW = 0x00CF0000, WS_EX_CLIENTEDGE = 0x200,
                WS_EX_TOOLWINDOW = 0x80, WS_EX_NOREDIRECTIONBITMAP = 0x00200000, ES_AUTOHSCROLL = 0x80, ES_PASSWORD = 0x20, ES_LEFT = 0;
constexpr UINT CS_HREDRAW = 2, CS_VREDRAW = 1, CS_DBLCLKS = 8;
constexpr int CW_USEDEFAULT = static_cast<int>(0x80000000), SW_HIDE = 0, SW_SHOWNORMAL = 1, SW_SHOW = 5, SW_MINIMIZE = 6, SW_RESTORE = 9, SW_SHOWDEFAULT = 10;
constexpr UINT SWP_NOSIZE = 1, SWP_NOMOVE = 2, SWP_NOZORDER = 4, SWP_NOACTIVATE = 0x10, SWP_SHOWWINDOW = 0x40, SWP_HIDEWINDOW = 0x80, SWP_FRAMECHANGED = 0x20;
inline const HWND HWND_TOP = nullptr; inline const HWND HWND_MESSAGE = reinterpret_cast<HWND>(static_cast<LONG_PTR>(-3));
constexpr int GWLP_USERDATA = -21, GWLP_WNDPROC = -4, SM_CXSMICON = 49, SM_CYSMICON = 50, SM_CXICON = 11, SM_CYICON = 12, SM_MENUDROPALIGNMENT = 40, SM_CXSCREEN = 0, SM_CYSCREEN = 1;
constexpr int COLOR_WINDOW = 5, COLOR_WINDOWTEXT = 8, COLOR_HIGHLIGHT = 13, COLOR_HIGHLIGHTTEXT = 14, COLOR_GRAYTEXT = 17, COLOR_BTNFACE = 15, COLOR_HOTLIGHT = 26, COLOR_3DFACE = 15;
constexpr UINT SPI_GETHIGHCONTRAST = 0x0042, SPI_GETCLIENTAREAANIMATION = 0x1042, SPI_GETNONCLIENTMETRICS = 0x0029; constexpr DWORD HCF_HIGHCONTRASTON = 1;
constexpr UINT MF_STRING = 0, MF_GRAYED = 1, MF_DISABLED = 2, MF_CHECKED = 8, MF_SEPARATOR = 0x800, MF_BYCOMMAND = 0;
constexpr UINT TPM_LEFTALIGN = 0, TPM_RIGHTALIGN = 8, TPM_BOTTOMALIGN = 0x20, TPM_RIGHTBUTTON = 2, TPM_RETURNCMD = 0x100, TPM_NONOTIFY = 0x80;
constexpr DWORD TME_LEAVE = 2; constexpr int HTCLIENT = 1; constexpr int WHEEL_DELTA = 120; constexpr UINT IMAGE_ICON = 1; constexpr UINT LR_DEFAULTCOLOR = 0, LR_SHARED = 0x8000, LR_DEFAULTSIZE = 0x40;
constexpr UINT PBT_APMSUSPEND = 4, PBT_APMRESUMESUSPEND = 7, PBT_APMRESUMEAUTOMATIC = 0x12; constexpr DWORD DEVICE_NOTIFY_WINDOW_HANDLE = 0;
constexpr UINT CF_UNICODETEXT = 13; constexpr UINT GMEM_MOVEABLE = 2; constexpr UINT CP_UTF8 = 65001; constexpr DWORD WC_ERR_INVALID_CHARS = 0x80, MB_ERR_INVALID_CHARS = 8;
constexpr DWORD GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000, FILE_SHARE_READ = 1, CREATE_NEW = 1, CREATE_ALWAYS = 2, OPEN_EXISTING = 3, FILE_ATTRIBUTE_NORMAL = 0x80,
                MOVEFILE_REPLACE_EXISTING = 1, MOVEFILE_WRITE_THROUGH = 8, MAX_PATH = 260;
constexpr int DEFAULT_CHARSET = 1, OUT_DEFAULT_PRECIS = 0, CLIP_DEFAULT_PRECIS = 0, ANTIALIASED_QUALITY = 4, CLEARTYPE_QUALITY = 5, DEFAULT_PITCH = 0, FF_DONTCARE = 0, FW_NORMAL = 400, FW_SEMIBOLD = 600;
inline const LPCWSTR IDC_ARROW = reinterpret_cast<LPCWSTR>(32512), IDC_HAND = reinterpret_cast<LPCWSTR>(32649), IDC_IBEAM = reinterpret_cast<LPCWSTR>(32513), IDI_APPLICATION = reinterpret_cast<LPCWSTR>(32512);
inline const DPI_AWARENESS_CONTEXT DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = reinterpret_cast<DPI_AWARENESS_CONTEXT>(static_cast<LONG_PTR>(-4));
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20, DWMWA_WINDOW_CORNER_PREFERENCE = 33, DWMWA_SYSTEMBACKDROP_TYPE = 38;
#define MAKEINTRESOURCEW(i) (reinterpret_cast<LPCWSTR>(static_cast<ULONG_PTR>(static_cast<WORD>(i))))
#define RGB(r, g, b) (static_cast<COLORREF>((static_cast<BYTE>(r)) | (static_cast<WORD>(static_cast<BYTE>(g)) << 8) | (static_cast<DWORD>(static_cast<BYTE>(b)) << 16)))
#define GetRValue(rgb) (static_cast<BYTE>(rgb))
#define GetGValue(rgb) (static_cast<BYTE>((static_cast<WORD>(rgb)) >> 8))
#define GetBValue(rgb) (static_cast<BYTE>((rgb) >> 16))
#define LOWORD(l) (static_cast<WORD>(static_cast<DWORD_PTR>(l) & 0xffff))
#define HIWORD(l) (static_cast<WORD>((static_cast<DWORD_PTR>(l) >> 16) & 0xffff))
#define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))
#define GET_WHEEL_DELTA_WPARAM(wParam) (static_cast<short>(HIWORD(wParam)))
#define MAKELPARAM(l, h) (static_cast<LPARAM>(static_cast<DWORD>((static_cast<WORD>(l)) | (static_cast<DWORD>(static_cast<WORD>(h)) << 16))))
constexpr UINT MB_OKCANCEL = 1, MB_ICONWARNING = 0x30, MB_DEFBUTTON2 = 0x100;
constexpr int IDOK = 1;
WINMOCK_FN(int, MessageBoxW)
WINMOCK_FN(HWND, CreateWindowExW) WINMOCK_FN(LRESULT, DefWindowProcW) WINMOCK_FN(ATOM, RegisterClassExW) WINMOCK_FN(BOOL, ShowWindow) WINMOCK_FN(BOOL, UpdateWindow)
WINMOCK_FN(BOOL, DestroyWindow) WINMOCK_FN(BOOL, GetMessageW) WINMOCK_FN(BOOL, TranslateMessage) WINMOCK_FN(LRESULT, DispatchMessageW) WINMOCK_FN(void, PostQuitMessage)
WINMOCK_FN(BOOL, PostMessageW) WINMOCK_FN(LRESULT, SendMessageW) WINMOCK_FN(LONG_PTR, GetWindowLongPtrW) WINMOCK_FN(LONG_PTR, SetWindowLongPtrW) WINMOCK_FN(BOOL, GetClientRect)
WINMOCK_FN(BOOL, InvalidateRect) WINMOCK_FN(HDC, BeginPaint) WINMOCK_FN(BOOL, EndPaint) WINMOCK_FN(BOOL, SetWindowPos) WINMOCK_FN(HWND, SetFocus) WINMOCK_FN(HWND, GetFocus)
WINMOCK_FN(BOOL, SetForegroundWindow) WINMOCK_FN(BOOL, AllowSetForegroundWindow) WINMOCK_FN(BOOL, IsIconic) WINMOCK_FN(BOOL, IsWindow) WINMOCK_FN(BOOL, IsWindowVisible) WINMOCK_FN(HWND, FindWindowW)
WINMOCK_FN(UINT, RegisterWindowMessageW) WINMOCK_FN(HCURSOR, LoadCursorW) WINMOCK_FN(HCURSOR, SetCursor) WINMOCK_FN(HICON, LoadIconW) WINMOCK_FN(HANDLE, LoadImageW)
WINMOCK_FN(HRESULT, LoadIconMetric) WINMOCK_FN(BOOL, DestroyIcon) WINMOCK_FN(int, GetSystemMetrics) WINMOCK_FN(DWORD, GetSysColor) WINMOCK_FN(BOOL, SystemParametersInfoW)
WINMOCK_FN(UINT_PTR, SetTimer) WINMOCK_FN(BOOL, KillTimer) WINMOCK_FN(SHORT, GetKeyState) WINMOCK_FN(BOOL, TrackMouseEvent) WINMOCK_FN(HWND, SetCapture) WINMOCK_FN(BOOL, ReleaseCapture)
WINMOCK_FN(BOOL, ScreenToClient) WINMOCK_FN(BOOL, ClientToScreen) WINMOCK_FN(int, GetWindowTextW) WINMOCK_FN(int, GetWindowTextLengthW) WINMOCK_FN(BOOL, SetWindowTextW)
WINMOCK_FN(HMENU, CreatePopupMenu) WINMOCK_FN(BOOL, AppendMenuW) WINMOCK_FN(BOOL, DestroyMenu) WINMOCK_FN(BOOL, SetMenuDefaultItem) WINMOCK_FN(BOOL, TrackPopupMenuEx)
WINMOCK_FN(UINT, GetDpiForWindow) WINMOCK_FN(UINT, GetDpiForSystem) WINMOCK_FN(BOOL, SetProcessDpiAwarenessContext) WINMOCK_FN(int, MulDiv) WINMOCK_FN(DWORD, GetWindowThreadProcessId)
WINMOCK_FN(HPOWERNOTIFY, RegisterSuspendResumeNotification) WINMOCK_FN(BOOL, UnregisterSuspendResumeNotification) WINMOCK_FN(BOOL, OpenClipboard) WINMOCK_FN(BOOL, CloseClipboard)
WINMOCK_FN(BOOL, EmptyClipboard) WINMOCK_FN(HANDLE, SetClipboardData) WINMOCK_FN(HGLOBAL, GlobalAlloc) WINMOCK_FN(LPVOID, GlobalLock) WINMOCK_FN(BOOL, GlobalUnlock) WINMOCK_FN(HGLOBAL, GlobalFree)
WINMOCK_FN(HLOCAL, LocalFree) WINMOCK_FN(HFONT, CreateFontW) WINMOCK_FN(HBRUSH, CreateSolidBrush) WINMOCK_FN(BOOL, DeleteObject) WINMOCK_FN(COLORREF, SetBkColor) WINMOCK_FN(COLORREF, SetTextColor)
WINMOCK_FN(BOOL, SetWindowSubclass) WINMOCK_FN(BOOL, RemoveWindowSubclass) WINMOCK_FN(LRESULT, DefSubclassProc) WINMOCK_FN(BOOL, InitCommonControlsEx) WINMOCK_FN(HRESULT, DwmSetWindowAttribute)
WINMOCK_FN(HANDLE, CreateMutexW) WINMOCK_FN(HANDLE, CreateEventW) WINMOCK_FN(BOOL, SetEvent) WINMOCK_FN(BOOL, ResetEvent) WINMOCK_FN(BOOL, CloseHandle) WINMOCK_FN(DWORD, WaitForSingleObject)
WINMOCK_FN(DWORD, WaitForMultipleObjects) WINMOCK_FN(DWORD, GetLastError) WINMOCK_FN(void, SetLastError) WINMOCK_FN(DWORD, GetCurrentProcessId) WINMOCK_FN(DWORD, GetModuleFileNameW)
WINMOCK_FN(LPWSTR, GetCommandLineW) WINMOCK_FN(ULONG64, GetTickCount64) WINMOCK_FN(void, GetSystemTime) WINMOCK_FN(void, Sleep) WINMOCK_FN(HANDLE, CreateFileW) WINMOCK_FN(BOOL, ReadFile)
WINMOCK_FN(BOOL, WriteFile) WINMOCK_FN(BOOL, FlushFileBuffers) WINMOCK_FN(BOOL, GetFileSizeEx) WINMOCK_FN(BOOL, MoveFileExW) WINMOCK_FN(BOOL, DeleteFileW) WINMOCK_FN(BOOL, CreateDirectoryW)
WINMOCK_FN(int, MultiByteToWideChar) WINMOCK_FN(int, WideCharToMultiByte) WINMOCK_FN(HMODULE, GetModuleHandleW) WINMOCK_FN(BOOL, MessageBeep)
template <class T> inline LONG InterlockedIncrement(T* v) { return static_cast<LONG>(++*v); }
template <class T> inline LONG InterlockedDecrement(T* v) { return static_cast<LONG>(--*v); }

// ---- registry / credentials / shell ----
inline const HKEY HKEY_CURRENT_USER = reinterpret_cast<HKEY>(static_cast<ULONG_PTR>(0x80000001));
constexpr DWORD KEY_READ = 0x20019, KEY_WRITE = 0x20006, KEY_NOTIFY = 0x10, KEY_QUERY_VALUE = 1, KEY_SET_VALUE = 2, REG_SZ = 1, REG_BINARY = 3, REG_DWORD = 4,
                RRF_RT_REG_SZ = 2, RRF_RT_REG_BINARY = 8, RRF_RT_REG_DWORD = 0x10, RRF_RT_ANY = 0xffff,
                REG_NOTIFY_CHANGE_NAME = 1, REG_NOTIFY_CHANGE_LAST_SET = 4, REG_NOTIFY_THREAD_AGNOSTIC = 0x10000000;
WINMOCK_FN(LSTATUS, RegOpenKeyExW) WINMOCK_FN(LSTATUS, RegCloseKey) WINMOCK_FN(LSTATUS, RegGetValueW) WINMOCK_FN(LSTATUS, RegSetKeyValueW) WINMOCK_FN(LSTATUS, RegDeleteKeyValueW)
WINMOCK_FN(LSTATUS, RegNotifyChangeKeyValue) WINMOCK_FN(LSTATUS, RegCreateKeyExW) WINMOCK_FN(LSTATUS, RegSetValueExW)
constexpr DWORD CRED_TYPE_GENERIC = 1, CRED_PERSIST_LOCAL_MACHINE = 2, CRED_MAX_CREDENTIAL_BLOB_SIZE = 5 * 512;
WINMOCK_FN(BOOL, CredReadW) WINMOCK_FN(BOOL, CredWriteW) WINMOCK_FN(BOOL, CredDeleteW) WINMOCK_FN(void, CredFree)
constexpr DWORD KF_FLAG_CREATE = 0x8000, KF_FLAG_DEFAULT = 0; constexpr int SIGDN_FILESYSPATH = static_cast<int>(0x80058000);
WINMOCK_FN(HRESULT, SHGetKnownFolderPath) WINMOCK_FN(HINSTANCE, ShellExecuteW) WINMOCK_FN(BOOL, Shell_NotifyIconW) WINMOCK_FN(LPWSTR*, CommandLineToArgvW)
constexpr DWORD NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2, NIM_SETFOCUS = 3, NIM_SETVERSION = 4, NIF_MESSAGE = 1, NIF_ICON = 2, NIF_TIP = 4, NIF_INFO = 0x10, NIF_SHOWTIP = 0x80;
constexpr UINT NOTIFYICON_VERSION_4 = 4, NIN_SELECT = WM_USER + 0, NIN_KEYSELECT = WM_USER + 1, NIN_POPUPOPEN = WM_USER + 6;
constexpr DWORD ICC_STANDARD_CLASSES = 0x4000, ICC_WIN95_CLASSES = 0xff; constexpr int LIM_SMALL = 0, LIM_LARGE = 1;

// ---- COM ----
constexpr DWORD COINIT_APARTMENTTHREADED = 2, COINIT_DISABLE_OLE1DDE = 4, CLSCTX_INPROC_SERVER = 1, CLSCTX_ALL = 23;
WINMOCK_FN(HRESULT, CoInitializeEx) WINMOCK_FN(void, CoUninitialize) WINMOCK_FN(HRESULT, CoCreateInstance) WINMOCK_FN(void, CoTaskMemFree)
struct IUnknown {
    virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) = 0;
    virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
    virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
#define WINMOCK_M(R, name) template <class... A> R name(A&&...) { return ::winmock::ret<R>(); }
struct IShellItem : IUnknown { WINMOCK_M(HRESULT, GetDisplayName) };
struct IFileSaveDialog : IUnknown {
    WINMOCK_M(HRESULT, SetFileTypes) WINMOCK_M(HRESULT, SetFileTypeIndex) WINMOCK_M(HRESULT, SetFileName) WINMOCK_M(HRESULT, SetDefaultExtension)
    WINMOCK_M(HRESULT, SetTitle) WINMOCK_M(HRESULT, SetOptions) WINMOCK_M(HRESULT, GetOptions) WINMOCK_M(HRESULT, Show) WINMOCK_M(HRESULT, GetResult) };
constexpr DWORD FOS_OVERWRITEPROMPT = 2, FOS_FORCEFILESYSTEM = 0x40, FOS_STRICTFILETYPES = 4;
struct IAccPropServices : IUnknown { WINMOCK_M(HRESULT, SetHwndPropStr) WINMOCK_M(HRESULT, ClearHwndProps) WINMOCK_M(HRESULT, SetHwndProp) };
constexpr LONG OBJID_CLIENT = -4, CHILDID_SELF = 0;

namespace Microsoft { namespace WRL {
template <class T> class ComPtr {
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    ComPtr(T* p) : p_(p) {}
    template <class U> ComPtr(const ComPtr<U>& other) : p_(other.Get()) {}
    T* Get() const { return p_; }
    T** GetAddressOf() { return &p_; }
    T* const* GetAddressOf() const { return &p_; }
    T** ReleaseAndGetAddressOf() { p_ = nullptr; return &p_; }
    T** operator&() { return &p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }
    ComPtr& operator=(std::nullptr_t) { p_ = nullptr; return *this; }
    ComPtr& operator=(T* p) { p_ = p; return *this; }
    void Reset() { p_ = nullptr; }
    T* Detach() { T* p = p_; p_ = nullptr; return p; }
    void Attach(T* p) { p_ = p; }
    void Swap(ComPtr& other) { T* t = p_; p_ = other.p_; other.p_ = t; }
    template <class U> HRESULT As(ComPtr<U>*) const { return S_OK; }
    template <class U> HRESULT CopyTo(U**) const { return S_OK; }
    friend bool operator==(const ComPtr& a, std::nullptr_t) { return a.p_ == nullptr; }
    friend bool operator==(const ComPtr& a, const ComPtr& b) { return a.p_ == b.p_; }
private:
    T* p_ = nullptr;
};
}}  // namespace Microsoft::WRL

// ---- Direct2D ----
struct D2D1_COLOR_F { float r = 0, g = 0, b = 0, a = 0; }; using D2D_COLOR_F = D2D1_COLOR_F;
struct D2D1_POINT_2F { float x = 0, y = 0; }; using D2D_POINT_2F = D2D1_POINT_2F;
struct D2D1_SIZE_F { float width = 0, height = 0; }; using D2D_SIZE_F = D2D1_SIZE_F;
struct D2D1_SIZE_U { UINT32 width = 0, height = 0; }; using D2D_SIZE_U = D2D1_SIZE_U;
struct D2D1_RECT_F { float left = 0, top = 0, right = 0, bottom = 0; }; using D2D_RECT_F = D2D1_RECT_F;
struct D2D1_ROUNDED_RECT { D2D1_RECT_F rect; float radiusX = 0, radiusY = 0; };
struct D2D1_ELLIPSE { D2D1_POINT_2F point; float radiusX = 0, radiusY = 0; };
struct D2D1_MATRIX_3X2_F { float _11 = 1, _12 = 0, _21 = 0, _22 = 1, _31 = 0, _32 = 0; }; using D2D_MATRIX_3X2_F = D2D1_MATRIX_3X2_F;
struct D2D1_BEZIER_SEGMENT { D2D1_POINT_2F point1, point2, point3; };
enum D2D1_CAP_STYLE { D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_SQUARE, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_TRIANGLE };
enum D2D1_LINE_JOIN { D2D1_LINE_JOIN_MITER, D2D1_LINE_JOIN_BEVEL, D2D1_LINE_JOIN_ROUND, D2D1_LINE_JOIN_MITER_OR_BEVEL };
enum D2D1_DASH_STYLE { D2D1_DASH_STYLE_SOLID, D2D1_DASH_STYLE_DASH };
enum D2D1_FIGURE_BEGIN { D2D1_FIGURE_BEGIN_FILLED, D2D1_FIGURE_BEGIN_HOLLOW };
enum D2D1_FIGURE_END { D2D1_FIGURE_END_OPEN, D2D1_FIGURE_END_CLOSED };
enum D2D1_ANTIALIAS_MODE { D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1_ANTIALIAS_MODE_ALIASED };
enum D2D1_TEXT_ANTIALIAS_MODE { D2D1_TEXT_ANTIALIAS_MODE_DEFAULT, D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE, D2D1_TEXT_ANTIALIAS_MODE_ALIASED };
enum D2D1_DRAW_TEXT_OPTIONS { D2D1_DRAW_TEXT_OPTIONS_NONE = 0, D2D1_DRAW_TEXT_OPTIONS_NO_SNAP = 1, D2D1_DRAW_TEXT_OPTIONS_CLIP = 2, D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT = 4 };
enum D2D1_FACTORY_TYPE { D2D1_FACTORY_TYPE_SINGLE_THREADED, D2D1_FACTORY_TYPE_MULTI_THREADED };
enum D2D1_RENDER_TARGET_TYPE { D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1_RENDER_TARGET_TYPE_HARDWARE };
enum D2D1_PRESENT_OPTIONS { D2D1_PRESENT_OPTIONS_NONE = 0, D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS = 1, D2D1_PRESENT_OPTIONS_IMMEDIATELY = 2 };
enum D2D1_ALPHA_MODE { D2D1_ALPHA_MODE_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED, D2D1_ALPHA_MODE_STRAIGHT, D2D1_ALPHA_MODE_IGNORE };
enum DXGI_FORMAT { DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_B8G8R8A8_UNORM = 87 };
enum D2D1_FILL_MODE { D2D1_FILL_MODE_ALTERNATE, D2D1_FILL_MODE_WINDING };
enum D2D1_WINDOW_STATE { D2D1_WINDOW_STATE_NONE = 0, D2D1_WINDOW_STATE_OCCLUDED = 1 };
struct D2D1_PIXEL_FORMAT { DXGI_FORMAT format; D2D1_ALPHA_MODE alphaMode; };
struct D2D1_BITMAP_PROPERTIES { D2D1_PIXEL_FORMAT pixelFormat; float dpiX, dpiY; };
enum D2D1_BITMAP_INTERPOLATION_MODE { D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR };
struct D2D1_RENDER_TARGET_PROPERTIES { D2D1_RENDER_TARGET_TYPE type; D2D1_PIXEL_FORMAT pixelFormat; float dpiX, dpiY; int usage; int minLevel; };
struct D2D1_HWND_RENDER_TARGET_PROPERTIES { HWND hwnd; D2D1_SIZE_U pixelSize; D2D1_PRESENT_OPTIONS presentOptions; };
struct D2D1_STROKE_STYLE_PROPERTIES { D2D1_CAP_STYLE startCap, endCap, dashCap; D2D1_LINE_JOIN lineJoin; float miterLimit; D2D1_DASH_STYLE dashStyle; float dashOffset; };
namespace D2D1 {
class ColorF : public D2D1_COLOR_F {
public:
    enum Enum { Black = 0x000000, White = 0xFFFFFF, Magenta = 0xFF00FF };
    ColorF(UINT32 rgb, float alpha = 1.0f) { r = ((rgb >> 16) & 0xff) / 255.f; g = ((rgb >> 8) & 0xff) / 255.f; b = (rgb & 0xff) / 255.f; a = alpha; }
    ColorF(Enum known, float alpha = 1.0f) : ColorF(static_cast<UINT32>(known), alpha) {}
    ColorF(float red, float green, float blue, float alpha = 1.0f) { r = red; g = green; b = blue; a = alpha; }
};
inline D2D1_RECT_F RectF(float l = 0, float t = 0, float r = 0, float b = 0) { return {l, t, r, b}; }
inline D2D1_POINT_2F Point2F(float x = 0, float y = 0) { return {x, y}; }
inline D2D1_SIZE_F SizeF(float w = 0, float h = 0) { return {w, h}; }
inline D2D1_SIZE_U SizeU(UINT32 w = 0, UINT32 h = 0) { return {w, h}; }
inline D2D1_ROUNDED_RECT RoundedRect(const D2D1_RECT_F& rc, float rx, float ry) { return {rc, rx, ry}; }
inline D2D1_ELLIPSE Ellipse(const D2D1_POINT_2F& c, float rx, float ry) { return {c, rx, ry}; }
inline D2D1_BEZIER_SEGMENT BezierSegment(const D2D1_POINT_2F& a, const D2D1_POINT_2F& b, const D2D1_POINT_2F& c) { return {a, b, c}; }
inline D2D1_PIXEL_FORMAT PixelFormat(DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE m = D2D1_ALPHA_MODE_UNKNOWN) { return {f, m}; }
D2D1_BITMAP_PROPERTIES BitmapProperties(D2D1_PIXEL_FORMAT pixelFormat = PixelFormat(), float dpiX = 96.0f, float dpiY = 96.0f);
inline D2D1_RENDER_TARGET_PROPERTIES RenderTargetProperties(D2D1_RENDER_TARGET_TYPE t = D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1_PIXEL_FORMAT pf = PixelFormat(),
                                                             float dx = 0, float dy = 0, int usage = 0, int level = 0) { return {t, pf, dx, dy, usage, level}; }
inline D2D1_HWND_RENDER_TARGET_PROPERTIES HwndRenderTargetProperties(HWND h, D2D1_SIZE_U s = SizeU(), D2D1_PRESENT_OPTIONS o = D2D1_PRESENT_OPTIONS_NONE) { return {h, s, o}; }
inline D2D1_STROKE_STYLE_PROPERTIES StrokeStyleProperties(D2D1_CAP_STYLE s = D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE e = D2D1_CAP_STYLE_FLAT,
        D2D1_CAP_STYLE d = D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN j = D2D1_LINE_JOIN_MITER, float ml = 10, D2D1_DASH_STYLE ds = D2D1_DASH_STYLE_SOLID, float off = 0) {
    return {s, e, d, j, ml, ds, off};
}
class Matrix3x2F : public D2D1_MATRIX_3X2_F {
public:
    Matrix3x2F() = default;
    Matrix3x2F(float a, float b, float c, float d, float e, float f) { _11 = a; _12 = b; _21 = c; _22 = d; _31 = e; _32 = f; }
    static Matrix3x2F Identity() { return Matrix3x2F(); }
    static Matrix3x2F Translation(float x, float y) { return Matrix3x2F(1, 0, 0, 1, x, y); }
    static Matrix3x2F Translation(D2D1_SIZE_F s) { return Translation(s.width, s.height); }
    static Matrix3x2F Scale(float x, float y, D2D1_POINT_2F = Point2F()) { return Matrix3x2F(x, 0, 0, y, 0, 0); }
    static Matrix3x2F Scale(D2D1_SIZE_F s, D2D1_POINT_2F c = Point2F()) { return Scale(s.width, s.height, c); }
    static Matrix3x2F Rotation(float, D2D1_POINT_2F = Point2F()) { return Matrix3x2F(); }
    Matrix3x2F operator*(const Matrix3x2F&) const { return *this; }
    D2D1_POINT_2F TransformPoint(D2D1_POINT_2F p) const { return p; }
    bool Invert() { return true; }
    static const Matrix3x2F* ReinterpretBaseType(const D2D1_MATRIX_3X2_F* m) { return static_cast<const Matrix3x2F*>(m); }
    static Matrix3x2F* ReinterpretBaseType(D2D1_MATRIX_3X2_F* m) { return static_cast<Matrix3x2F*>(m); }
};
inline D2D1_MATRIX_3X2_F IdentityMatrix() { return {}; }
}  // namespace D2D1
inline D2D1_MATRIX_3X2_F operator*(const D2D1_MATRIX_3X2_F& a, const D2D1_MATRIX_3X2_F& b) { (void)b; return a; }
struct ID2D1Resource : IUnknown { WINMOCK_M(void, GetFactory) };
struct ID2D1Bitmap : ID2D1Resource {};
struct IWICBitmapSource;
struct ID2D1Brush : ID2D1Resource { WINMOCK_M(void, SetOpacity) WINMOCK_M(float, GetOpacity) };
struct ID2D1SolidColorBrush : ID2D1Brush { WINMOCK_M(void, SetColor) D2D1_COLOR_F GetColor() const { return {}; } };
struct ID2D1StrokeStyle : ID2D1Resource {};
struct ID2D1GeometrySink : IUnknown {
    WINMOCK_M(void, BeginFigure) WINMOCK_M(void, AddLine) WINMOCK_M(void, AddLines) WINMOCK_M(void, AddBezier) WINMOCK_M(void, AddBeziers) WINMOCK_M(void, AddArc)
    WINMOCK_M(void, AddQuadraticBezier) WINMOCK_M(void, EndFigure) WINMOCK_M(HRESULT, Close) WINMOCK_M(void, SetFillMode) WINMOCK_M(void, SetSegmentFlags) };
struct ID2D1Geometry : ID2D1Resource { WINMOCK_M(HRESULT, GetBounds) };
struct ID2D1PathGeometry : ID2D1Geometry { WINMOCK_M(HRESULT, Open) };
struct ID2D1RenderTarget : ID2D1Resource {
    HRESULT CreateBitmapFromWicBitmap(IWICBitmapSource*, const D2D1_BITMAP_PROPERTIES*, ID2D1Bitmap**);
    void DrawBitmap(ID2D1Bitmap*, const D2D1_RECT_F*, FLOAT, D2D1_BITMAP_INTERPOLATION_MODE, const D2D1_RECT_F*);
    WINMOCK_M(HRESULT, CreateSolidColorBrush) WINMOCK_M(void, BeginDraw) WINMOCK_M(HRESULT, EndDraw) WINMOCK_M(void, Clear) WINMOCK_M(void, SetTransform)
    WINMOCK_M(void, GetTransform) WINMOCK_M(void, FillRectangle) WINMOCK_M(void, DrawRectangle) WINMOCK_M(void, FillRoundedRectangle) WINMOCK_M(void, DrawRoundedRectangle)
    WINMOCK_M(void, FillEllipse) WINMOCK_M(void, DrawEllipse) WINMOCK_M(void, DrawLine) WINMOCK_M(void, DrawGeometry) WINMOCK_M(void, FillGeometry)
    WINMOCK_M(void, DrawTextLayout) WINMOCK_M(void, DrawText) WINMOCK_M(void, PushAxisAlignedClip) WINMOCK_M(void, PopAxisAlignedClip) WINMOCK_M(void, SetAntialiasMode)
    WINMOCK_M(void, SetTextAntialiasMode) WINMOCK_M(void, SetTextRenderingParams) WINMOCK_M(void, GetTextRenderingParams)
    WINMOCK_M(D2D1_TEXT_ANTIALIAS_MODE, GetTextAntialiasMode) WINMOCK_M(void, SetDpi) WINMOCK_M(void, GetDpi) WINMOCK_M(HRESULT, Flush)
    D2D1_PIXEL_FORMAT GetPixelFormat() const { return {}; }
    D2D1_SIZE_F GetSize() const { return {}; }
    D2D1_SIZE_U GetPixelSize() const { return {}; } };
struct ID2D1HwndRenderTarget : ID2D1RenderTarget { WINMOCK_M(HRESULT, Resize) WINMOCK_M(D2D1_WINDOW_STATE, CheckWindowState) HWND GetHwnd() const { return nullptr; } };
struct ID2D1Factory : IUnknown {
    WINMOCK_M(HRESULT, CreateHwndRenderTarget) WINMOCK_M(HRESULT, CreatePathGeometry) WINMOCK_M(HRESULT, CreateStrokeStyle) WINMOCK_M(HRESULT, CreateEllipseGeometry)
    WINMOCK_M(HRESULT, CreateRoundedRectangleGeometry) WINMOCK_M(void, GetDesktopDpi) WINMOCK_M(HRESULT, ReloadSystemMetrics) };
WINMOCK_FN(HRESULT, D2D1CreateFactory)

// ---- WIC (declarations only; no image decoding or resource loading) ----
inline const CLSID CLSID_WICImagingFactory{};
inline const GUID GUID_WICPixelFormat32bppPBGRA{};
enum WICBitmapDitherType { WICBitmapDitherTypeNone = 0 };
enum WICBitmapPaletteType { WICBitmapPaletteTypeCustom = 0 };
struct IWICPalette;
struct IWICBitmapSource : IUnknown {};
struct IWICBitmap : IWICBitmapSource {};
struct IWICFormatConverter : IWICBitmapSource {
    HRESULT Initialize(IWICBitmapSource*, REFGUID, WICBitmapDitherType, IWICPalette*, DOUBLE, WICBitmapPaletteType);
};
struct IWICImagingFactory : IUnknown {
    HRESULT CreateBitmapFromHICON(HICON, IWICBitmap**);
    HRESULT CreateFormatConverter(IWICFormatConverter**);
};

// ---- DirectWrite ----
enum DWRITE_FACTORY_TYPE { DWRITE_FACTORY_TYPE_SHARED, DWRITE_FACTORY_TYPE_ISOLATED };
enum DWRITE_PIXEL_GEOMETRY { DWRITE_PIXEL_GEOMETRY_FLAT, DWRITE_PIXEL_GEOMETRY_RGB, DWRITE_PIXEL_GEOMETRY_BGR };
enum DWRITE_RENDERING_MODE { DWRITE_RENDERING_MODE_DEFAULT, DWRITE_RENDERING_MODE_ALIASED, DWRITE_RENDERING_MODE_GDI_CLASSIC,
                            DWRITE_RENDERING_MODE_GDI_NATURAL, DWRITE_RENDERING_MODE_NATURAL, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                            DWRITE_RENDERING_MODE_OUTLINE };
struct IDWriteRenderingParams : IUnknown {
    WINMOCK_M(float, GetGamma) WINMOCK_M(float, GetEnhancedContrast) WINMOCK_M(float, GetClearTypeLevel)
    WINMOCK_M(DWRITE_PIXEL_GEOMETRY, GetPixelGeometry) WINMOCK_M(DWRITE_RENDERING_MODE, GetRenderingMode) };
enum DWRITE_FONT_WEIGHT { DWRITE_FONT_WEIGHT_NORMAL = 400, DWRITE_FONT_WEIGHT_MEDIUM = 500, DWRITE_FONT_WEIGHT_SEMI_BOLD = 600, DWRITE_FONT_WEIGHT_BOLD = 700 };
enum DWRITE_FONT_STYLE { DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STYLE_OBLIQUE, DWRITE_FONT_STYLE_ITALIC };
enum DWRITE_FONT_STRETCH { DWRITE_FONT_STRETCH_UNDEFINED = 0, DWRITE_FONT_STRETCH_NORMAL = 5 };
enum DWRITE_TEXT_ALIGNMENT { DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_TEXT_ALIGNMENT_JUSTIFIED };
enum DWRITE_PARAGRAPH_ALIGNMENT { DWRITE_PARAGRAPH_ALIGNMENT_NEAR, DWRITE_PARAGRAPH_ALIGNMENT_FAR, DWRITE_PARAGRAPH_ALIGNMENT_CENTER };
enum DWRITE_WORD_WRAPPING { DWRITE_WORD_WRAPPING_WRAP, DWRITE_WORD_WRAPPING_NO_WRAP, DWRITE_WORD_WRAPPING_EMERGENCY_BREAK, DWRITE_WORD_WRAPPING_WHOLE_WORD, DWRITE_WORD_WRAPPING_CHARACTER };
enum DWRITE_LINE_SPACING_METHOD { DWRITE_LINE_SPACING_METHOD_DEFAULT, DWRITE_LINE_SPACING_METHOD_UNIFORM, DWRITE_LINE_SPACING_METHOD_PROPORTIONAL };
enum DWRITE_TRIMMING_GRANULARITY { DWRITE_TRIMMING_GRANULARITY_NONE, DWRITE_TRIMMING_GRANULARITY_CHARACTER, DWRITE_TRIMMING_GRANULARITY_WORD };
struct DWRITE_TEXT_METRICS { float left, top, width, widthIncludingTrailingWhitespace, height, layoutWidth, layoutHeight; UINT32 maxBidiReorderingDepth, lineCount; };
struct DWRITE_TEXT_RANGE { UINT32 startPosition, length; };
struct DWRITE_TRIMMING { DWRITE_TRIMMING_GRANULARITY granularity; UINT32 delimiter, delimiterCount; };
struct DWRITE_HIT_TEST_METRICS { UINT32 textPosition, length; float left, top, width, height; UINT32 bidiLevel; BOOL isText, isTrimmed; };
struct IDWriteFont : IUnknown { HRESULT HasCharacter(UINT32 unicodeValue, BOOL* exists); };
struct IDWriteFontFamily : IUnknown {
    HRESULT GetFirstMatchingFont(DWRITE_FONT_WEIGHT, DWRITE_FONT_STRETCH, DWRITE_FONT_STYLE, IDWriteFont**);
};
struct IDWriteFontCollection : IUnknown {
    WINMOCK_M(HRESULT, FindFamilyName) WINMOCK_M(UINT32, GetFontFamilyCount)
    HRESULT GetFontFamily(UINT32 index, IDWriteFontFamily** fontFamily);
};
struct IDWriteTextFormat : IUnknown {
    WINMOCK_M(HRESULT, SetTextAlignment) WINMOCK_M(HRESULT, SetParagraphAlignment) WINMOCK_M(HRESULT, SetWordWrapping) WINMOCK_M(HRESULT, SetLineSpacing)
    WINMOCK_M(HRESULT, SetTrimming) WINMOCK_M(HRESULT, SetReadingDirection) WINMOCK_M(float, GetFontSize) };
struct IDWriteTextLayout : IDWriteTextFormat {
    WINMOCK_M(HRESULT, GetMetrics) WINMOCK_M(HRESULT, SetUnderline) WINMOCK_M(HRESULT, SetMaxWidth) WINMOCK_M(HRESULT, SetMaxHeight) WINMOCK_M(HRESULT, SetFontWeight)
    WINMOCK_M(HRESULT, SetFontSize) WINMOCK_M(HRESULT, HitTestPoint) WINMOCK_M(HRESULT, HitTestTextPosition) WINMOCK_M(float, GetMaxWidth) WINMOCK_M(HRESULT, GetLineMetrics)
    WINMOCK_M(HRESULT, DetermineMinWidth) };
struct IDWriteFactory : IUnknown { WINMOCK_M(HRESULT, CreateTextFormat) WINMOCK_M(HRESULT, CreateTextLayout) WINMOCK_M(HRESULT, GetSystemFontCollection)
    WINMOCK_M(HRESULT, CreateRenderingParams) WINMOCK_M(HRESULT, CreateMonitorRenderingParams) WINMOCK_M(HRESULT, CreateCustomRenderingParams) };
WINMOCK_FN(HRESULT, DWriteCreateFactory)

// ---- UI Automation / OLE automation ----
using PROPERTYID = int; using PATTERNID = int; using EVENTID = int; using CONTROLTYPEID = int;
enum ProviderOptions { ProviderOptions_ClientSideProvider = 1, ProviderOptions_ServerSideProvider = 2, ProviderOptions_NonClientAreaProvider = 4,
                       ProviderOptions_OverrideProvider = 8, ProviderOptions_ProviderOwnsSetFocus = 0x10, ProviderOptions_UseComThreading = 0x20 };
constexpr ProviderOptions operator|(ProviderOptions a, ProviderOptions b) { return static_cast<ProviderOptions>(static_cast<int>(a) | static_cast<int>(b)); }
enum NavigateDirection { NavigateDirection_Parent, NavigateDirection_NextSibling, NavigateDirection_PreviousSibling, NavigateDirection_FirstChild, NavigateDirection_LastChild };
enum ToggleState { ToggleState_Off, ToggleState_On, ToggleState_Indeterminate };
enum StructureChangeType { StructureChangeType_ChildAdded, StructureChangeType_ChildRemoved, StructureChangeType_ChildrenInvalidated,
                           StructureChangeType_ChildrenBulkAdded, StructureChangeType_ChildrenBulkRemoved, StructureChangeType_ChildrenReordered };
enum LiveSetting { Off = 0, Polite = 1, Assertive = 2 };
struct UiaRect { double left, top, width, height; };
constexpr int UiaAppendRuntimeId = 3, UiaRootObjectId = -25;
constexpr PROPERTYID UIA_ControlTypePropertyId = 30003, UIA_NamePropertyId = 30005, UIA_HasKeyboardFocusPropertyId = 30008, UIA_IsKeyboardFocusablePropertyId = 30009,
    UIA_IsEnabledPropertyId = 30010, UIA_AutomationIdPropertyId = 30011, UIA_ClassNamePropertyId = 30012, UIA_HelpTextPropertyId = 30013, UIA_IsControlElementPropertyId = 30016,
    UIA_IsContentElementPropertyId = 30017, UIA_IsOffscreenPropertyId = 30022, UIA_FrameworkIdPropertyId = 30024, UIA_LiveSettingPropertyId = 30135;
constexpr CONTROLTYPEID UIA_ButtonControlTypeId = 50000, UIA_CheckBoxControlTypeId = 50002, UIA_EditControlTypeId = 50004, UIA_HyperlinkControlTypeId = 50005,
    UIA_ListItemControlTypeId = 50007, UIA_ListControlTypeId = 50008, UIA_RadioButtonControlTypeId = 50013, UIA_TabControlTypeId = 50018, UIA_TabItemControlTypeId = 50019,
    UIA_TextControlTypeId = 50020, UIA_GroupControlTypeId = 50026, UIA_PaneControlTypeId = 50033;
constexpr PATTERNID UIA_InvokePatternId = 10000, UIA_ValuePatternId = 10002, UIA_SelectionItemPatternId = 10010, UIA_TogglePatternId = 10015;
constexpr EVENTID UIA_StructureChangedEventId = 20002, UIA_AutomationFocusChangedEventId = 20005, UIA_Invoke_InvokedEventId = 20009, UIA_LiveRegionChangedEventId = 20024;
constexpr VARTYPE VT_EMPTY = 0, VT_I4 = 3, VT_R8 = 5, VT_BSTR = 8, VT_BOOL = 11, VT_UNKNOWN = 13;
constexpr VARIANT_BOOL VARIANT_TRUE = -1, VARIANT_FALSE = 0;
struct VARIANT { VARTYPE vt; WORD wReserved1, wReserved2, wReserved3; union { LONG lVal; int intVal; BSTR bstrVal; VARIANT_BOOL boolVal; double dblVal; IUnknown* punkVal; }; };
struct SAFEARRAY { USHORT cDims, fFeatures; ULONG cbElements, cLocks; PVOID pvData; };
WINMOCK_FN(SAFEARRAY*, SafeArrayCreateVector) WINMOCK_FN(HRESULT, SafeArrayPutElement) WINMOCK_FN(HRESULT, SafeArrayDestroy) WINMOCK_FN(BSTR, SysAllocString)
WINMOCK_FN(void, SysFreeString) WINMOCK_FN(void, VariantInit) WINMOCK_FN(HRESULT, VariantClear)
struct IRawElementProviderSimple : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID patternId, IUnknown** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) = 0;
};
struct IRawElementProviderFragmentRoot;
struct IRawElementProviderFragment : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** pRetVal) = 0;
};
struct IRawElementProviderFragmentRoot : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** pRetVal) = 0;
};
struct IInvokeProvider : IUnknown { virtual HRESULT STDMETHODCALLTYPE Invoke() = 0; };
struct IToggleProvider : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Toggle() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* pRetVal) = 0;
};
struct ISelectionItemProvider : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Select() = 0;
    virtual HRESULT STDMETHODCALLTYPE AddToSelection() = 0;
    virtual HRESULT STDMETHODCALLTYPE RemoveFromSelection() = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* pRetVal) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** pRetVal) = 0;
};
WINMOCK_FN(HRESULT, UiaHostProviderFromHwnd) WINMOCK_FN(LRESULT, UiaReturnRawElementProvider) WINMOCK_FN(HRESULT, UiaRaiseAutomationEvent)
WINMOCK_FN(HRESULT, UiaRaiseStructureChangedEvent) WINMOCK_FN(HRESULT, UiaDisconnectProvider) WINMOCK_FN(BOOL, UiaClientsAreListening) WINMOCK_FN(HRESULT, UiaDisconnectAllProviders)

// ---- WinHTTP ----
constexpr DWORD WINHTTP_ACCESS_TYPE_DEFAULT_PROXY = 0, WINHTTP_ACCESS_TYPE_NO_PROXY = 1, WINHTTP_ACCESS_TYPE_NAMED_PROXY = 3, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY = 4;
#define WINHTTP_NO_PROXY_NAME nullptr
#define WINHTTP_NO_PROXY_BYPASS nullptr
#define WINHTTP_NO_REFERER nullptr
#define WINHTTP_DEFAULT_ACCEPT_TYPES nullptr
#define WINHTTP_NO_ADDITIONAL_HEADERS nullptr
#define WINHTTP_NO_REQUEST_DATA nullptr
#define WINHTTP_HEADER_NAME_BY_INDEX nullptr
#define WINHTTP_NO_OUTPUT_BUFFER nullptr
#define WINHTTP_NO_HEADER_INDEX nullptr
constexpr DWORD WINHTTP_FLAG_SECURE = 0x00800000, WINHTTP_FLAG_REFRESH = 0x100, WINHTTP_OPTION_REDIRECT_POLICY = 88, WINHTTP_OPTION_REDIRECT_POLICY_NEVER = 0,
    WINHTTP_OPTION_DISABLE_FEATURE = 63, WINHTTP_DISABLE_COOKIES = 1, WINHTTP_DISABLE_REDIRECTS = 2, WINHTTP_DISABLE_AUTHENTICATION = 4, WINHTTP_DISABLE_KEEP_ALIVE = 8,
    WINHTTP_OPTION_SECURE_PROTOCOLS = 84, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 = 0x800, WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 = 0x2000, WINHTTP_OPTION_AUTOLOGON_POLICY = 77,
    WINHTTP_AUTOLOGON_SECURITY_LEVEL_HIGH = 2, WINHTTP_QUERY_CONTENT_TYPE = 1, WINHTTP_QUERY_CONTENT_LENGTH = 5, WINHTTP_QUERY_STATUS_CODE = 19, WINHTTP_QUERY_LOCATION = 33,
    WINHTTP_QUERY_FLAG_NUMBER = 0x20000000, WINHTTP_OPTION_CONNECT_TIMEOUT = 3;
constexpr INTERNET_PORT INTERNET_DEFAULT_HTTP_PORT = 80, INTERNET_DEFAULT_HTTPS_PORT = 443;
WINMOCK_FN(HINTERNET, WinHttpOpen) WINMOCK_FN(HINTERNET, WinHttpConnect) WINMOCK_FN(HINTERNET, WinHttpOpenRequest) WINMOCK_FN(BOOL, WinHttpSendRequest)
WINMOCK_FN(BOOL, WinHttpReceiveResponse) WINMOCK_FN(BOOL, WinHttpQueryHeaders) WINMOCK_FN(BOOL, WinHttpQueryDataAvailable) WINMOCK_FN(BOOL, WinHttpReadData)
WINMOCK_FN(BOOL, WinHttpCloseHandle) WINMOCK_FN(BOOL, WinHttpSetOption) WINMOCK_FN(BOOL, WinHttpSetTimeouts) WINMOCK_FN(BOOL, WinHttpGetIEProxyConfigForCurrentUser)
WINMOCK_FN(BOOL, WinHttpAddRequestHeaders) WINMOCK_FN(BOOL, WinHttpCrackUrl)

// Local receipt drive classification (declarations only).
constexpr UINT DRIVE_UNKNOWN = 0, DRIVE_NO_ROOT_DIR = 1, DRIVE_REMOVABLE = 2, DRIVE_FIXED = 3, DRIVE_REMOTE = 4, DRIVE_CDROM = 5, DRIVE_RAMDISK = 6;
WINMOCK_FN(UINT, GetDriveTypeW)
constexpr DWORD FILE_READ_ATTRIBUTES = 0x80, FILE_FLAG_OPEN_REPARSE_POINT = 0x00200000, FILE_FLAG_BACKUP_SEMANTICS = 0x02000000,
                FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_REPARSE_POINT = 0x400;
struct BY_HANDLE_FILE_INFORMATION { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD dwVolumeSerialNumber, nFileSizeHigh, nFileSizeLow, nNumberOfLinks, nFileIndexHigh, nFileIndexLow; };
WINMOCK_FN(DWORD, GetFullPathNameW) WINMOCK_FN(BOOL, GetFileInformationByHandle)

WINMOCK_FN(BOOL, AdjustWindowRectExForDpi)
constexpr DWORD MONITOR_DEFAULTTONEAREST = 2;
constexpr UINT SPI_GETFONTSMOOTHING = 0x004A, SPI_GETFONTSMOOTHINGTYPE = 0x200A, FE_FONTSMOOTHINGCLEARTYPE = 2;
WINMOCK_FN(HMONITOR, MonitorFromWindow)
struct MONITORINFO { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; };
WINMOCK_FN(BOOL, GetMonitorInfoW)

constexpr UINT WM_DISPLAYCHANGE = 0x007E;
