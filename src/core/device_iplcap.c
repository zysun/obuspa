/*
 *
 * Copyright (C) 2026, Broadband Forum
 * Copyright (C) 2026, Vantiva Technologies SAS
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

/**
 * \file device_iplcap.c
 *
 * Implements IPLayerCapacity diagnostics
 * NOTE: IPLCapThreadMain (and most functions within this file) run in a separate thread from the rest of USP Agent
 *       Therefore care must be taken in which functions are called. Functions called in USP Agent must be
 *       thread-safe or mutex protected
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/wait.h>

#include "common_defs.h"
#include "os_utils.h"
#include "kv_vector.h"
#include "dm_access.h"
#include "json.h"
#include "text_utils.h"
#include "msg_handler.h"
#include "path_resolver.h"

#ifndef REMOVE_IP_CAPACITY_DIAG
//------------------------------------------------------------------------------------
// Maximum number of concurrent connections that the code supports
// NOTE: This limit is actually set by the udpst client executable (24). The value defined here must not exceed udpst's capabilities.
#define IPL_MAX_CONNECTIONS 24
#define IPL_MAX_CONNECTIONS_STR      TO_STR(IPL_MAX_CONNECTIONS)

// Maximum number of instances in the IPLayerCapacity().IncrementalResult.{i} output arguments that the code supports
// NOTE: This limit is actually set by the lower of the value supported by the udpst client executable (3600) and TR-181's
// range for the NumberTestSubIntervals input argument (100).
#define IPL_MAX_INC_RESULT 100
#define IPL_MAX_INC_RESULT_STR       TO_STR(IPL_MAX_INC_RESULT)

//------------------------------------------------------------------------------------
// Location of the IP Layer Capacity Auth table within the data model
#define DEVICE_IPLC_AUTH_ROOT "Device.IP.Diagnostics.IPLayerCapacityAuthCode"

//------------------------------------------------------------------------------------
// Input conditions for IPLayerCapacity test
typedef struct
{
    int request_instance;   // Instance number of this operation in the Device.LocalAgent.Request table
    str_vector_t argv;      // Arguments to pass to udpst
} iplcap_input_cond_t;

//------------------------------------------------------------------------------------
// Output results of IPLayerCapacity test
typedef struct
{
  char *json;
} iplcap_output_res_t;

//------------------------------------------------------------------------------------
// Structure to hold capabilities of udpst executable, parsed from its JSON response
typedef struct
{
    char sw_version[65];
    char protocol_version[65];
    char metrics[128];
} iplc_caps_t;

static iplc_caps_t iplc_caps;

//------------------------------------------------------------------------------------
// Buffer to hold error message used by this diagnostic thread
static char iplcap_err_msg[USP_ERR_MAXLEN] = { 0 };

//------------------------------------------------------------------------------------
// Array of valid input arguments
static char *iplcap_input_args[] =
{
    "Role",
    "ProtocolVersion",
    "FlowCount",
    "MaximumFlows",
    "JumboFramesPermitted",
    "MTU",
    "UDPPayloadContent",
    "MaximumTestBandwidth",
    "NumberFirstModeTestSubIntervals",
    "IPDVEnable",
    "TestType",
    "ReordDupIgnoreEnable",
    "AuthenticationEnabled",
    "AuthenticationCode",
    "AuthenticationKeyId",
    "AuthenticationKeyFileLocation",
    "DSCP",
    "StartSendingRateIndex",
    "TestSubInterval",
    "NumberTestSubIntervals",
    "RateAdjAlgorithm",
    "LowerThresh",
    "UpperThresh",
    "StatusFeedbackInterval",
    "SlowAdjThresh",
    "HighSpeedDelta",
    "SeqErrThresh",
    "Interface",
    "LocalInterfaceRateIncluded",
    "ServerList",
};

//------------------------------------------------------------------------------------
// Array of valid output arguments
static char *iplcap_output_args[] =
{
    "Status",
    "StatusCode",
    "StatusMessage",
    "BOMTime",
    "EOMTime",
    "TmaxUsed",
    "TestInterval",
    "TmaxRTTUsed",
    "TimestampResolutionUsed",
    "MaxIPLayerCapacity",
    "TimeOfMax",
    "MaxETHCapacityNoFCS",
    "MaxETHCapacityWithFCS",
    "MaxETHCapacityWithFCSVLAN",
    "LossRatioAtMax",
    "RTTRangeAtMax",
    "PDVRangeAtMax",
    "MinOnewayDelayAtMax",
    "ReorderedRatioAtMax",
    "ReplicatedRatioAtMax",
    "IPLayerCapacitySummary",
    "LossRatioSummary",
    "RTTRangeSummary",
    "PDVRangeSummary",
    "MinOnewayDelaySummary",
    "MinRTTSummary",
    "ReorderedRatioSummary",
    "ReplicatedRatioSummary",
    "RTTMinAtMax",
    "RTTMaxAtMax",
    "InterfaceEthMbpsAtMax",
    "InterfaceEthMbpsSummary",

    "IncrementalResult.{i}.IPLayerCapacity",
    "IncrementalResult.{i}.TimeOfSubInterval",
    "IncrementalResult.{i}.LossRatio",
    "IncrementalResult.{i}.RTTRange",
    "IncrementalResult.{i}.PDVRange",
    "IncrementalResult.{i}.MinOnewayDelay",
    "IncrementalResult.{i}.ReorderedRatio",
    "IncrementalResult.{i}.ReplicatedRatio",
    "IncrementalResult.{i}.InterfaceEthMbpsAtMax",

    "ModalResult.{i}.MaxIPLayerCapacity",
    "ModalResult.{i}.TimeOfMax",
    "ModalResult.{i}.MaxETHCapacityNoFCS",
    "ModalResult.{i}.MaxETHCapacityWithFCS",
    "ModalResult.{i}.MaxETHCapacityWithFCSVLAN",
    "ModalResult.{i}.LossRatioAtMax",
    "ModalResult.{i}.RTTRangeAtMax",
    "ModalResult.{i}.PDVRangeAtMax",
    "ModalResult.{i}.MinOnewayDelayAtMax",
    "ModalResult.{i}.ReorderedRatioAtMax",
    "ModalResult.{i}.ReplicatedRatioAtMax",
    "ModalResult.{i}.InterfaceEthMbpsAtMax",
};

//------------------------------------------------------------------------------------
// Forward declarations. Note these are not static, because we need them in the symbol table for USP_LOG_Callstack() to show them
int IPLCap_Operate(dm_req_t *req, kv_vector_t *input_args, int instance);
void *IPLCapThreadMain(void *param);
int IPLCap_SaveResults(iplcap_input_cond_t *cond, iplcap_output_res_t *res, kv_vector_t *output_args);
int IPLCap_Execute(iplcap_input_cond_t *cond, iplcap_output_res_t *res);
void AddJsonArrayChildToOutputArgs(JsonNode *element, char *key, kv_vector_t *output_args, char *arg_object, int instance, char *arg_name, char *number_format);
int AddJsonChildToOutputArgs(JsonNode *obj, char *key, kv_vector_t *output_args, char *arg_name, char *number_format);
void IPLCap_ExtractCapabilities(JsonNode *root);
int Get_IPLCap_SoftwareVersion(dm_req_t *req, char *buf, int len);
int Get_IPLCap_ProtocolVersion(dm_req_t *req, char *buf, int len);
int Get_IPLCap_SupportedMetrics(dm_req_t *req, char *buf, int len);
int LookupKeyByAuthKeyID(int auth_key_id, char *buf, int len, combined_role_t *combined_role);
void IPLCap_AddCmdArgs(str_vector_t *sv, char *cmd_switch, const char *fmt, ...);
int AutoPopulate_AuthenticationKeyID(dm_req_t *req, char *buf, int len);
int Validate_AuthenticationKeyID(dm_req_t *req, char *value);

/*********************************************************************//**
**
** DEVICE_IPLCAP_Init
**
** Initialises this component, and registers all parameters which it implements
**
** \param   None
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int DEVICE_IPLCAP_Init(void)
{
    int err = USP_ERR_OK;

    // Register IPLayerCapacity() command
    err |= USP_REGISTER_Param_Constant("Device.IP.Diagnostics.IPLayerCapacitySupported", "true", DM_BOOL);
    err |= USP_REGISTER_AsyncOperation("Device.IP.Diagnostics.IPLayerCapacity()", IPLCap_Operate, NULL);

    err |= USP_REGISTER_OperationArguments("Device.IP.Diagnostics.IPLayerCapacity()", iplcap_input_args, NUM_ELEM(iplcap_input_args),
                                                                             iplcap_output_args, NUM_ELEM(iplcap_output_args));

    err |= USP_REGISTER_AsyncOperation_MaxConcurrency("Device.IP.Diagnostics.IPLayerCapacity()", 1);

    // Register IPLayerCapacityAuth table
    err |= USP_REGISTER_Object(DEVICE_IPLC_AUTH_ROOT ".{i}", NULL, NULL, NULL,
                                                        NULL, NULL, NULL);
    err |= USP_REGISTER_Param_NumEntries(DEVICE_IPLC_AUTH_ROOT "NumberOfEntries", DEVICE_IPLC_AUTH_ROOT ".{i}");
    err |= USP_REGISTER_DBParam_Alias(DEVICE_IPLC_AUTH_ROOT ".{i}.Alias", NULL);
    err |= USP_REGISTER_DBParam_Secure(DEVICE_IPLC_AUTH_ROOT ".{i}.AuthenticationKey", "", NULL, NULL);
    err |= USP_REGISTER_DBParam_ReadWriteAuto(DEVICE_IPLC_AUTH_ROOT ".{i}.AuthenticationKeyID", AutoPopulate_AuthenticationKeyID, Validate_AuthenticationKeyID, NULL, DM_UINT);

    // Register unique keys for IPLayerCapacityAuth table
    char *unique_keys[] = { "AuthenticationKeyID" };
    err |= USP_REGISTER_Object_UniqueKey(DEVICE_IPLC_AUTH_ROOT ".{i}", unique_keys, NUM_ELEM(unique_keys));

    // Register capabilities parameters
    // NOTE: These will be populated when we receive a JSON response from the UDPST client
    memset(&iplc_caps, 0, sizeof(iplc_caps));
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.IP.Diagnostics.IPLayerCapSupportedSoftwareVersion", Get_IPLCap_SoftwareVersion, DM_STRING);
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.IP.Diagnostics.IPLayerCapSupportedControlProtocolVersion", Get_IPLCap_ProtocolVersion, DM_STRING);
    err |= USP_REGISTER_VendorParam_ReadOnly("Device.IP.Diagnostics.IPLayerCapSupportedMetrics", Get_IPLCap_SupportedMetrics, DM_STRING);

    // The following parameters are constants, rather than extracting them from returned JSON result,
    // since we need to check that the USP command isn't trying to use more than this number
    err |= USP_REGISTER_Param_Constant("Device.IP.Diagnostics.IPLayerMaxConnections", IPL_MAX_CONNECTIONS_STR, DM_UINT);
    err |= USP_REGISTER_Param_Constant("Device.IP.Diagnostics.IPLayerMaxIncrementalResult", IPL_MAX_INC_RESULT_STR, DM_UINT);

    // Exit if an error occurred whilst registering the data model
    if (err != USP_ERR_OK)
    {
        return err;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** IPLCap_Operate
**
** Starts the asynchronous IP Layer Capacity operation
** Checks that all mandatory parameters are present and valid, defaults non-mandatory parameters,
** then starts a thread to perform the USP command
**
** \param   req - pointer to structure identifying the operation in the data model
** \param   input_args - vector containing input arguments and their values
** \param   instance - instance number of this operation in the Device.LocalAgent.Request table
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int IPLCap_Operate(dm_req_t *req, kv_vector_t *input_args, int instance)
{
    int err;
    char *role;
    char *protocol_version;
    unsigned flow_count;
    unsigned maximum_flows;
    bool jumbo_frames_permitted;
    unsigned mtu;
    char *udp_payload_content;
    unsigned max_test_bandwidth;
    unsigned num_first_mode_test_sub_intervals;
    bool ipdv_enable;
    bool reord_dup_ignore_enable;
    bool auth_enabled;
    char *auth_code;
    int auth_key_id;
    char *auth_key_file;
    unsigned dscp;
    unsigned start_sending_rate_index;
    unsigned test_sub_interval;
    unsigned num_test_sub_intervals;
    unsigned test_interval_time;
    char *rate_adj_alg;
    unsigned lower_thresh;
    unsigned upper_thresh;
    unsigned status_feedback_interval;
    unsigned slow_adj_thresh;
    unsigned high_speed_delta;
    unsigned seq_err_thresh;
    char *interface;
    char path[MAX_DM_PATH];
    char interface_name[32] = {0};
    bool local_interface_rate_included;
    char *server_list;
    str_vector_t servers;
    char cmd[4096];
    iplcap_input_cond_t *cond;
    int i;
    char key[MAX_DM_VALUE_LEN];
    combined_role_t combined_role;
    char *test_type;

    STR_VECTOR_Init(&servers);

    // Allocate input conditions to pass to thread
    cond = USP_MALLOC(sizeof(iplcap_input_cond_t));
    memset(cond, 0, sizeof(iplcap_input_cond_t));
    cond->request_instance = instance;

    STR_VECTOR_Init(&cond->argv);
    STR_VECTOR_Add(&cond->argv, UDPST_PATH);
    IPLCap_AddCmdArgs(&cond->argv, "-f", "%s", "jsonf");

    // Exit if unable to get the Role
    role = USP_ARG_Get(input_args, "Role", "Receiver");
    if (role==NULL)
    {
        USP_ERR_SetMessage("%s: No Role provided", __FUNCTION__);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if Role is invalid
    if (strcmp(role, "Sender")==0)
    {
        STR_VECTOR_Add(&cond->argv, "-u");
    }
    else if (strcmp(role, "Receiver")==0)
    {
        STR_VECTOR_Add(&cond->argv, "-d");
    }
    else
    {
        USP_ERR_SetMessage("%s: Input argument (Role=%s) is invalid", __FUNCTION__, role);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get the IP ProtocolVersion
    protocol_version = USP_ARG_Get(input_args, "ProtocolVersion", "Any");
    if (strcmp(protocol_version, "IPv4")==0)
    {
        STR_VECTOR_Add(&cond->argv, "-4");
    }
    else if (strcmp(protocol_version, "IPv6")==0)
    {
        STR_VECTOR_Add(&cond->argv, "-6");
    }
    else if (strcmp(protocol_version, "Any")!=0)
    {
        USP_ERR_SetMessage("%s: Input argument (ProtocolVersion=%s) is invalid", __FUNCTION__, protocol_version);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get FlowCount
    err = USP_ARG_GetUnsigned(input_args, "FlowCount", 0, &flow_count);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get MaximumFlows
    err = USP_ARG_GetUnsignedWithinRange(input_args, "MaximumFlows", 0, 0, IPL_MAX_CONNECTIONS, &maximum_flows);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (flow_count != 0)
    {
        if (maximum_flows != 0)
        {
            IPLCap_AddCmdArgs(&cond->argv, "-C", "%u-%u", flow_count, maximum_flows);

        }
        else
        {
            IPLCap_AddCmdArgs(&cond->argv, "-C", "%u", flow_count);
        }
    }

    // Exit if unable to get JumboFramesPermitted
    err = USP_ARG_GetBool(input_args, "JumboFramesPermitted", true, &jumbo_frames_permitted);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (jumbo_frames_permitted == false)
    {
        STR_VECTOR_Add(&cond->argv, "-j");
    }

    // Exit if unable to get starting frame size. The frame size will increase above this if jumbo frames are permitted
    #define SAFE_MTU 1250    // Safe MTU size that avoids fragmentation when tunneling or encapsulation is present. Typical for FWA
    #define TRAD_MTU 1500    // Traditional MTU size that avoids fragmentation when tunneling or encapsulation is not present. Typical for PON/copper
    err = USP_ARG_GetUnsigned(input_args, "MTU", TRAD_MTU, &mtu);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if MTU is set to a value which is not supported by udpst
    if ((mtu != TRAD_MTU) && (mtu != SAFE_MTU))
    {
        USP_ERR_SetMessage("%s: Invalid MTU specified (%d). Only values supported are %d and %d", __FUNCTION__, mtu, TRAD_MTU, SAFE_MTU);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    if (mtu == TRAD_MTU)
    {
        STR_VECTOR_Add(&cond->argv, "-T");
    }

    // Exit if unable to get UDPPayloadContent
    udp_payload_content = USP_ARG_Get(input_args, "UDPPayloadContent", "zeroes");
    if (strcmp(udp_payload_content, "random")==0)
    {
        STR_VECTOR_Add(&cond->argv, "-X");
    }
    else if (strcmp(udp_payload_content, "zeroes")!=0)
    {
        USP_ERR_SetMessage("%s: Input argument (UDPPayloadContent=%s) is invalid or unsupported", __FUNCTION__, udp_payload_content);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get MaximumTestBandwidth
    err = USP_ARG_GetUnsignedWithinRange(input_args, "MaximumTestBandwidth", 0, 0, 32767, &max_test_bandwidth);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (max_test_bandwidth > 0)
    {
        IPLCap_AddCmdArgs(&cond->argv, "-B", "%u", max_test_bandwidth);
    }

    // Exit if unable to get NumberFirstModeTestSubIntervals
    err = USP_ARG_GetUnsignedWithinRange(input_args, "NumberFirstModeTestSubIntervals", 0, 0, 100, &num_first_mode_test_sub_intervals);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (num_first_mode_test_sub_intervals > 0)
    {
        IPLCap_AddCmdArgs(&cond->argv, "-i", "%u", num_first_mode_test_sub_intervals);
    }

    // Exit if unable to get IPDVEnable
    err = USP_ARG_GetBool(input_args, "IPDVEnable", false, &ipdv_enable);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (ipdv_enable == true)
    {
        STR_VECTOR_Add(&cond->argv, "-o");
    }

    // Exit if TestType is not "Search"
    test_type = USP_ARG_Get(input_args, "TestType", "Search");
    if (strcmp(test_type, "Search") != 0)
    {
        USP_ERR_SetMessage("%s: Unsupported value in TestType (%s). Only 'Search' supported", __FUNCTION__, test_type);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get ReordDupIgnoreEnable
    err = USP_ARG_GetBool(input_args, "ReordDupIgnoreEnable", true, &reord_dup_ignore_enable);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (reord_dup_ignore_enable == false)
    {
        STR_VECTOR_Add(&cond->argv, "-R");
    }

    // Exit if unable to get AuthenticationEnabled
    err = USP_ARG_GetBool(input_args, "AuthenticationEnabled", false, &auth_enabled);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get AuthenticationKeyID
    err = USP_ARG_GetInt(input_args, "AuthenticationKeyID", INVALID, &auth_key_id);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if AuthenticationKeyID is not in range
    #define AUTH_KEY_ID_MIN 0
    #define AUTH_KEY_ID_MAX 255
    if ((auth_key_id != INVALID) && ((auth_key_id < AUTH_KEY_ID_MIN) || (auth_key_id > AUTH_KEY_ID_MAX)) )
    {
        USP_ERR_SetMessage("%s: AuthenticationKeyID (%d) is out of range [%d:%d]", __FUNCTION__, auth_key_id, AUTH_KEY_ID_MIN, AUTH_KEY_ID_MAX);
        err = USP_ERR_INVALID_VALUE;
        goto exit;
    }

    // Handle AuthenticationCode, AuthenticationKeyID and AuthenticationKeyFileLocation
    auth_code = USP_ARG_Get(input_args, "AuthenticationCode", "");
    auth_key_file = USP_ARG_Get(input_args, "AuthenticationKeyFileLocation", "");

    if (auth_enabled==true)
    {
        // Exit if authentication is enabled, but no authentication details provided
        if ((*auth_code == '\0') && (auth_key_id == INVALID) && (*auth_key_file == '\0'))
        {
            USP_ERR_SetMessage("%s: Authentication is enabled, but no authentication details provided", __FUNCTION__);
            err = USP_ERR_INVALID_COMMAND_ARGS;
            goto exit;
        }

        if ((*auth_code != '\0') && (auth_key_id == INVALID) && (*auth_key_file == '\0'))
        {
            // If only AuthenticationCode is provided, the client uses the value directly as the key.
            IPLCap_AddCmdArgs(&cond->argv, "-a", "%s", auth_code);
        }
        else if ((*auth_code != '\0') && (auth_key_id != INVALID) && (*auth_key_file == '\0'))
        {
            // If both AuthenticationCode and AuthenticationKeyID are provided but AuthenticationKeyFileLocation is not provided,
            // the client uses the AuthenticationCode as the key and the AuthenticationKeyID as key ID.
            // The IPLayerCapacityAuthCode table is ignored.
            IPLCap_AddCmdArgs(&cond->argv, "-y", "%d", auth_key_id);
            IPLCap_AddCmdArgs(&cond->argv, "-a", "%s", auth_code);
        }
        else if ((*auth_code == '\0') && (auth_key_id != INVALID) && (*auth_key_file == '\0'))
        {
            // If only AuthenticationKeyID is provided, AuthenticationKeyID is used to select the instance to use from the IPLayerCapacityAuthCode
            // table. The client uses IPLayerCapacityAuthCode.{i}.AuthenticationKey as the key and AuthenticationKeyID as key ID.
            // If the IPLayerCapacityAuthCode table does not contain the AuthenticationKeyID , the client returns an error.
            MSG_HANDLER_GetMsgRole(&combined_role);
            err = LookupKeyByAuthKeyID(auth_key_id, key, sizeof(key), &combined_role);
            if (err != USP_ERR_OK)
            {
                goto exit;
            }

            IPLCap_AddCmdArgs(&cond->argv, "-y", "%d", auth_key_id);
            IPLCap_AddCmdArgs(&cond->argv, "-a", "%s", key);
        }
        else if ((*auth_code == '\0') && (auth_key_id != INVALID) && (*auth_key_file |= '\0'))
        {
            // If both AuthenticationKeyID and AuthenticationKeyFileLocation are provided, but AuthenticationCode
            // is not provided, the client uses the AuthenticationKeyID as key ID along with AuthenticationKeyFileLocation
            // as file location. The IPLayerCapacityAuthCode table is ignored.
            IPLCap_AddCmdArgs(&cond->argv, "-y", "%d", auth_key_id);
            IPLCap_AddCmdArgs(&cond->argv, "-K", "%s", auth_key_file);
        }
        else if ((*auth_code == '\0') && (auth_key_id == INVALID) && (*auth_key_file != '\0'))
        {
            // If only AuthenticationKeyFileLocation is provided, if the file specified contains only a single entry,
            // then it uses that as the key. If the file contains multiple entries, then it uses the key from the entry
            // with Key ID equal to 0. If no entry exists with Key ID equal to 0, the client returns an error.
            IPLCap_AddCmdArgs(&cond->argv, "-K", "%s", auth_key_file);
        }
        else
        {
            USP_ERR_SetMessage("%s: Invalid combination of Authentication arguments(AuthenticationCode, AuthenticationKeyID=%d, AuthenticationKeyFileLocation=%s)", __FUNCTION__, auth_key_id, auth_key_file);
            err = USP_ERR_INVALID_COMMAND_ARGS;
            goto exit;
        }
    }

    // Exit if unable to get DSCP
    err = USP_ARG_GetUnsignedWithinRange(input_args, "DSCP", 0, 0, 63, &dscp);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-m", "%u", dscp);

    // Exit if unable to get StartSendingRateIndex
    err = USP_ARG_GetUnsignedWithinRange(input_args, "StartSendingRateIndex", 0, 0, 11108, &start_sending_rate_index);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-I", "@%u", start_sending_rate_index);

    // Exit if unable to get TestSubInterval
    err = USP_ARG_GetUnsignedWithinRange(input_args, "TestSubInterval", 1000, 100, 6000, &test_sub_interval);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get NumberTestSubIntervals
    err = USP_ARG_GetUnsignedWithinRange(input_args, "NumberTestSubIntervals", 10, 1, IPL_MAX_INC_RESULT, &num_test_sub_intervals);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    test_interval_time = (test_sub_interval*num_test_sub_intervals)/1000;
    if (test_interval_time > 0)
    {
        IPLCap_AddCmdArgs(&cond->argv, "-t", "%u", test_interval_time);
    }
    IPLCap_AddCmdArgs(&cond->argv, "-P", "%u", test_sub_interval/1000 );

    // Exit if unable to get RateAdjAlgorithm
    rate_adj_alg = USP_ARG_Get(input_args, "RateAdjAlgorithm", "B");
    if ( (strlen(rate_adj_alg)==1) &&
         ((*rate_adj_alg == 'B') || (*rate_adj_alg == 'C')) )
    {
        IPLCap_AddCmdArgs(&cond->argv, "-A", "%c", *rate_adj_alg);
    }
    else
    {
        USP_ERR_SetMessage("%s: Input argument (RateAdjAlgorithm=%s) is invalid", __FUNCTION__, rate_adj_alg);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get LowerThresh
    err = USP_ARG_GetUnsignedWithinRange(input_args, "LowerThresh", 30, 5, 250, &lower_thresh);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if unable to get UpperThresh
    err = USP_ARG_GetUnsignedWithinRange(input_args, "UpperThresh", 90, 5, 250, &upper_thresh);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    if (lower_thresh > upper_thresh)
    {
        USP_ERR_SetMessage("%s: Input argument LowerThresh=%u should be less than UpperThresh=%u", __FUNCTION__, lower_thresh, upper_thresh);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    IPLCap_AddCmdArgs(&cond->argv, "-L", "%u", lower_thresh);
    IPLCap_AddCmdArgs(&cond->argv, "-U", "%u", upper_thresh);

    // Exit if unable to get StatusFeedbackInterval
    err = USP_ARG_GetUnsignedWithinRange(input_args, "StatusFeedbackInterval", 50, 5, 250, &status_feedback_interval);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-F", "%u", status_feedback_interval);

    // Exit if unable to get SlowAdjThresh
    err = USP_ARG_GetUnsignedWithinRange(input_args, "SlowAdjThresh", 3, 2, UINT_MAX, &slow_adj_thresh);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-c", "%u", slow_adj_thresh);

    // Exit if unable to get HighSpeedDelta
    err = USP_ARG_GetUnsignedWithinRange(input_args, "HighSpeedDelta", 10, 2, UINT_MAX, &high_speed_delta);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-h", "%u", high_speed_delta);

    // Exit if unable to get SeqErrThresh
    err = USP_ARG_GetUnsignedWithinRange(input_args, "SeqErrThresh", 10, 0, 100, &seq_err_thresh);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }
    IPLCap_AddCmdArgs(&cond->argv, "-q", "%u", seq_err_thresh);

    // Exit if unable to get the Interface (reference)
    interface = USP_ARG_Get(input_args, "Interface", "");
    if (*interface != '\0')
    {
        // Exit if the Interface reference is to the wrong table
        err = DM_ACCESS_ValidateReference(interface, "Device.IP.Interface.{i}", NULL);
        if (err != USP_ERR_OK)
        {
            goto exit;
        }

        // Exit if unable to get the name of the interface
        USP_SNPRINTF(path, sizeof(path), "%s.Name", interface);
        err = DATA_MODEL_GetParameterValue(path, interface_name, sizeof(interface_name), 0);
        if (err != USP_ERR_OK)
        {
            goto exit;
        }

        IPLCap_AddCmdArgs(&cond->argv, "-E", "%s", interface_name);
    }

    // Exit if unable to get LocalInterfaceRateIncluded
    err = USP_ARG_GetBool(input_args, "LocalInterfaceRateIncluded", true, &local_interface_rate_included);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // NOTE: The -M option may only be set of the -E option has been set
    if ((local_interface_rate_included==true) && (*interface != '\0'))
    {
        STR_VECTOR_Add(&cond->argv, "-M");
    }

    // Exit if unable to get ServerList
    server_list = USP_ARG_Get(input_args, "ServerList", "");
    if (*server_list == '\0')
    {
        USP_ERR_SetMessage("%s: Input argument ServerList must be specified", __FUNCTION__);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Add all servers to command line
    TEXT_UTILS_SplitString(server_list, &servers, ",");
    for (i=0; i < servers.num_entries; i++)
    {
        STR_VECTOR_Add(&cond->argv, servers.vector[i]);
    }

    // Form the command that will be run (for debug purposes)
    TEXT_UTILS_ListToString(cond->argv.vector, cond->argv.num_entries, cmd, sizeof(cmd), " ");

    // Log the input conditions for the operation
    USP_LOG_Info("=== IP Layer Capacity Conditions ===");
    USP_LOG_Info("Role: %s", role);
    USP_LOG_Info("ProtocolVersion: %s", protocol_version);
    USP_LOG_Info("FlowCount: %u", flow_count);
    USP_LOG_Info("MaximumFlows: %u", maximum_flows);
    USP_LOG_Info("JumboFramesPermitted: %d", jumbo_frames_permitted);
    USP_LOG_Info("MTU: %u", mtu);
    USP_LOG_Info("UDPPayloadContent: %s", udp_payload_content);
    USP_LOG_Info("MaximumTestBandwidth: %u", max_test_bandwidth);
    USP_LOG_Info("NumberFirstModeTestSubIntervals: %u", num_first_mode_test_sub_intervals);
    USP_LOG_Info("IPDVEnable: %d", ipdv_enable);
    USP_LOG_Info("ReordDupIgnoreEnable: %d", reord_dup_ignore_enable);
    USP_LOG_Info("AuthenticationEnabled: %d", auth_enabled);
    USP_LOG_Info("AuthenticationCode: %s", auth_code);
    USP_LOG_Info("AuthenticationKeyID: %d", auth_key_id);
    USP_LOG_Info("AuthenticationKeyFileLocation: %s", auth_key_file);
    USP_LOG_Info("DSCP: %d", dscp);
    USP_LOG_Info("StartSendingRateIndex: %u", start_sending_rate_index);
    USP_LOG_Info("TestSubInterval: %u", test_sub_interval);
    USP_LOG_Info("NumberTestSubIntervals: %u", num_test_sub_intervals);
    USP_LOG_Info("RateAdjAlgorithm: %s", rate_adj_alg);
    USP_LOG_Info("LowerThresh: %u", lower_thresh);
    USP_LOG_Info("UpperThresh: %u", upper_thresh);
    USP_LOG_Info("StatusFeedbackInterval: %u", status_feedback_interval);
    USP_LOG_Info("SlowAdjThresh: %u", slow_adj_thresh);
    USP_LOG_Info("HighSpeedDelta: %u", high_speed_delta);
    USP_LOG_Info("SeqErrThresh: %u", seq_err_thresh);
    USP_LOG_Info("Interface: %s (%s)", interface, interface_name);
    USP_LOG_Info("LocalInterfaceRateIncluded: %d", local_interface_rate_included);
    USP_LOG_Info("ServerList: %s", server_list);
    USP_LOG_Info("command line: %s", cmd);

    // Exit if unable to start a thread to perform this operation
    // NOTE: ownership of input conditions passes to the thread
    err = OS_UTILS_CreateThread("IPLCap", IPLCapThreadMain, cond);
    if (err != USP_ERR_OK)
    {
        err = USP_ERR_COMMAND_FAILURE;
        goto exit;
    }

exit:
    STR_VECTOR_Destroy(&servers);

    // Exit if an error occurred (freeing the input conditions)
    if (err != USP_ERR_OK)
    {
        STR_VECTOR_Destroy(&cond->argv);
        USP_FREE(cond);
        return USP_ERR_COMMAND_FAILURE;
    }

    // Ownership of the input conditions has passed to the thread
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** IPLCapThreadMain
**
** Main function for IP Layer Capacity Asynchronous operation thread
**
** \param   param - pointer to input conditions
**
** \return  NULL
**
**************************************************************************/
void *IPLCapThreadMain(void *param)
{
    iplcap_input_cond_t *cond = (iplcap_input_cond_t *) param;
    iplcap_output_res_t results;
    kv_vector_t *output_args;
    int err;

    // Ensure that when this USP command is invoked from the CLI, that the data model thread gets to complete the command,
    // so that the CLI client isn't waiting for the result
    usleep(1);

    // Set default results
    memset(&results, 0, sizeof(results));
    results.json = NULL;
    iplcap_err_msg[0] = '\0';

    // Exit if unable to signal that this operation is active
    err = USP_SIGNAL_OperationStatus(cond->request_instance, "Active");
    if (err != USP_ERR_OK)
    {
        USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: USP_SIGNAL_OperationStatus() failed", __FUNCTION__);
        USP_SIGNAL_OperationComplete(cond->request_instance, USP_ERR_COMMAND_FAILURE, iplcap_err_msg, NULL);
        goto exit;
    }

    // Perform the diagnostic
    err = IPLCap_Execute(cond, &results);
    if (err != USP_ERR_OK)
    {
        USP_LOG_Error("%s", iplcap_err_msg);
    }

    // Exit if diagnostic failed
    USP_LOG_Info("=== IP Layer Capacity Diagnostic completed with result=%d ===", err);
    if (err != USP_ERR_OK)
    {
        USP_SIGNAL_OperationComplete(cond->request_instance, err, iplcap_err_msg, NULL);
        goto exit;
    }

    // Parse the results from the JSON into the output arguments
    output_args = USP_ARG_Create();
    err = IPLCap_SaveResults(cond, &results, output_args);

    // Inform the protocol handler, that the operation has completed
    // Ownership of the output args passes to protocol handler
    if (err == USP_ERR_OK)
    {
        USP_SIGNAL_OperationComplete(cond->request_instance, USP_ERR_OK, NULL, output_args);
    }
    else
    {
        USP_LOG_Error("%s", iplcap_err_msg);
        USP_SIGNAL_OperationComplete(cond->request_instance, err, iplcap_err_msg, output_args);
    }

exit:
    // Free the input conditions
    STR_VECTOR_Destroy(&cond->argv);
    USP_FREE(cond);
    USP_SAFE_FREE(results.json);

    return NULL;
}

