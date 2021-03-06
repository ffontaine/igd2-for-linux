/** 
 * This file is part of Nokia InternetGatewayDevice v2 reference implementation
 * Copyright © 2009 Nokia Corporation and/or its subsidiary(-ies).
 * Contact: mika.saaranen@nokia.com
 * Developer(s): jaakko.pasanen@tieto.com, opensource@tieto.com
 *  
 * This file is part of igd2-for-linux project
 * Copyright © 2011-2012 France Telecom / Orange.
 * Contact: fabrice.fontaine@orange.com
 * Developer(s): fabrice.fontaine@orange.com, rmenard.ext@orange.com
 * 
 * This program is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU General Public License as published by 
 * the Free Software Foundation, either version 2 of the License, or 
 * (at your option) any later version. 
 * 
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
 * GNU General Public License for more details. 
 * 
 * You should have received a copy of the GNU General Public License 
 * along with this program, see the /doc directory of this program. If 
 * not, see http://www.gnu.org/licenses/. 
 * 
 */
 
#ifndef _GATEDEVICE_H_
#define _GATEDEVICE_H_ 1

#include <upnp/upnp.h>
#include "threadutil/TimerThread.h"
#include "util.h"

// Thread which contains all kind of timers and threads used in gatedevice.c and deviceprotection.c
extern TimerThread gExpirationTimerThread;

// IGD Device Globals
extern UpnpDevice_Handle deviceHandle;
extern UpnpDevice_Handle deviceHandleIPv6;
extern UpnpDevice_Handle deviceHandleIPv6UlaGua;
extern char *gateUDN;
extern char *wanUDN;
extern char *wanConnectionUDN;
extern char *lanUDN;

// Linked list for portmapping entries
extern struct portMap *pmlist_Head;
extern struct portMap *pmlist_Current;

// WanIPConnection Actions
int EventHandler(Upnp_EventType EventType, const void *Event, void *Cookie);
int StateTableInit(char *descDocUrl);
void AcceptSubscriptionExtForIPv4andIPv6(const char *DevID, const char *ServID,
                                        IXML_Document *PropSet, Upnp_SID SubsId);
void NotifyExtForIPv4AndIPv6(const char *DevID, const char *ServID,
                            IXML_Document *PropSet);
int HandleSubscriptionRequest(UpnpSubscriptionRequest *sr_event);
int HandleGetVarRequest(UpnpStateVarRequest *gv_event);
int HandleActionRequest(UpnpActionRequest *ca_event);

int GetConnectionTypeInfo(UpnpActionRequest *ca_event);
int GetNATRSIPStatus(UpnpActionRequest *ca_event);
int SetConnectionType(UpnpActionRequest *ca_event);
int SetAutoDisconnectTime(UpnpActionRequest *ca_event);
int SetIdleDisconnectTime(UpnpActionRequest *ca_event);
int SetWarnDisconnectDelay(UpnpActionRequest *ca_event);
int GetAutoDisconnectTime(UpnpActionRequest *ca_event);
int GetIdleDisconnectTime(UpnpActionRequest *ca_event);
int GetWarnDisconnectDelay(UpnpActionRequest *ca_event);
int RequestConnection(UpnpActionRequest *ca_event);
int GetTotal(UpnpActionRequest *ca_event, stats_t stat);
int GetCommonLinkProperties(UpnpActionRequest *ca_event);
int InvalidAction(UpnpActionRequest *ca_event);
int GetStatusInfo(UpnpActionRequest *ca_event);
int AddPortMapping(UpnpActionRequest *ca_event);
int GetGenericPortMappingEntry(UpnpActionRequest *ca_event);
int GetSpecificPortMappingEntry(UpnpActionRequest *ca_event);
int GetExternalIPAddress(UpnpActionRequest *ca_event);
int DeletePortMapping(UpnpActionRequest *ca_event);
int DeletePortMappingRange(UpnpActionRequest *ca_event);
int AddAnyPortMapping(UpnpActionRequest *ca_event);
int GetListOfPortmappings(UpnpActionRequest *ca_event);
int ForceTermination(UpnpActionRequest *ca_event);
int RequestTermination(UpnpActionRequest *ca_event);

// WANEthernetLinkConfig Actions
int GetEthernetLinkStatus (UpnpActionRequest *ca_event);

// Definitions for mapping expiration timer thread
#define THREAD_IDLE_TIME 5000
#define JOBS_PER_THREAD 10
#define MIN_THREADS 2
#define MAX_THREADS 12

int ExpirationTimerThreadInit(void);
int ExpirationTimerThreadShutdown(void);
int ScheduleMappingExpiration(struct portMap *mapping, const char *DevUDN, const char *ServiceID);
int CancelMappingExpiration(int eventId);
void DeleteAllPortMappings(void);
int AddNewPortMapping(UpnpActionRequest *ca_event, char* new_enabled, long int leaseDuration,
                     char* new_remote_host, char* new_external_port, char* new_internal_port,
                     char* new_protocol, char* new_internal_client, char* new_port_mapping_description,
                     int is_update);
int createAutoDisconnectTimer(void);
void DisconnectWAN(void *input);
int createEventUpdateTimer(void);
void UpdateEvents(void *input);
int EthernetLinkStatusEventing(IXML_Document *propSet);
int ExternalIPAddressEventing(IXML_Document *propSet);
int ConnectionStatusEventing(IXML_Document *propSet);
int ConnectionTermination(UpnpActionRequest *ca_event, long int disconnectDelay);
int AuthorizeControlPoint(UpnpActionRequest *ca_event, int managed, int addError);

int WANIPv6FirewallStatusEventing(IXML_Document *propSet);

// Definition for authorizing control point
typedef enum
{
    CONTROL_POINT_AUTHORIZED,
    CONTROL_POINT_HALF_AUTHORIZED,
    CONTROL_POINT_NOT_AUTHORIZED
} authorization_levels;

#endif //_GATEDEVICE_H
