/* 
 * This file is part of Nokia InternetGatewayDevice v2 reference implementation 
 * Copyright © 2009 Nokia Corporation and/or its subsidiary(-ies).
 * Contact:mika.saaranen@nokia.com
 * 
 * This program is free software: you can redistribute it and/or modify 
 * it under the terms of the GNU (Lesser) General Public License as 
 * published by the Free Software Foundation, version 2 of the License. 
 * 
 * This program is distributed in the hope that it will be useful, 
 * but WITHOUT ANY WARRANTY; without even the implied warranty of 
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the 
 * GNU (Lesser) General Public License for more details. 
 * 
 * You should have received a copy of the GNU (Lesser) General Public 
 * License along with this program. If not, see http://www.gnu.org/licenses/. 
 * 
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "deviceprotection.h"
#include "gatedevice.h"
#include "globals.h"
#include "util.h"
#include <wpsutil/enrollee_state_machine.h>
#include <wpsutil/base64mem.h>
#include <wpsutil/cryptoutil.h>
#include <upnp/upnptools.h>
#include <upnp/upnp.h>

static int InitDP();
static void FreeDP();
static void message_received(struct Upnp_Action_Request *ca_event, int error, unsigned char *data, int len);
static int getSaltAndStoredForName(const char *nameUPPER, unsigned char **b64_salt, int *salt_len, unsigned char **b64_stored, int *stored_len);
static int createUserLoginChallengeResponse(struct Upnp_Action_Request *ca_event, const char *nameUPPER);
static int getValuesFromPasswdFile(const char *nameUPPER, unsigned char **b64_salt, int *salt_len, unsigned char **b64_stored, int *stored_len, int max_size);
static int putValuesToPasswdFile(const char *name, const unsigned char *b64_salt, const unsigned char *b64_stored);
static int updateValuesToPasswdFile(const char *nameUPPER, const unsigned char *b64_salt, const unsigned char *b64_stored, int delete_values);

// WPS state machine related stuff
static WPSuEnrolleeSM* esm;
static unsigned char* Enrollee_send_msg;
static int Enrollee_send_msg_len;
static WPSuStationInput input;

// address of control point which is executin introduction process
static char prev_addr[INET6_ADDRSTRLEN];

// Access Control List
static IXML_Document *ACLDoc = NULL;


/*
 * Document containing SSL session and username relationship. This in only for internal use of LinuxIGD.
 * Identity is either username or 20 bytes of certificate hash. It is trusted that no-one will never-ever
 * use username that could be some certificates hash.
 * Active attribute tells if session is currentlyt used. If value is 0, it can be later used in session
 * resumption
 *
 * <SIR>
 *  <session id="7467363" active="1">
 *      <identity>username</identity>
 *  </session>
 * </SIR>
 */
static IXML_Document *SIRDoc = NULL;


/**
 * Initialize DeviceProtection StateVariables for their default values.
 * 
 * @return void
 */
void DPStateTableInit()
{
    // DeviceProtection is ready for introduction
    SetupReady = 1;
    strcpy(SupportedProtocols, "<SupportedProtocols><Introduction><Name>WPS</Name></Introduction></SupportedProtocols>");   
    
    // init ACL
    ACLDoc = ixmlLoadDocument(ACL_XML);
    if (ACLDoc == NULL)
    {
        trace(1, "Couldn't load ACL (Access Control List) document which should locate here: %s\nExiting...\n",ACL_XML);
        UpnpFinish();
        exit(1);
    }
    
    // session-user relationships are stored in this
    SIRDoc = SIR_init();
    if (SIRDoc == NULL)
    {
        trace(1, "Couldn't load SIR document.\nSIR is LinuxIDG's internal structure for containing SSL-session-User relationships\nExiting...\n");
        UpnpFinish();
        exit(1);
    } 
}


/**
 * Update username,salt,stored values in password file.
 * Username and values can also be removed totally from file by putting delete_values as 1.
 * 
 * @param ca_event Upnp event struct.
 * @param targetRole Rolename that control point or user should have registerd in ACL
 * @return 0 if rolename is found and everything is ok, 1 if CP doesn't have privileges. Something else if error
 */