/*********************************************************************//**
**
**  IPLCap_Execute
**
**  Performs the IP Layer Capacity diagnostic
**
** \param   cond - pointer to structure containing the data model parameters controlling this diagnostic
** \param   res - pointer to structure containing the test's results
**
** \return  USP_ERR_OK if JSON formatted results obtained. NOTE: udpst may indicate errors in the JSON or directly on stdout
**
**************************************************************************/
int IPLCap_Execute(iplcap_input_cond_t *cond, iplcap_output_res_t *res)
{
    int err = USP_ERR_OK;
    int status;
    int len;
    int new_len;
    int rc;
    int num_bytes_read;
    int i;
    char buf[USP_LOG_MAXLEN];
    int p[2];
    pid_t pid;
    pid_t w_pid;

    USP_LOG_Info("=== Executing IP Layer Capacity Test ===");

    // Add a NULL terminator entry to the argv list, as execve() needs it to terminate the list
    STR_VECTOR_Add(&cond->argv, NULL);

    // Exit if unable to create a pipe to get the output from UDPST
    rc = pipe2(p, O_CLOEXEC);
    if (rc == -1)
    {
        USP_LOG_Error("%s: pipe2() failed (errno=%d): %s", __FUNCTION__, errno, strerror(errno));
        return USP_ERR_COMMAND_FAILURE;
    }

    // Exit if unable to fork this process (the child will go on to run UDPST)
    pid = fork();
    if (pid == -1)
    {
        close(p[0]);
        close(p[1]);
        USP_LOG_Error("%s: fork() failed (errno=%d): %s", __FUNCTION__, errno, strerror(errno));
        return USP_ERR_COMMAND_FAILURE;
    }

    // Handle starting UDPST in the child process
    if (pid == 0)
    {
        // Merge stderr and stdout into p[1]
        close(p[0]);
        dup2(p[1], STDOUT_FILENO);
        dup2(STDOUT_FILENO, STDERR_FILENO);
        close(p[1]);

        // Start the speed test executable
        // NOTE: If successful execve does not return
        execve(UDPST_PATH, cond->argv.vector, NULL);

        // Since execve returned, an error occurred trying to start UDPST, so return an error code to parent process
        _exit(127);
    }

    // If the code gets here, then this is the parent process (ie USP Agent)
    // p[0] is for reading stdout and stderr of the child process (child process is writing to p[1])
    close(p[1]);

    // Read udpst's JSON formatted output until there's no more output left or an error occurred
    USP_ASSERT(res->json == NULL);
    len = 0;
    while (FOREVER)
    {
        num_bytes_read = read(p[0], buf, sizeof(buf)-1);  // Minus 1 to allow buffer to be NULL terminated later

        // Exit if an error occurred while reading the output
        if (num_bytes_read == -1)
        {
            USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: Failed to read all output of %s (error occurred)", __FUNCTION__, UDPST_PATH);
            err = USP_ERR_COMMAND_FAILURE;
            goto exit;
        }

        // Break out of loop, if all bytes read
        if (num_bytes_read == 0)
        {
            break;
        }

        // Log received JSON
        buf[num_bytes_read] = '\0';
        USP_LOG_Debug("%s", buf);

        // Increase the size of the result buffer and copy the received data into it
        new_len = len + num_bytes_read + 1;     // Plus 1 to include NULL terminator
        res->json = USP_REALLOC(res->json, new_len);
        memcpy(&res->json[len], buf, num_bytes_read+1);
        len += num_bytes_read;

        // Exit if the result is getting too large
        if (len >= MAX_USP_MSG_LEN)
        {
            USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: JSON output is too large (>%d bytes)", __FUNCTION__, MAX_USP_MSG_LEN);
            err = USP_ERR_COMMAND_FAILURE;
            goto exit;
        }

    }

    // Exit if failed to wait for udpst to terminate
    w_pid = waitpid(pid, &status, 0);
    if (w_pid == -1)
    {
        USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: waitpid failed (%s)", __FUNCTION__, strerror(errno));
        err = USP_ERR_COMMAND_FAILURE;
        goto exit;
    }

    // Exit if we got JSON formatted output. In this case, causes of errors are contained in the JSON, so allow the JSON to be parsed by the caller
    if (res->json[0] == '{')
    {
        err = USP_ERR_OK;
        goto exit;
    }

    // Exit if udpst's exit code indicated an error (and the error message wasn't in JSON format)
    rc = WEXITSTATUS(status);
    if (rc != 0)
    {
        // Convert carriage returns to spaces in the captured stdout/stderr
        for (i=0; i<len; i++)
        {
            if (res->json[i] == '\n')
            {
                res->json[i] = ' ';
            }
        }

        USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: udpst's exit code (=%d)  indicated error (%s)", __FUNCTION__, rc, res->json);
        err = USP_ERR_COMMAND_FAILURE;
        goto exit;
    }

exit:
    close(p[0]);
    return err;
}

