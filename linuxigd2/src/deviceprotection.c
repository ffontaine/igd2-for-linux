#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "deviceprotection.h"
#include "globals.h"
#include "util.h"
#include <wpsutil/enrollee_state_machine.h>
#include <wpsutil/base64mem.h>


static int InitDP();
static void FreeDP();
static int message_received(int error, unsigned char *data, int len, void* control);


// TODO Should these be in main? Or somewhere else. 
WPSuEnrolleeSM* esm;
unsigned char* Enrollee_send_msg;
int Enrollee_send_msg_len;
WPSuStationInput input;

int InitDP()
{   
    // TODO these values. What those should be and move to config file 
    // TODO: Should start new thread for multiple simultanious registration processes?
    int err;
    
    unsigned char MAC[SC_MAC_LEN];
    memset(MAC, 0xAB, SC_MAC_LEN);

    err = wpsu_enrollee_station_input_add_device_info(&input, 
                                        "stasecret",
                                        "TestManufacturer", 
                                        "TestModelName",
                                        "TestModelNumber", 
                                        "TestSerialNumber", 
                                        "TestDeviceName", 
                                        NULL,
                                        0,
                                        MAC,
                                        SC_MAC_LEN,
                                        "TestUUID",
                                        8,
                                        NULL,
                                        0,
                                        NULL,
                                        0,
                                        SC_CONF_METHOD_LABEL, 
                                        SC_RFBAND_2_4GHZ);
                                        
    // station has applications A, B and C
    input.Apps = 3;

    unsigned char UUID[SC_MAX_UUID_LEN];

    memset(UUID, 0xAA, SC_MAX_UUID_LEN);

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,SC_MAX_UUID_LEN,
        NULL,0,
        NULL,0);
    
    memset(UUID, 0xBB, SC_MAX_UUID_LEN);

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,SC_MAX_UUID_LEN,
        "B data from STA",strlen("B data from STA") + 1,
        NULL,0);

    memset(UUID, 0xCC, SC_MAX_UUID_LEN);
    

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,SC_MAX_UUID_LEN,
        "C data from STA",strlen("C data from STA") + 1,
        NULL,0);

    // create enrollee state machine
    esm = wpsu_create_enrollee_sm_station(&input, &err);
    printf ("wpsu_create_enrollee_sm_station %d\n",err);

    // set state variable SetupReady to false, meaning DP service is busy
    SetupReady = 0;
    return 0;
}

void FreeDP()
{
    int error;
    free(input.AppsList[1].Data);
    free(input.AppsList[2].Data);
    wpsu_cleanup_enrollee_sm(esm, &error);
    
    // DP is free
    SetupReady = 1;
}

/**
 * When message M2, M2D, M4, M6, M8 or Done ACK is received, enrollee state machine is updated here
 */
static int message_received(int error, unsigned char *data, int len, void* control)
{
    int status;

    if (error)
    {
        trace(2,"Message receive failure! Error = %d", error);
        return error;
    }

    wpsu_update_enrollee_sm(esm, data, len, &Enrollee_send_msg, &Enrollee_send_msg_len, &status, &error);

    switch (status)
    {
        case SC_E_SUCCESS:
        {
            trace(3,"Last message received!\n");
            FreeDP();
            break;
        }
        case SC_E_SUCCESSINFO:
        {
            trace(3,"Last message received M2D!\n");
            FreeDP();
            break;
        }

        case SC_E_FAILURE:
        {
            trace(3,"Error in state machine. Terminating...\n");
            FreeDP();
            break;
        }

        case SC_E_FAILUREEXIT:
        {
            trace(3,"Error in state machine. Terminating...\n");
            FreeDP();
            break;
        }
        default:
        {

        }
    }
    return 0;
}



/**
 * Action: SendSetupMessage.
 *
 * Return M1 message for sender of action.
 */
int SendSetupMessage(struct Upnp_Action_Request *ca_event)
{
    int result = 0;
    char resultStr[RESULT_LEN];
    char *protocoltype = NULL;
    char *inmessage = NULL;
    
    protocoltype = GetFirstDocumentItem(ca_event->ActionRequest, "NewProtocolType");
    
    if (strcmp(protocoltype, "DeviceProtection:1") != 0)
    {
        trace(1, "Introduction protocol type must be DeviceProtection:1: Invalid NewProtocolType=%s\n",protocoltype);
        result = 703;
        addErrorData(ca_event, result, "Unknown Protocol Type");       
    }    


    if (SetupReady)
    {
        // begin introduction
        InitDP();
        // start the state machine and create M1
        wpsu_start_enrollee_sm(esm, &Enrollee_send_msg, &Enrollee_send_msg_len, &result);
        if (result != WPSU_E_SUCCESS)
        {
            trace(1, "Failed to start WPS state machine. Returned %d\n",result);
            result = 704;
            addErrorData(ca_event, result, "Processing Error");               
        }
    }
    else
    {    
        // continue introduction
        inmessage = GetFirstDocumentItem(ca_event->ActionRequest, "NewInMessage");

        // to bin
        int b64msglen = strlen(inmessage);
        unsigned char *pBinMsg=(unsigned char *)malloc(b64msglen);
        int outlen;
        
        wpsu_base64_to_bin(b64msglen,(const unsigned char *)inmessage,&outlen,pBinMsg,b64msglen);
        
        //printf("Message in bin: %s\n",pBinMsg);
        // update state machine
        message_received(0, pBinMsg, outlen, NULL);
        if (pBinMsg) free(pBinMsg);
    }

    if (result == 0)
    {
        // response (next message) to base64   
        int maxb64len = 2*Enrollee_send_msg_len; 
        int b64len = 0;    
        unsigned char *pB64Msg = (unsigned char *)malloc(maxb64len); 
        wpsu_bin_to_base64(Enrollee_send_msg_len,Enrollee_send_msg, &b64len, pB64Msg,maxb64len);
        
        trace(3,"Send response for SendSetupMessage request\n");
        
        ca_event->ErrCode = UPNP_E_SUCCESS;
        snprintf(resultStr, RESULT_LEN, "<u:%sResponse xmlns:u=\"%s\">\n<NewOutMessage>%s</NewOutMessage>\n</u:%sResponse>",
                 ca_event->ActionName, "urn:schemas-upnp-org:service:DeviceProtection:1", pB64Msg, ca_event->ActionName);
        ca_event->ActionResult = ixmlParseBuffer(resultStr);
        if (pB64Msg) free(pB64Msg);     
    }
    else
    {
        FreeDP();       
    }
    
    if (inmessage) free(inmessage);
    if (protocoltype) free(protocoltype);
    return ca_event->ErrCode;
}




/**
 * Action: GetSupportedProtocols.
 *
 */
int GetSupportedProtocols(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: GetSessionLoginChallenge.
 *
 */
int GetSessionLoginChallenge(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: SessionLogin.
 *
 */
int SessionLogin(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: SessionLogout.
 *
 */
int SessionLogout(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}


/**
 * Action: GetACLData.
 *
 */
int GetACLData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: AddRolesForIdentity.
 *
 */
int AddRolesForIdentity(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: RemoveRolesForIdentity.
 *
 */
int RemoveRolesForIdentity(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: AddLoginData.
 *
 */
int AddLoginData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}


/**
 * Action: RemoveLoginData.
 *
 */
int RemoveLoginData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: AddIdentityData.
 *
 */
int AddIdentityData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * Action: RemoveIdentityData.
 *
 */
int RemoveIdentityData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}
