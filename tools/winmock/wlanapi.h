// Declaration-only syntax aid. Native WLAN signatures/behavior are verified by Windows CI.
#pragma once
#include "winmock.h"
enum WLAN_INTERFACE_STATE { wlan_interface_state_not_ready, wlan_interface_state_connected,
    wlan_interface_state_ad_hoc_network_formed, wlan_interface_state_disconnecting, wlan_interface_state_disconnected,
    wlan_interface_state_associating, wlan_interface_state_discovering, wlan_interface_state_authenticating };
struct WLAN_INTERFACE_INFO { GUID InterfaceGuid; WCHAR strInterfaceDescription[256]; WLAN_INTERFACE_STATE isState; };
struct WLAN_INTERFACE_INFO_LIST { DWORD dwNumberOfItems, dwIndex; WLAN_INTERFACE_INFO InterfaceInfo[1]; };
enum DOT11_RADIO_STATE { dot11_radio_state_unknown, dot11_radio_state_on, dot11_radio_state_off };
struct WLAN_PHY_RADIO_STATE { DWORD dwPhyIndex; DOT11_RADIO_STATE dot11SoftwareRadioState, dot11HardwareRadioState; };
constexpr DWORD WLAN_MAX_PHY_INDEX = 64, WLAN_MAX_NAME_LENGTH = 256, DOT11_SSID_MAX_LENGTH = 32;
struct WLAN_RADIO_STATE { DWORD dwNumberOfPhys; WLAN_PHY_RADIO_STATE PhyRadioState[WLAN_MAX_PHY_INDEX]; };
enum WLAN_INTF_OPCODE { wlan_intf_opcode_radio_state = 4, wlan_intf_opcode_current_connection = 7 };
struct DOT11_SSID { ULONG uSSIDLength; UCHAR ucSSID[32]; };
enum DOT11_BSS_TYPE { dot11_BSS_type_infrastructure = 1 };
constexpr int DOT11_AUTH_ALGO_80211_OPEN = 1, DOT11_CIPHER_ALGO_NONE = 0;
using DOT11_AUTH_ALGORITHM = int;
using DOT11_CIPHER_ALGORITHM = int;
constexpr DWORD WLAN_AVAILABLE_NETWORK_HAS_PROFILE = 0x00000002;
struct WLAN_AVAILABLE_NETWORK { WCHAR strProfileName[WLAN_MAX_NAME_LENGTH]; DOT11_SSID dot11Ssid; DOT11_BSS_TYPE dot11BssType;
    ULONG uNumberOfBssids; BOOL bNetworkConnectable; DWORD wlanNotConnectableReason; ULONG uNumberOfPhyTypes;
    int dot11PhyTypes[8]; BOOL bMorePhyTypes; ULONG wlanSignalQuality; BOOL bSecurityEnabled;
    int dot11DefaultAuthAlgorithm, dot11DefaultCipherAlgorithm; DWORD dwFlags, dwReserved; };
struct WLAN_AVAILABLE_NETWORK_LIST { DWORD dwNumberOfItems, dwIndex; WLAN_AVAILABLE_NETWORK Network[1]; };
enum WLAN_CONNECTION_MODE { wlan_connection_mode_profile = 0, wlan_connection_mode_temporary_profile = 1 };
struct WLAN_CONNECTION_PARAMETERS { WLAN_CONNECTION_MODE wlanConnectionMode; LPCWSTR strProfile; DOT11_SSID* pDot11Ssid;
    void* pDesiredBssidList; DOT11_BSS_TYPE dot11BssType; DWORD dwFlags; };
struct WLAN_ASSOCIATION_ATTRIBUTES { DOT11_SSID dot11Ssid; DOT11_BSS_TYPE dot11BssType; };
struct WLAN_SECURITY_ATTRIBUTES { BOOL bSecurityEnabled, bOneXEnabled;
    DOT11_AUTH_ALGORITHM dot11AuthAlgorithm; DOT11_CIPHER_ALGORITHM dot11CipherAlgorithm; };
struct WLAN_CONNECTION_ATTRIBUTES { WLAN_INTERFACE_STATE isState; WLAN_CONNECTION_MODE wlanConnectionMode;
    WCHAR strProfileName[WLAN_MAX_NAME_LENGTH]; WLAN_ASSOCIATION_ATTRIBUTES wlanAssociationAttributes;
    WLAN_SECURITY_ATTRIBUTES wlanSecurityAttributes; };
WINMOCK_FN(DWORD, WlanOpenHandle) WINMOCK_FN(DWORD, WlanCloseHandle) WINMOCK_FN(DWORD, WlanEnumInterfaces)
WINMOCK_FN(void, WlanFreeMemory) WINMOCK_FN(DWORD, WlanQueryInterface) WINMOCK_FN(DWORD, WlanSetInterface)
WINMOCK_FN(DWORD, WlanScan) WINMOCK_FN(DWORD, WlanGetAvailableNetworkList) WINMOCK_FN(DWORD, WlanConnect)
constexpr DWORD WLAN_NOTIFICATION_SOURCE_NONE = 0, WLAN_NOTIFICATION_SOURCE_ACM = 8;
constexpr DWORD wlan_notification_acm_scan_complete = 7, wlan_notification_acm_scan_fail = 8;
struct WLAN_NOTIFICATION_DATA { DWORD NotificationSource, NotificationCode; GUID InterfaceGuid; DWORD dwDataSize; void* pData; };
using WLAN_NOTIFICATION_CALLBACK = void (WINAPI*)(WLAN_NOTIFICATION_DATA*, void*);
WINMOCK_FN(DWORD, WlanRegisterNotification)