/*********************************************************************//**
**
**  IPLCap_SaveResults
**
**  Saves the results of the test into the output args vector
**
** \param   cond - pointer to structure containing the data model parameters controlling this diagnostic
** \param   res - pointer to structure containing the test's results
** \param   output_args - pointer to key value vector structure to save the results into
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int IPLCap_SaveResults(iplcap_input_cond_t *cond, iplcap_output_res_t *res, kv_vector_t *output_args)
{
    JsonNode *node;
    JsonNode *root;
    JsonNode *output_node;
    JsonNode *at_max_node;
    JsonNode *summary_node;
    JsonNode *inc_res_node;
    JsonNode *mod_res_node;
    JsonNode *element;
    int instance;
    char *inc_res_str = "IncrementalResult";
    char *mod_res_str = "ModalResult";
    kv_pair_t *pair;
    int i;
    char buf[512];
    int len;
    int err;

    // Exit if unable to parse the JSON formatted output
    root = json_decode(res->json);
    if (root == NULL)
    {
        USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: Failed to parse JSON output", __FUNCTION__);
        return USP_ERR_COMMAND_FAILURE;
    }

    // Extract properties of the udpst executable
    IPLCap_ExtractCapabilities(root);

    // Extract StatusCode
    err = AddJsonChildToOutputArgs(root, "ErrorStatus", output_args, "StatusCode", "%.0f");
    if (err != USP_ERR_OK)
    {
        USP_SNPRINTF(iplcap_err_msg, sizeof(iplcap_err_msg), "%s: ErrorStatus not present in JSON output", __FUNCTION__);
        return USP_ERR_COMMAND_FAILURE;
    }

    // Concatenate ErrorMessage and ErrorMessage2 to form StatusMessage
    *buf = '\0';
    len = 0;
    node = json_find_member(root, "ErrorMessage");
    if ((node != NULL) && (node->tag == JSON_STRING) && (node->string_[0] != '\0'))
    {
        len += USP_SNPRINTF(&buf[len], sizeof(buf)-len, "%s", node->string_);
    }

    node = json_find_member(root, "ErrorMessage2");
    if ((node != NULL) && (node->tag == JSON_STRING) && (node->string_[0] != '\0'))
    {
        len += USP_SNPRINTF(&buf[len], sizeof(buf)-len, " %s", node->string_);
    }

    USP_ARG_Add(output_args, "StatusMessage", buf);

    // Exit if no Output node
    // NOTE: This may be the case if udpst detected an error eg server response timeout
    output_node = json_find_member(root, "Output");
    if (output_node == NULL)
    {
        goto exit;
    }

    // Extract Output arguments
    AddJsonChildToOutputArgs(output_node, "Status", output_args, NULL, NULL);
    AddJsonChildToOutputArgs(output_node, "BOMTime", output_args, NULL, NULL);
    AddJsonChildToOutputArgs(output_node, "EOMTime", output_args, NULL, NULL);
    AddJsonChildToOutputArgs(output_node, "TmaxUsed", output_args, NULL, "%.0f");
    AddJsonChildToOutputArgs(output_node, "TestInterval", output_args, NULL, "%.0f");
    AddJsonChildToOutputArgs(output_node, "TmaxRTTUsed", output_args, NULL, "%.0f");
    AddJsonChildToOutputArgs(output_node, "TimestampResolutionUsed", output_args, NULL, "%.0f");

    // Extract AtMax arguments
    at_max_node = json_find_member(output_node, "AtMax");
    if (at_max_node != NULL)
    {
        AddJsonChildToOutputArgs(at_max_node, "MaxIPLayerCapacity", output_args, NULL, "%.2f");
        AddJsonChildToOutputArgs(at_max_node, "TimeOfMax", output_args, NULL, NULL);
        AddJsonChildToOutputArgs(at_max_node, "MaxETHCapacityNoFCS", output_args, NULL, "%.2f");
        AddJsonChildToOutputArgs(at_max_node, "MaxETHCapacityWithFCS", output_args, NULL, "%.2f");
        AddJsonChildToOutputArgs(at_max_node, "MaxETHCapacityWithFCSVLAN", output_args, NULL, "%.2f");
        AddJsonChildToOutputArgs(at_max_node, "LossRatioAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "RTTRangeAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "PDVRangeAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "MinOnewayDelayAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "ReorderedRatioAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "ReplicatedRatioAtMax", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "RTTMin", output_args, "RTTMinAtMax", "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "RTTMax", output_args, "RTTMaxAtMax", "%.9g");
        AddJsonChildToOutputArgs(at_max_node, "InterfaceEthMbps", output_args, "InterfaceEthMbpsAtMax", "%.2f");
    }

    // Extract Summary arguments
    summary_node = json_find_member(output_node, "Summary");
    if (summary_node != NULL)
    {
        AddJsonChildToOutputArgs(summary_node, "IPLayerCapacitySummary", output_args, NULL, "%.2f");
        AddJsonChildToOutputArgs(summary_node, "LossRatioSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "RTTRangeSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "PDVRangeSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "MinOnewayDelaySummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "MinRTTSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "ReorderedRatioSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "ReplicatedRatioSummary", output_args, NULL, "%.9g");
        AddJsonChildToOutputArgs(summary_node, "InterfaceEthMbps", output_args, "InterfaceEthMbpsSummary", "%.2f");
    }

    // Extract IncrementalResult arguments
    inc_res_node = json_find_member(output_node, inc_res_str);
    if ((inc_res_node != NULL) && (inc_res_node->tag == JSON_ARRAY))
    {
        instance = 1;
        element = json_first_child(inc_res_node);
        while (element != NULL)
        {
            AddJsonArrayChildToOutputArgs(element, "IPLayerCapacity", output_args, inc_res_str, instance, NULL, "%.2f");
            AddJsonArrayChildToOutputArgs(element, "TimeOfSubInterval", output_args, inc_res_str, instance, NULL, NULL);
            AddJsonArrayChildToOutputArgs(element, "LossRatio", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "RTTRange", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "PDVRange", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "MinOnewayDelay", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "ReorderedRatio", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "ReplicatedRatio", output_args, inc_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "InterfaceEthMbps", output_args, inc_res_str, instance, "InterfaceEthMbpsAtMax", "%.2f");

            // Move to next array element
            element = element->next;
            instance++;
        }
    }

    // Extract ModalResult arguments
    mod_res_node = json_find_member(output_node, mod_res_str);
    if ((mod_res_node != NULL) && (mod_res_node->tag == JSON_ARRAY))
    {
        instance = 1;
        element = json_first_child(mod_res_node);
        while (element != NULL)
        {
            AddJsonArrayChildToOutputArgs(element, "MaxIPLayerCapacity", output_args, mod_res_str, instance, NULL, "%.2f");
            AddJsonArrayChildToOutputArgs(element, "TimeOfMax", output_args, mod_res_str, instance, NULL, NULL);
            AddJsonArrayChildToOutputArgs(element, "MaxETHCapacityNoFCS", output_args, mod_res_str, instance, NULL, "%.2f");
            AddJsonArrayChildToOutputArgs(element, "MaxETHCapacityWithFCS", output_args, mod_res_str, instance, NULL, "%.2f");
            AddJsonArrayChildToOutputArgs(element, "MaxETHCapacityWithFCSVLAN", output_args, mod_res_str, instance, NULL, "%.2f");
            AddJsonArrayChildToOutputArgs(element, "LossRatioAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "RTTRangeAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "PDVRangeAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "MinOnewayDelayAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "ReorderedRatioAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "ReplicatedRatioAtMax", output_args, mod_res_str, instance, NULL, "%.9g");
            AddJsonArrayChildToOutputArgs(element, "InterfaceEthMbps", output_args, mod_res_str, instance, "InterfaceEthMbpsAtMax", "%.2f");

            // Move to next array element
            element = element->next;
            instance++;
        }
    }

    // Log results
    for (i=0; i < output_args->num_entries; i++)
    {
        pair = &output_args->vector[i];
        USP_LOG_Info("%s => %s", pair->key, pair->value);
    }

exit:
    if (root != NULL)
    {
        json_delete(root);
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** IPLCap_AddCmdArgs
**
** Called to build up the arguments to pass to UDPST
**
** \param   cond - input conditions containing arguments
** \param   cmd_switch - command switch to add
** \param   fmt - printf style format for the command switch's arguments
**
** \return  None
**
**************************************************************************/
void IPLCap_AddCmdArgs(str_vector_t *sv, char *cmd_switch, const char *fmt, ...)
{
    va_list ap;
    char buf[MAX_DM_VALUE_LEN];

    // Print the value to the buffer
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    STR_VECTOR_Add(sv, cmd_switch);
    STR_VECTOR_Add(sv, buf);
}