int checkCPPrivileges(struct Upnp_Action_Request *ca_event, const char *targetRole)
{
    int ret;
    int cert_size = 1000;
    unsigned char cert[cert_size];
    unsigned char hash[cert_size];
    
    if (ca_event->SSLSession == NULL)
        return 1;
    
    // 1. get certificate of client
    ret = UpnpGetClientCert(ca_event->SSLSession, cert, &cert_size);
    if (ret != UPNP_E_SUCCESS)
        return ret;
    
    // 2. create hash from certificate
    ret = wpsu_sha256(cert, cert_size, hash);
    if (ret < 0)
        return ret;
    
    // 3. create base64 from 20 first bytes of hash
    int identitylen = 20;
    int maxb64len = 2*identitylen; 
    int b64len = 0;        
    unsigned char *b64_identity = (unsigned char *)malloc(maxb64len);
    wpsu_bin_to_base64(identitylen, hash, &b64len, b64_identity, maxb64len);      
    
    // 4. fetch current identity of CP from SIR. Identity may be username or hash of certificate (== b64_identity)
    int active;
    char *identity = SIR_getIdentityOfSession(SIRDoc, (char *)b64_identity, &active);
    if (identity == NULL)
        return -1;
     
    // 5. check if target role is one of current roles of identity
    if ( ACL_doesIdentityHasRole(ACLDoc, identity, targetRole) )
        return 0;
    
    return 1;
}


/**
 * Initialize DeviceProtection. Create input data and state machine for WPS
 * 
 * @return int. 0 on success
 */
static int InitDP()
{   
    int err;
    char descDocFile[sizeof(g_vars.xmlPath)+sizeof(g_vars.descDocName)+2];
    unsigned char MAC[WPSU_MAC_LEN];
    memset(MAC, 0x00, WPSU_MAC_LEN);
    GetMACAddressStr(MAC, WPSU_MAC_LEN, g_vars.intInterfaceName);

    // manufacturer and device info is read from device description XML
    sprintf(descDocFile, "%s/%s", g_vars.xmlPath, g_vars.descDocName);
    IXML_Document *descDoc = ixmlLoadDocument(descDocFile);
    
    if (descDoc)
    {
        char *UUID = GetFirstDocumentItem(descDoc, "UDN");
        if (strlen(UUID) > 5)
        {
            UUID = UUID + 5; // remove text uuid: from beginning of string
        }
        if (strlen(UUID) > WPSU_MAX_UUID_LEN) // if uuid is too long, crop only allowed length from beginning
        {
            UUID[WPSU_MAX_UUID_LEN] = '\0';
        }
        
        err = wpsu_enrollee_station_input_add_device_info(&input, 
                                            g_vars.pinCode,
                                            GetFirstDocumentItem(descDoc, "manufacturer"),
                                            GetFirstDocumentItem(descDoc, "modelName"),
                                            GetFirstDocumentItem(descDoc, "modelNumber"),
                                            GetFirstDocumentItem(descDoc, "serialNumber"),
                                            GetFirstDocumentItem(descDoc, "friendlyName"),
                                            NULL,
                                            0,
                                            MAC,
                                            WPSU_MAC_LEN,
                                            (unsigned char*)UUID,
                                            strlen(UUID),
                                            NULL,
                                            0,
                                            NULL,
                                            0,
                                            WPSU_CONF_METHOD_LABEL, 
                                            WPSU_RFBAND_2_4GHZ);
        if (err != WPSU_E_SUCCESS)
            return err;                                                                             
    }
    else return UPNP_E_FILE_NOT_FOUND;
    
                                        
    // station has applications A, B and C
    //input.Apps = 3;
/*
    unsigned char UUID[WPSU_MAX_UUID_LEN];

    memset(UUID, 0xAA, WPSU_MAX_UUID_LEN);

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,WPSU_MAX_UUID_LEN,
        NULL,0,
        NULL,0);
    
    memset(UUID, 0xBB, WPSU_MAX_UUID_LEN);

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,WPSU_MAX_UUID_LEN,
        "B data from STA",strlen("B data from STA") + 1,
        NULL,0);

    memset(UUID, 0xCC, WPSU_MAX_UUID_LEN);
    

    err =  wpsu_enrollee_station_input_add_app(&input,
        UUID,WPSU_MAX_UUID_LEN,
        "C data from STA",strlen("C data from STA") + 1,
        NULL,0);
*/
    // create enrollee state machine
    esm = wpsu_create_enrollee_sm_station(&input, &err);
    if (err != WPSU_E_SUCCESS)
    {
        return err;
    }

    // set state variable SetupReady to false, meaning DP service is busy
    SetupReady = 0;
    IXML_Document *propSet = NULL;
    trace(3, "DeviceProtection SetupReady: %d", SetupReady);
    UpnpAddToPropertySet(&propSet, "SetupReady", "0");
    UpnpNotifyExt(deviceHandle, gateUDN, "urn:upnp-org:serviceId:DeviceProtection1", propSet);
    ixmlDocument_free(propSet);
    
    return 0;
}


/**
 * Deinitialize WPS state machine. Counterpart of InitDP()
 *
 * @return void
 */
