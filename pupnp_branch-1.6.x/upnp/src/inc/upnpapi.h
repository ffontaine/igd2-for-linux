///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2000-2003 Intel Corporation 
// All rights reserved. 
//
// Redistribution and use in source and binary forms, with or without 
// modification, are permitted provided that the following conditions are met: 
//
// * Redistributions of source code must retain the above copyright notice, 
// this list of conditions and the following disclaimer. 
// * Redistributions in binary form must reproduce the above copyright notice, 
// this list of conditions and the following disclaimer in the documentation 
// and/or other materials provided with the distribution. 
// * Neither name of Intel Corporation nor the names of its contributors 
// may be used to endorse or promote products derived from this software 
// without specific prior written permission.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR 
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL INTEL OR 
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, 
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR 
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY 
// OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS 
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////

// File : upnpapi.h

#ifndef UPNPDK_H
#define UPNPDK_H

#include <gnutls/gnutls.h>
#include "upnp.h"
#include "client_table.h"
//#include "../ssdp/ssdplib.h"

#define MAX_INTERFACES 256

#define DEFAULT_INTERFACE 1

#define DEV_LIMIT 200

#define NUM_HANDLE 200

#define DEFAULT_MX 5

#define DEFAULT_MAXAGE 1800

#define DEFAULT_SOAP_CONTENT_LENGTH 16000
#define MAX_SOAP_CONTENT_LENGTH 32000

extern size_t g_maxContentLength;

// 30-second timeout
#define UPNP_TIMEOUT	30

typedef enum {HND_INVALID=-1,HND_CLIENT,HND_DEVICE} Upnp_Handle_Type;

// data used for SSL sessions
struct SSL_Info
{
    gnutls_session_t tls_session; // tls session
    gnutls_certificate_credentials_t tls_cred;
};

// Data to be stored in handle table for
struct Handle_Info
{
    Upnp_Handle_Type HType;
    Upnp_FunPtr  Callback; // Callback function pointer.
    char * Cookie;

    // Device Only
#ifdef INCLUDE_DEVICE_APIS
    char  DescURL[LINE_SIZE];   // URL for the use of SSDP
    char  SecureDescURL[LINE_SIZE];   // Secure URL for the use of SSDP, used mainly for advertising SECURELOCATION.UPNP.ORG
    char  DescXML[LINE_SIZE];   // XML file path for device 
                                //description

    int MaxAge;                 // Advertisement timeout
    IXML_Document *DescDocument;// Description parsed in 
                                //terms of DOM document 
    IXML_NodeList *DeviceList;  // List of devices in the 
                                //description document
    IXML_NodeList *ServiceList; // List of services in the 
                                // description document
    service_table ServiceTable; //table holding subscriptions and 
                                //URL information
    int MaxSubscriptions;
    int MaxSubscriptionTimeOut;
#endif
     
    // Client only
#ifdef INCLUDE_CLIENT_APIS
    client_subscription *ClientSubList; //client subscription list
    LinkedList SsdpSearchList; // active ssdp searches  
    struct SSL_Info *SSLInfo;
#endif
    int   aliasInstalled;       // 0 = not installed; otherwise installed
};

extern ithread_rwlock_t GlobalHndRWLock;
Upnp_Handle_Type GetHandleInfo(int Hnd, struct Handle_Info **HndInfo); 

#define HandleLock() HandleWriteLock()

#define HandleWriteLock()  \
	UpnpPrintf(UPNP_INFO, API, __FILE__, __LINE__, "Trying a write lock"); \
	ithread_rwlock_wrlock(&GlobalHndRWLock); \
	UpnpPrintf(UPNP_INFO, API, __FILE__, __LINE__, "Write lock acquired");

#define HandleReadLock()  \
	UpnpPrintf(UPNP_INFO, API, __FILE__, __LINE__, "Trying a read lock"); \
	ithread_rwlock_rdlock(&GlobalHndRWLock); \
	UpnpPrintf(UPNP_INFO, API, __FILE__, __LINE__, "Read lock acquired");

#define HandleUnlock() \
	UpnpPrintf(UPNP_INFO, API,__FILE__, __LINE__, "Trying Unlock"); \
	ithread_rwlock_unlock(&GlobalHndRWLock); \
	UpnpPrintf(UPNP_INFO, API, __FILE__, __LINE__, "Unlocked rwlock");

Upnp_Handle_Type GetClientHandleInfo(int *client_handle_out, 
                                     struct Handle_Info **HndInfo);
Upnp_Handle_Type GetDeviceHandleInfo(int *device_handle_out, 
                                     struct Handle_Info **HndInfo);


extern char LOCAL_HOST[LINE_SIZE];

extern unsigned short LOCAL_PORT;

extern TimerThread gTimerThread;
extern ThreadPool gRecvThreadPool;
extern ThreadPool gSendThreadPool;
extern ThreadPool gMiniServerThreadPool;
extern ThreadPool gHttpsServerThreadPool;

typedef enum {
    FUN_SUBSCRIBE,
    FUN_UNSUBSCRIBE,
    FUN_DK_NOTIFY,
    FUN_QUERY,
    FUN_ACTION,
    FUN_STATUS,
    FUN_DEVDESCRIPTION,
    FUN_SERVDESCRIPTION,
    FUN_MINI,
    FUN_RENEW} UpnpFunName;

struct  UpnpNonblockParam 
{ 
    UpnpFunName  FunName;
    int   Handle;
    int   TimeOut;
    char  VarName[NAME_SIZE];
    char  NewVal[NAME_SIZE];
    char  DevType[NAME_SIZE];
    char  DevId[NAME_SIZE];
    char  ServiceType[NAME_SIZE];
    char  ServiceVer[NAME_SIZE];
    char  Url[NAME_SIZE];
    Upnp_SID   SubsId;
    char  *Cookie;
    Upnp_FunPtr Fun;
	IXML_Document *Header;
    IXML_Document *Act;
    struct DevDesc *Devdesc;
};


extern virtualDirList *pVirtualDirList;
extern struct UpnpVirtualDirCallbacks virtualDirCallback;

extern gnutls_x509_crt_t client_crt;
extern gnutls_x509_privkey_t client_privkey;

typedef enum { WEB_SERVER_DISABLED, WEB_SERVER_ENABLED } WebServerState;

#define E_HTTP_SYNTAX -6

void InitHandleList();
int GetFreeHandle();
int FreeHandle(int Handle);
void UpnpThreadDistribution(struct UpnpNonblockParam * Param);


void AutoAdvertise(void *input); 
int getlocalhostname(char *out);

extern WebServerState bWebServerState;

#endif



/************************ END OF upnpapi.h **********************/