/*********************************************************************//**
**
** AddJsonArrayChildToOutputArgs
**
** Gets the value of a specified child node in a JSON array and adds it to the output arguments for the USP command
**
** \param   element - pointer to json node representing an instance
** \param   key - Name of key in JSON to get the value of
** \param   output_args - pointer to key value vector structure to save the results into
** \param   arg_object - Name of the object to put in the output arguments
** \param   instance - Instance number of the object to put in the output arguments
** \param   arg_name - name of argument to put in the output_args or NULL if the key's name should be used for this
** \param   number_format - printf format specifier to use if the value is a number. If NULL, "%f" is used
**
** \return  None
**
**************************************************************************/
void AddJsonArrayChildToOutputArgs(JsonNode *element, char *key, kv_vector_t *output_args, char *arg_object, int instance, char *arg_name, char *number_format)
{
    char full_name[MAX_DM_PATH];

    // If no output argument name is provided, then this signifies that it is the same as the key
    if (arg_name == NULL)
    {
        arg_name = key;
    }

    // Form full name of the output argument, including instance number
    USP_SNPRINTF(full_name, sizeof(full_name), "%s.%d.%s", arg_object, instance, arg_name);

    // Extract the output argument's value
    AddJsonChildToOutputArgs(element, key, output_args, full_name, number_format);
}