static void FreeDP()
{
    int error;
    
    trace(2,"Finished DeviceProtection pairwise introduction process\n");
    wpsu_enrollee_station_input_free(&input);
    wpsu_cleanup_enrollee_sm(esm, &error);
    
    // DP is free
    SetupReady = 1;
    IXML_Document *propSet = NULL;
    trace(3, "DeviceProtection SetupReady: %d", SetupReady);
    UpnpAddToPropertySet(&propSet, "SetupReady", "1");
    UpnpNotifyExt(deviceHandle, gateUDN, "urn:upnp-org:serviceId:DeviceProtection1", propSet);
    ixmlDocument_free(propSet);
}


/**
 * WPS introduction uses this function. SendSetupMessage calls this.
 * When message M2, M2D, M4, M6, M8 or Done ACK is received, enrollee state machine is updated here
 * 
 * @param error Error code is passed through this.
 * @param data Received WPS introduction binary message
 * @oaram len Length of binary message
 * @return void
 */
static void message_received(struct Upnp_Action_Request *ca_event, int error, unsigned char *data, int len)
{
    int status;

    if (error)
    {
        trace(2,"DeviceProtection introduction message receive failure! Error = %d", error);
        return;
    }

    wpsu_update_enrollee_sm(esm, data, len, &Enrollee_send_msg, &Enrollee_send_msg_len, &status, &error);

    switch (status)
    {
        case WPSU_SM_E_SUCCESS:
        {
            trace(3,"DeviceProtection introduction last message received!\n");
            // Add CP certificate hash into ACL
            //unsigned char buffer[10 * 1024];
            //int buffer_size = sizeof (buffer);         
            //UpnpGetClientCert(ca_event->SSLSession, buffer, &buffer_size);
            
            FreeDP();
            break;
        }
        case WPSU_SM_E_SUCCESSINFO:
        {
            trace(3,"DeviceProtection introduction last message received M2D!\n");
            FreeDP();
            break;
        }

        case WPSU_SM_E_FAILURE:
        {
            trace(3,"DeviceProtection introduction error in state machine. Terminating...\n");
            FreeDP();
            break;
        }

        case WPSU_SM_E_FAILUREEXIT:
        {
            trace(3,"DeviceProtection introduction error in state machine. Terminating...\n");
            FreeDP();
            break;
        }
        default:
        {

        }
    }
}


/**
 * Get salt and stored values of user with nameUpper as username.
 * With getValuesFromPasswdFile(name, NULL, NULL, NULL, NULL, 0) it is possible to check 
 * if password file contains that specific username.
 * 
 * @param nameUPPER User name in upper case.
 * @param b64_salt Pointer to salt data read from file (base64 encoded)
 * @param salt_len Pointer to integer where length of salt is inserted
 * @oaram b64_stored Pointer to stored data read from file (base64 encoded)
 * @param stored_len Pointer to integer where length of stored is inserted
 * @param max_size Maximum space available for salt and stored. If they are longer than max_size, error is returned
 * @return -1 if fail, -2 if username is not found, -3 if salt or stored is too long, 0 on success
 */
static int getValuesFromPasswdFile(const char *nameUPPER, unsigned char **b64_salt, int *salt_len, unsigned char **b64_stored, int *stored_len, int max_size)
{
    // file is formatted as this (every user in own row):
    // Username,base64(SALT),base64(STORED)
    char line[200];
    char *name;
    char *temp;

    FILE *stream = fopen(g_vars.passwdFile, "r");
    if (!stream) return -1;
    
    while(fgets(line, 200, stream) != NULL) 
    {
        line[strlen(line)-1] = '\0';
    
        name = strtok(line, ",");
        if (name != NULL)
        {        
            name = toUpperCase(name);
            if (name == NULL)
            {
                fclose(stream);
                return -1;             
            }
            // if names match
            if ( strcmp(name,nameUPPER) == 0 )
            {
                fclose(stream);
                
                if (b64_salt)
                {
                    memset(*b64_salt, '\0', max_size);
                    temp = strtok(NULL, ",");
                    *salt_len = strlen(temp);
                    
                    if (*salt_len > max_size) return -3;
                    
                    memcpy(*b64_salt, temp, *salt_len);
                }
                if (b64_stored)
                {
                    memset(*b64_stored, '\0', max_size);
                    temp = strtok(NULL, ",");
                    *stored_len = strlen(temp);
                    
                    if (*stored_len > max_size) return -3;
                    
                    memcpy(*b64_stored, temp, *stored_len);
                }
                
                return 0;
            }
        }
    }
    
    fclose(stream);
    return -2; 
}