/*********************************************************************//**
**
** AddJsonChildToOutputArgs
**
** Gets the value of a specified child node in the JSON tree and adds it to the output arguments for the USP command
**
** \param   obj - pointer to node having child key-value pair nodes
** \param   key - Name of key in JSON to get the value of
** \param   output_args - pointer to key value vector structure to save the results into
** \param   arg_name - name of argument to put in the output_args or NULL if the key's name should be used for this
** \param   number_format - printf format specifier to use if the value is a number. If NULL, "%f" is used
**
** \return  USP_ERR_OK if child node was found and extracted
**
**************************************************************************/
int AddJsonChildToOutputArgs(JsonNode *obj, char *key, kv_vector_t *output_args, char *arg_name, char *number_format)
{
    JsonNode *node;
    char *s;
    char number[32];

    // If no output argument name is provided, then this signifies that it is the same as the key
    if (arg_name == NULL)
    {
        arg_name = key;
    }

    // Exit if unable to find the node containing the specified key
    node = json_find_member(obj, key);
    if (node == NULL)
    {
        s = (obj->key != NULL) ? obj->key : "";
        USP_LOG_Warning("%s: JSON Node %s.%s not found", __FUNCTION__, s, arg_name);
        return USP_ERR_COMMAND_FAILURE;
    }

    // Add the node's value to the output args
    switch(node->tag)
    {
        case JSON_BOOL:
            s = (node->bool_) ? "true" : "false";
            USP_ARG_Add(output_args, arg_name, s);
            break;

        case JSON_STRING:
            USP_ARG_Add(output_args, arg_name, node->string_);
            break;

        case JSON_NUMBER:
            if (number_format == NULL)
            {
                number_format = "%.9g";
            }

            USP_SNPRINTF(number, sizeof(number), number_format, node->number_);
            USP_ARG_Add(output_args, arg_name, number);
            break;

        default:
        // These special types have been added by us. The JSON parser will not have added any of these nodes to the tree
        case JSON_LL_NUMBER:
        case JSON_ULL_NUMBER:

        // The node shouldn't be any of these types
        case JSON_NULL:
        case JSON_ARRAY:
        case JSON_OBJECT:
            USP_LOG_Warning("%s: JSON Node %s.%s is unexpected type (%d)", __FUNCTION__, obj->key, key, node->tag);
            return USP_ERR_COMMAND_FAILURE;
            break;
    }

    return USP_ERR_OK;
}

/*********************************************************************//**
**
** IPLCap_ExtractCapabilities
**
** Extracts the IP Layer Capacity capabilities from the JSON result
**
** \param   root - pointer to node having child key-value pair nodes
**
** \return  None
**
**************************************************************************/
void IPLCap_ExtractCapabilities(JsonNode *root)
{
    JsonNode *node;
    JsonNode *cap_node;

    // Exit if no IPLayerCapSupported node
    cap_node = json_find_member(root, "IPLayerCapSupported");
    if (cap_node == NULL)
    {
        return;
    }

    // Extract SoftwareVersion
    node = json_find_member(cap_node, "SoftwareVersion");
    if ((node != NULL) && (node->tag==JSON_STRING))
    {
        USP_STRNCPY(iplc_caps.sw_version, node->string_, sizeof(iplc_caps.sw_version));
    }

    // Extract ControlProtocolVersion
    node = json_find_member(cap_node, "ControlProtocolVersion");
    if ((node != NULL) && (node->tag==JSON_NUMBER))
    {
        USP_SNPRINTF(iplc_caps.protocol_version, sizeof(iplc_caps.protocol_version), "%d", (int)node->number_);
    }

    // Extract Metrics
    node = json_find_member(cap_node, "Metrics");
    if ((node != NULL) && (node->tag==JSON_STRING))
    {
        USP_STRNCPY(iplc_caps.metrics, node->string_, sizeof(iplc_caps.metrics));
    }
}