/**
 * Update username,salt,stored values in password file.
 * 
 * @param name User name in uppercase
 * @param b64_salt Pointer to salt data read from file (base64 encoded)
 * @oaram b64_stored Pointer to stored data read from file (base64 encoded)
 * @return -1 if fail -2 if username already exist, 0 on success
 */
static int putValuesToPasswdFile(const char *name, const unsigned char *b64_salt, const unsigned char *b64_stored)
{
    if (getValuesFromPasswdFile(name,NULL,NULL, NULL, NULL, 0) == 0)
        return -2;
    
    FILE *stream = fopen(g_vars.passwdFile, "a");
    if (!stream) return -1;
    
    fprintf(stream, "%s,%s,%s\n", name, b64_salt, b64_stored);
    
    fclose(stream);
    return 0;     
}

/**
 * Update username,salt,stored values in password file.
 * Username and values can also be removed totally from file by putting delete_values as 1.
 * 
 * @param name User name in uppercase
 * @param b64_salt Pointer to salt data read from file (base64 encoded)
 * @oaram b64_stored Pointer to stored data read from file (base64 encoded)
 * @param delete_values Use 0 to update existing values, 1 to delete.
 * @return -1 if fail or username is not found, 0 on success
 */
static int updateValuesToPasswdFile(const char *nameUPPER, const unsigned char *b64_salt, const unsigned char *b64_stored, int delete_values)
{
    // file is formatted as this (every user in own row):
    // Username,base64(SALT),base64(STORED)
    char line[200];
    char temp[200];
    char *name;
    int ret = -1;
    
    char tempfile[strlen(g_vars.passwdFile) + 5];
    strcpy(tempfile,g_vars.passwdFile);
    strcat(tempfile,".temp");
    
    // open 2 files, passwordfile which is read and temp file where lines are written.
    // if usernames match write new values in temp file.
    // Finally remove original passwordfile and rename temp file as original.
    FILE *in = fopen(g_vars.passwdFile, "r");
    if (!in) return -1;
    FILE *out = fopen(tempfile, "w");
    if (!out) 
    {
        fclose(in);      
        return -1;
    }
    
    while(fgets(line, 200, in) != NULL) 
    {
        line[strlen(line)-1] = '\0';
        strcpy(temp,line); // copy line, strtok modifies it
        
        name = strtok(line, ",");
        
        if (name != NULL)
        {
            name = toUpperCase(name);
            if (name == NULL)
            {
                fclose(in);
                fclose(out);
                return -1;             
            }
            // if names match        
            if ( strcmp(name,nameUPPER) == 0 )
            {
                // if we want to remove user from passwd file, lets not add him to temp file
                if (!delete_values)
                    fprintf(out, "%s,%s,%s\n", nameUPPER, b64_salt, b64_stored);
                ret = 0;
            }
            else
            {
                fprintf(out, "%s\n", temp);
            }
        }
    }
    
    fclose(in);
    fclose(out);
    
    // delete original password file
    remove(g_vars.passwdFile);
    // rename temp file is original password file
    rename(tempfile, g_vars.passwdFile); 

    return ret; 
}

/**
 * Get salt and stored values of user with nameUPPER as username.
 * Username "ADMIN" is an special case: if it is not found form password file, totally
 * new salt and stored values are creted for that username. Password used for creation 
 * of stored is stored in config file.
 *  
 * 
 * @param nameUPPER User name in upper case.
 * @param b64_salt Pointer to salt data read from file or newly created (base64 encoded)
 * @param salt_len Pointer to integer where length of salt is inserted
 * @oaram b64_stored Pointer to stored data read from file or newly created (base64 encoded)
 * @param stored_len Pointer to integer where length of stored is inserted
 * @return 0 on success
 */