/*********************************************************************//**
**
** Get_IPLCap_SoftwareVersion
**
** Gets the value of Device.IP.Diagnostics.IPLayerCapSupportedSoftwareVersion
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer in which to return the parameter's value
** \param   len - length of return buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Get_IPLCap_SoftwareVersion(dm_req_t *req, char *buf, int len)
{
    USP_SNPRINTF(buf, len, "UDPST-%s", iplc_caps.sw_version);
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Get_IPLCap_ProtocolVersion
**
** Gets the value of Device.IP.Diagnostics.IPLayerCapSupportedControlProtocolVersion
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer in which to return the parameter's value
** \param   len - length of return buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Get_IPLCap_ProtocolVersion(dm_req_t *req, char *buf, int len)
{
    USP_STRNCPY(buf, iplc_caps.protocol_version, len);
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** Get_IPLCap_SupportedMetrics
**
** Gets the value of Device.IP.Diagnostics.IPLayerCapSupportedMetrics
**
** \param   req - pointer to structure identifying the parameter
** \param   buf - pointer to buffer in which to return the parameter's value
** \param   len - length of return buffer
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Get_IPLCap_SupportedMetrics(dm_req_t *req, char *buf, int len)
{
    USP_STRNCPY(buf, iplc_caps.metrics, len);
    return USP_ERR_OK;
}

/*********************************************************************//**
**
** AutoPopulate_AuthenticationKeyID
**
** Called to get an auto-populated parameter value for Device.IP.Diagnostics.IPLayerCapacityAuthCode.{i}.AuthenticationKeyID
**
** \param   req - pointer to structure identifying the path
** \param   buf - pointer to buffer in which to store the value to use to auto-populate the parameter's value
** \param   len - length of return buffer
**
** \return  USP_ERR_OK if auto assigned ID was unique
**
**************************************************************************/
int AutoPopulate_AuthenticationKeyID(dm_req_t *req, char *buf, int len)
{
    int err;
    int i;
    char path[MAX_DM_PATH];
    char value[MAX_DM_SHORT_VALUE_LEN];
    int auth_key_id;
    int num_converted;
    int_vector_t iv;
    bool auth_key_id_in_use[AUTH_KEY_ID_MAX+1];

    // Exit if unable to get the instances of the IPLayerCapacityAuthCode table
    err = DATA_MODEL_GetInstances(DEVICE_IPLC_AUTH_ROOT, &iv);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Iterate over all instances in the IPLayerCapacityAuthCode table, building up which AuthenticationKeyIDs are used
    memset(auth_key_id_in_use, 0, sizeof(auth_key_id_in_use));
    for (i=0; i < iv.num_entries; i++)
    {
        // Skip the instance that we're currently setting
        if (iv.vector[i] == inst1)
        {
            continue;
        }

        // Skip if unable to get the parameter as a string
        USP_SNPRINTF(path, sizeof(path), "%s.%d.AuthenticationKeyID", DEVICE_IPLC_AUTH_ROOT, iv.vector[i]);
        err = DATA_MODEL_GetParameterValue(path, value, sizeof(value), 0);
        if (err != USP_ERR_OK)
        {
            continue;
        }

        // Skip if unable to convert the string to an integer. NOTE: This should never occur
        num_converted = sscanf(value, "%d", &auth_key_id);
        if (num_converted != 1)
        {
            continue;
        }

        // Skip if outside range. NOTE: This should never occur
        if ((auth_key_id < AUTH_KEY_ID_MIN) || (auth_key_id > AUTH_KEY_ID_MAX))
        {
            continue;
        }

        // Mark the auth_key_id as taken
        auth_key_id_in_use[auth_key_id] = true;
    }

    // Iterate over all auth_key_ids, finding the first one that isn't taken
    for (i=0; i < AUTH_KEY_ID_MAX; i++)
    {
        if (auth_key_id_in_use[i] == false)
        {
            // Return the value in buf (rather than using val_uint), since this function is also called from Validate_AuthenticationKeyID)
            USP_SNPRINTF(buf, len, "%d", i);
            err = USP_ERR_OK;
            goto exit;
        }
    }

    // If the code gets here, then all of the auth_key_ids are taken
    USP_ERR_SetMessage("%s: No more unused AuthenticationKeyID values in the range %d to %d", __FUNCTION__, AUTH_KEY_ID_MIN, AUTH_KEY_ID_MAX);
    err = USP_ERR_INVALID_ARGUMENTS;

exit:
    INT_VECTOR_Destroy(&iv);

    return err;
}