static int getSaltAndStoredForName(const char *nameUPPER, unsigned char **b64_salt, int *salt_len, unsigned char **b64_stored, int *stored_len)
{
    int maxb64len = 2*DP_STORED_BYTES;     
    *b64_salt = (unsigned char *)malloc(maxb64len); 
    *b64_stored = (unsigned char *)malloc(maxb64len);
    
    int ret = getValuesFromPasswdFile(nameUPPER, b64_salt, salt_len, b64_stored, stored_len, maxb64len);
    
    if (ret != 0)
    {
        if (strcmp(nameUPPER,"ADMIN") == 0)
        {
            // create new salt and stored
            int name_len = strlen(nameUPPER);
            int namesalt_len = name_len + DP_SALT_BYTES;
            unsigned char namesalt[namesalt_len];
        
            // create SALT   
            unsigned char *salt = wpsu_createRandomValue(DP_SALT_BYTES);
            
            memcpy(namesalt, nameUPPER, name_len);
            memcpy(namesalt+name_len, salt, DP_SALT_BYTES);
            
            /* Create STORED = first 160 bits of the key T1, with T1 computed according to [PKCS#5] algorithm PBKDF2
                
                T1 is defined as the exclusive-or sum of the first c iterates of PRF applied to the concatenation 
                of the Password, Name, Salt, and four-octet block index (0x00000001) in big-endian format.  
                For DeviceProtection, the value for c is 5,000.  Name MUST be converted to upper-case, and 
                Password and Name MUST be encoded in UTF-8 format prior to invoking the PRF operation.  
                T1 = U1 \xor U2 \xor … \xor Uc
                where
                U1 = PRF(Password, Name || Salt || 0x0 || 0x0 || 0x0 || 0x1)
                U2 = PRF(Password, U1),
                …
                Uc = PRF(Password, Uc-1).
              
                NOTE1: SALT and STORED are created only if username is admin and passwordfile doesn't 
                
                NOTE2: wpsu_pbkdf2 goes through whole PBKDF2 algorithm, even if in this case only first block
                       is needed for result. First 160 bits are the same if all the data is processed or just 
                       the first block. (block size should be defined to 160bits => DP_STORED_BYTES = 8)
             */
            unsigned char bin_stored[DP_STORED_BYTES];
            ret = wpsu_pbkdf2(g_vars.adminPassword, strlen(g_vars.adminPassword), namesalt,
                            namesalt_len, DP_PRF_ROUNDS, DP_STORED_BYTES, bin_stored);
                            
            if (ret != 0) return ret;
            
            // SALT and STORED to base 64
            wpsu_bin_to_base64(DP_SALT_BYTES, salt, salt_len, *b64_salt, maxb64len);                                                
            wpsu_bin_to_base64(DP_STORED_BYTES, bin_stored, stored_len, *b64_stored, maxb64len);             

            
            // write values to password file
            ret = putValuesToPasswdFile(nameUPPER, *b64_salt, *b64_stored);
        }
    }
    
    return ret;
}

/**
 * Create userlogin challenge data and put it in upnp response struct.
 * GetUserLoginChallenge uses this.
 *
 * When Algorithm is the default value for DeviceProtection:1, the Salt and Challenge are derived as follows: 
 *  Salt = 16-octet random value used to hash Password into the STORED authentication value for each Name in the database.
 *  
 *  STORED = first 160 bits of the key T1, with T1 computed according to [PKCS#5] algorithm PBKDF2, with PRF=SHA-256.  A separate value of STORED is kept in the Device’s password file for each specific Name. 
 *  T1 is defined as the exclusive-or sum of the first c iterates of PRF applied to the concatenation of the Password, Name, Salt, and four-octet block index (0x00000001) in big-endian format.  For DeviceProtection, the value for c is 5,000.  Name MUST be converted to upper-case, and Password and Name MUST be encoded in UTF-8 format prior to invoking the PRF operation.  
 *  T1 = U1 \xor U2 \xor … \xor Uc
 *  where
 *  U1 = PRF(Password, Name || Salt || 0x0 || 0x0 || 0x0 || 0x1)
 *  U2 = PRF(Password, U1),
 *  …
 *  Uc = PRF(Password, Uc-1).
 * 
 *  Challenge = SHA-256(STORED || nonce).  Nonce is a fresh, random 128-bit value generated by the Device for each GetUserLoginChallenge() call.
 * 
 * @param ca_event Upnp event struct.
 * @param nameUPPER Username in uppercase
 * @return Upnp error code.
 */