/*********************************************************************//**
**
** Validate_AuthenticationKeyID
**
** Validates that Device.IP.Diagnostics.IPLayerCapacityAuthCode.{i}.AuthenticationKeyID is unique
** NOTE: Validation that the new value contains a number has already been performed by the caller
**
** \param   req - pointer to structure identifying the subscription
** \param   value - value that the controller would like to set the Subscription ID to
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int Validate_AuthenticationKeyID(dm_req_t *req, char *value)
{
    int i;
    int err;
    char path[MAX_DM_PATH];
    char buf[MAX_DM_SHORT_VALUE_LEN];
    char default_value[MAX_DM_SHORT_VALUE_LEN];
    int_vector_t iv;

    // Exit if AuthenticationKeyID is out of range
    if (val_uint > 255)
    {
        USP_ERR_SetMessage("%s: AuthenticationKeyID must be in range %d to %d", __FUNCTION__, AUTH_KEY_ID_MIN, AUTH_KEY_ID_MAX);
        return USP_ERR_INVALID_ARGUMENTS;
    }

    // Exit if setting to the same as the default value - we know that this is valid
    AutoPopulate_AuthenticationKeyID(req, default_value, sizeof(default_value));
    if (strcmp(value, default_value)==0)
    {
        return USP_ERR_OK;
    }

    // Exit if unable to get the current value of this AuthenticationKeyID in the table
    err = DATA_MODEL_GetParameterValue(req->path, buf, sizeof(buf), 0);
    if (err != USP_ERR_OK)
    {
        USP_ERR_SetMessage("%s: Failed to get the current value of %s", __FUNCTION__, req->path);
        return err;
    }

    // Exit if unable to get the instances of the IPLayerCapacityAuthCode table
    err = DATA_MODEL_GetInstances(DEVICE_IPLC_AUTH_ROOT, &iv);
    if (err != USP_ERR_OK)
    {
        return err;
    }

    // Iterate over all instances, checking that the given AuthenticationKeyID does not conflict with any entries in the table
    for (i=0; i < iv.num_entries; i++)
    {
        // Skip the instance that we're currently setting
        if (iv.vector[i] == inst1)
        {
            continue;
        }

        // Skip if unable to get the parameter as a string
        USP_SNPRINTF(path, sizeof(path), "%s.%d.AuthenticationKeyID", DEVICE_IPLC_AUTH_ROOT, iv.vector[i]);
        err = DATA_MODEL_GetParameterValue(path, buf, sizeof(buf), 0);
        if (err != USP_ERR_OK)
        {
            continue;
        }

        // Exit if the given AuthenticationKeyID conflicts with this table entry
        if (strcmp(buf, value)==0)
        {
            USP_ERR_SetMessage("%s: AuthenticationKeyID (%s) conflicts with entry at %s.%d", __FUNCTION__, value, DEVICE_IPLC_AUTH_ROOT, iv.vector[i]);
            err = USP_ERR_INVALID_ARGUMENTS;
            goto exit;
        }
    }

    // If the code gets here, then the given AuthenticationKeyID value is unique
    err = USP_ERR_OK;

exit:
    INT_VECTOR_Destroy(&iv);
    return err;
}

/*********************************************************************//**
**
** LookupKeyByAuthKeyID
**
** Finds the AuthenticationKey matching the specified AuthenticationKeyID in Device.IP.Diagnostics.IPLayerCapacityAuthCode.{i}
**
** \param   auth_key_id - AuthenticationKeyID identifying the instance in the table
** \param   buf - pointer to buffer in which to return the Key
** \param   len - length of buffer in which to return the Key
** \param   combined_role - role to use when performing the resolution
**
** \return  USP_ERR_OK if successful
**
**************************************************************************/
int LookupKeyByAuthKeyID(int auth_key_id, char *buf, int len, combined_role_t *combined_role)
{
    int err;
    str_vector_t sv;
    char path[MAX_DM_PATH+MAX_DM_VALUE_LEN];

    STR_VECTOR_Init(&sv);

    // Exit if failed to find the path matching AuthenticationAlias
    // NOTE: Permissions may cause no path to be returned
    USP_SNPRINTF(path, sizeof(path), "%s.[AuthenticationKeyID==\"%d\"].AuthenticationKey", DEVICE_IPLC_AUTH_ROOT, auth_key_id);
    err = PATH_RESOLVER_ResolvePath(path, &sv, NULL, kResolveOp_Get, FULL_DEPTH, combined_role, DONT_LOG_RESOLVER_ERRORS);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    // Exit if no path was found
    if (sv.num_entries == 0)
    {
        USP_ERR_SetMessage("%s: %s does not contain AuthenticationKeyID=='%d'", __FUNCTION__, DEVICE_IPLC_AUTH_ROOT, auth_key_id);
        err = USP_ERR_INVALID_COMMAND_ARGS;
        goto exit;
    }

    // Exit if unable to get the value of the Key
    err = DATA_MODEL_GetParameterValue(sv.vector[0], buf, len, SHOW_PASSWORD);
    if (err != USP_ERR_OK)
    {
        goto exit;
    }

    err = USP_ERR_OK;

exit:
    STR_VECTOR_Destroy(&sv);
    return err;
}

#endif // REMOVE_IP_CAPACITY_DIAG