static int createUserLoginChallengeResponse(struct Upnp_Action_Request *ca_event, const char *nameUPPER)
{
    int result = 0;
    unsigned char *b64_salt = NULL;
    unsigned char *b64_stored = NULL;
    int b64_salt_len = 0;
    int b64_stored_len = 0;

    if (getSaltAndStoredForName(nameUPPER, &b64_salt, &b64_salt_len, &b64_stored, &b64_stored_len) != 0)
    {
        trace(1, "Error creating/getting STORED value for user %s",nameUPPER);
        result = 501;
        addErrorData(ca_event, result, "Action Failed");
    }
    else
    {
        // stored to binary format
        unsigned char *bin_stored = (unsigned char *)malloc(b64_stored_len);
        int outlen;        
        wpsu_base64_to_bin(b64_stored_len, b64_stored, &outlen, bin_stored, DP_STORED_BYTES);


        // Create NONCE
        unsigned char *nonce = wpsu_createNonce(DP_NONCE_BYTES);
        
        unsigned char storednonce[DP_STORED_BYTES+DP_NONCE_BYTES];
        memcpy(storednonce, bin_stored, DP_STORED_BYTES);
        memcpy(storednonce+DP_STORED_BYTES, nonce, DP_NONCE_BYTES);
             
        // Create CHALLENGE = SHA-256(STORED || nonce)
        unsigned char challenge[DP_STORED_BYTES+DP_NONCE_BYTES];
        if ( wpsu_sha256(storednonce, DP_STORED_BYTES+DP_NONCE_BYTES, challenge) < 0 )
        {
            trace(1, "Error creating CHALLENGE value for %s",ca_event->ActionName);
            result = 501;
            addErrorData(ca_event, result, "Action Failed");                    
        }
        else
        {                
            // CHALLENGE to base64
            int maxb64len = 2*(DP_STORED_BYTES+DP_NONCE_BYTES); 
            int b64len = 0;        
    
            unsigned char *b64_challenge = (unsigned char *)malloc(maxb64len);
            wpsu_bin_to_base64(DP_STORED_BYTES+DP_NONCE_BYTES, challenge, &b64len, b64_challenge, maxb64len);               

            IXML_Document *ActionResult = NULL;
            ActionResult = UpnpMakeActionResponse(ca_event->ActionName, DP_SERVICE_TYPE,
                                        2,
                                        "Salt", b64_salt,
                                        "Challenge", b64_challenge);
                                        
            if (ActionResult)
            {
                ca_event->ActionResult = ActionResult;
                ca_event->ErrCode = UPNP_E_SUCCESS;        
            }
            else
            {
                trace(1, "Error parsing Response to %s",ca_event->ActionName);
                result = 501;
                addErrorData(ca_event, result, "Action Failed");
            }
            
            if (b64_challenge) free(b64_challenge);
        }
        if (bin_stored) free(bin_stored);
    }
    
    if (b64_salt) free(b64_salt);
    if (b64_stored) free(b64_stored);
    return result;
}


//-----------------------------------------------------------------------------
//
//                      DeviceProtection:1 Service Actions
//
//-----------------------------------------------------------------------------

/**
 * DeviceProtection:1 Action: SendSetupMessage
 * 
 * This action is used transport for pairwise introduction protocol messages.
 * Currently used protocol is WPS. Only one introduction process possible at same time.
 * 
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int SendSetupMessage(struct Upnp_Action_Request *ca_event)
{
    int result = 0;
    char resultStr[RESULT_LEN];
    char *protocoltype = NULL;
    char *inmessage = NULL;
    char curr_addr[INET6_ADDRSTRLEN];
    
    if ((protocoltype = GetFirstDocumentItem(ca_event->ActionRequest, "NewProtocolType")) &&
            (inmessage = GetFirstDocumentItem(ca_event->ActionRequest, "NewInMessage")))
    {    
        if (strcmp(protocoltype, "DeviceProtection:1") != 0)
        {
            trace(1, "Introduction protocol type must be DeviceProtection:1: Invalid NewProtocolType=%s\n",protocoltype);
            result = 703;
            addErrorData(ca_event, result, "Unknown Protocol Type");       
        } 
        
        inet_ntop(AF_INET, &ca_event->CtrlPtIPAddr, curr_addr, INET6_ADDRSTRLEN);
        if (result == 0 && SetupReady) // ready to start introduction
        {
            strcpy(prev_addr, curr_addr);
            // begin introduction
            trace(2,"Begin DeviceProtection pairwise introduction process. IP %s\n",prev_addr);
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
        else if (!SetupReady && (strcmp(prev_addr, curr_addr) == 0)) // continue started introduction
        {
            // to bin
            int b64msglen = strlen(inmessage);
            unsigned char *pBinMsg=(unsigned char *)malloc(b64msglen);
            int outlen;
            
            wpsu_base64_to_bin(b64msglen,(const unsigned char *)inmessage,&outlen,pBinMsg,b64msglen);

            // update state machine
            message_received(ca_event, 0, pBinMsg, outlen);
            if (pBinMsg) free(pBinMsg);
        }
        else // must be busy doing someone else's introduction process 
        {
            trace(1, "Busy with someone else's introduction process. IP %s\n",curr_addr);
            result = 708;
            addErrorData(ca_event, result, "Busy");         
        }
    }
    else
    {
        trace(1, "Failure in SendSetupMessage: Invalid Arguments!");
        result = 402;
        addErrorData(ca_event, result, "Invalid Args");
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
                 ca_event->ActionName, DP_SERVICE_TYPE, pB64Msg, ca_event->ActionName);
        ca_event->ActionResult = ixmlParseBuffer(resultStr);
        if (pB64Msg) free(pB64Msg);     
    }
    else if (result != 708)
    {
        FreeDP();       
    }
    
    if (inmessage) free(inmessage);
    if (protocoltype) free(protocoltype);
    return ca_event->ErrCode;
}


/**
 * DeviceProtection:1 Action: GetSupportedProtocols.
 *
 * Retrieve a list of setup protocols supported by the Device
 * 
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int GetSupportedProtocols(struct Upnp_Action_Request *ca_event)
{    
    IXML_Document *ActionResult = NULL;
    ActionResult = UpnpMakeActionResponse(ca_event->ActionName, DP_SERVICE_TYPE,
                                    1,
                                    "ProtocolList", SupportedProtocols);
                                    
    if (ActionResult)
    {
        ca_event->ActionResult = ActionResult;
        ca_event->ErrCode = UPNP_E_SUCCESS;        
    }
    else
    {
        trace(1, "Error parsing Response to GetSupportedProtocols");
        ca_event->ActionResult = NULL;
        ca_event->ErrCode = 501;
    }

    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: GetUserLoginChallenge.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int GetUserLoginChallenge(struct Upnp_Action_Request *ca_event)
{
    int result = 0;
    char *algorithm = NULL;
    char *name = NULL;
    char *nameUPPER = NULL;
    char *passwd = NULL;
    
    // check if connection was made over SSL    
    if (ca_event->SSLSession == NULL)
    {
        trace(1, "GetUserLoginChallenge: Connection wasn't made over SSL, terminating...");
        addErrorData(ca_event, 501, "Action Failed");
        return ca_event->ErrCode;
    }

    
    if ( (algorithm = GetFirstDocumentItem(ca_event->ActionRequest, "Algorithm") )
            && (name = GetFirstDocumentItem(ca_event->ActionRequest, "Name") ))
    {
        // check parameters
        if (strcmp(algorithm, "DeviceProtection:1") != 0)
        {
            trace(1, "Unknown algorithm %s",algorithm);
            result = 705;
            addErrorData(ca_event, result, "Invalid Algorithm");             
        }     
        else 
        {
            // name to uppercase
            nameUPPER = toUpperCase(name);
            if (nameUPPER == NULL)
            {
                trace(1, "Failed to convert name to upper case ");
                result = 501;
                addErrorData(ca_event, result, "Action Failed");
            }
            // check if user exits
            if ((strcmp(nameUPPER, "ADMIN") != 0) && (getValuesFromPasswdFile(nameUPPER, NULL,NULL,NULL,NULL,0) != 0))
            {
                trace(1, "Unknown username %s",nameUPPER);
                result = 706;
                addErrorData(ca_event, result, "Invalid Name");                 
            }
        }
        
        // parameters OK
        if (result == 0)
        {
            createUserLoginChallengeResponse(ca_event, nameUPPER);
        }
    }

    else
    {
        trace(1, "Failure in GetUserLoginChallenge: Invalid Arguments!");
        trace(1, "  Algotrithm: %s, Name: %s",algorithm,name);
        addErrorData(ca_event, 402, "Invalid Args");
    }
    
    if (algorithm) free(algorithm);
    if (name) free(name);    
    if (nameUPPER) free(nameUPPER);
    if (passwd) free(passwd);
    
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: UserLogin.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int UserLogin(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: UserLogout.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int UserLogout(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}


/**
 * DeviceProtection:1 Action: GetACLData.
 * 
 * Return the Device’s Access Control List (ACL).
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int GetACLData(struct Upnp_Action_Request *ca_event)
{
    char *ACL = ixmlDocumenttoString(ACLDoc);
    IXML_Document *ActionResult = NULL;
    
    if (ACL)
    {
        ActionResult = UpnpMakeActionResponse(ca_event->ActionName, DP_SERVICE_TYPE,
                                        1,
                                        "ACL", ACL);
    }
    else
    {
        trace(1, "Error reading ACL value");
        ca_event->ActionResult = NULL;
        ca_event->ErrCode = 501;
        return ca_event->ErrCode;
    }    
                                    
    if (ActionResult)
    {
        ca_event->ActionResult = ActionResult;
        ca_event->ErrCode = UPNP_E_SUCCESS;        
    }
    else
    {
        trace(1, "Error parsing Response to GetSupportedProtocols");
        ca_event->ActionResult = NULL;
        ca_event->ErrCode = 501;
    }

    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: AddRolesForIdentity.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int AddRolesForIdentity(struct Upnp_Action_Request *ca_event)
{
    int result = 0;
    char *identity = NULL;
    char *rolelist = NULL;
    
    if ( (identity = GetFirstDocumentItem(ca_event->ActionRequest, "Identity") )
            && (rolelist = GetFirstDocumentItem(ca_event->ActionRequest, "RoleList") ))
    {
        // try first to add roles for username
        result = ACL_addRolesForUser(ACLDoc, identity, rolelist);

        if (result == ACL_USER_ERROR)
        {
            // identity wasn't username, so it must be control point hash
            result = ACL_addRolesForCP(ACLDoc, identity, rolelist);
        }
        
        if (result == ACL_USER_ERROR)
        {
            // ok, identity wasn't username or hash
            trace(1, "AddRolesForIdentity: Unknown identity %s",identity);
            result = 706;
            addErrorData(ca_event, result, "Unknown Identity");
        }
        else if (result == ACL_ROLE_ERROR)
        {
            trace(1, "AddRolesForIdentity: Invalid rolelist received %s",rolelist);
            result = 707;
            addErrorData(ca_event, result, "Invalid RoleList");
        }
        else if (result != ACL_SUCCESS)
        {
            trace(1, "AddRolesForIdentity: Failed to add roles '%s' for identity '%s'",rolelist,identity);
            result = 501;
            addErrorData(ca_event, result, "Action Failed");
        }
        
        // all is well
        if (result == 0)
        {
            // write ACL in filesystem
            writeDocumentToFile(ACLDoc, ACL_XML);
            ca_event->ActionResult = UpnpMakeActionResponse(ca_event->ActionName, DP_SERVICE_TYPE,
                                        0, NULL);                                        
            ca_event->ErrCode = UPNP_E_SUCCESS;   
        }
        
    }
    else
    {
        trace(1, "AddRolesForIdentity: Invalid Arguments!");
        trace(1, "  Identity: %s, RoleList: %s",identity,rolelist);
        addErrorData(ca_event, 402, "Invalid Args");
    }
  
    if (identity) free(identity);
    if (rolelist) free(rolelist);
    
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: RemoveRolesForIdentity.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int RemoveRolesForIdentity(struct Upnp_Action_Request *ca_event)
{
    int result = 0;
    char *identity = NULL;
    char *rolelist = NULL;
    
    if ( (identity = GetFirstDocumentItem(ca_event->ActionRequest, "Identity") )
            && (rolelist = GetFirstDocumentItem(ca_event->ActionRequest, "RoleList") ))
    {
        // check that CP has Admin privileges
        
        
        // try first to remove roles from username
        result = ACL_removeRolesFromUser(ACLDoc, identity, rolelist);
        if (result == ACL_USER_ERROR)
        {
            // identity wasn't username, so it must be control point hash
            result = ACL_removeRolesFromCP(ACLDoc, identity, rolelist);
        }
        
        if (result == ACL_USER_ERROR)
        {
            // ok, identity wasn't username or hash
            trace(1, "RemoveRolesForIdentity: Unknown identity %s",identity);
            result = 706;
            addErrorData(ca_event, result, "Unknown Identity");
        }
        else if (result == ACL_ROLE_ERROR)
        {
            trace(1, "RemoveRolesForIdentity: Invalid rolelist received %s",rolelist);
            result = 707;
            addErrorData(ca_event, result, "Invalid RoleList");
        }
        else if (result != ACL_SUCCESS)
        {
            trace(1, "RemoveRolesForIdentity: Failed to add roles '%s' for identity '%s'",rolelist,identity);
            result = 501;
            addErrorData(ca_event, result, "Action Failed");
        }
        
        // all is well
        if (result == 0)
        {
            // write ACL in filesystem
            writeDocumentToFile(ACLDoc, ACL_XML);
            ca_event->ActionResult = UpnpMakeActionResponse(ca_event->ActionName, DP_SERVICE_TYPE,
                                        0, NULL);                                        
            ca_event->ErrCode = UPNP_E_SUCCESS;   
        }
        
    }
    else
    {
        trace(1, "RemoveRolesForIdentity: Invalid Arguments!");
        trace(1, "  Identity: %s, RoleList: %s",identity,rolelist);
        addErrorData(ca_event, 402, "Invalid Args");
    }
  
    if (identity) free(identity);
    if (rolelist) free(rolelist);
    
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: GetCurrentRole.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int GetCurrentRoles(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: AddLoginData.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int AddLoginData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}


/**
 * DeviceProtection:1 Action: RemoveLoginData.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int RemoveLoginData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: AddIdentityData.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int AddIdentityData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

/**
 * DeviceProtection:1 Action: RemoveIdentityData.
 *
 * @param ca_event Upnp event struct.
 * @return Upnp error code.
 */
int RemoveIdentityData(struct Upnp_Action_Request *ca_event)
{
    return ca_event->ErrCode;
}

