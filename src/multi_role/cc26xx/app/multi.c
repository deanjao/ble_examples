/******************************************************************************
 * @file  multirole.c
 *
 * @description Multirole GAPRole abstraction layer for GAP
 *
 *
 * Copyright (C) 2015 Texas Instruments Incorporated - http://www.ti.com/ 
 * 
 * 
 *  Redistribution and use in source and binary forms, with or without 
 *  modification, are permitted provided that the following conditions 
 *  are met:
 *
 *    Redistributions of source code must retain the above copyright 
 *    notice, this list of conditions and the following disclaimer.
 *
 *    Redistributions in binary form must reproduce the above copyright   
 *    notice, this list of conditions and the following disclaimer in the 
 *    documentation and/or other materials provided with the   
 *    distribution.
 *
 *    Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT 
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT 
 *  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, 
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *****************************************************************************/

/*********************************************************************
 * INCLUDES
 */
#include <string.h>
#include <xdc/std.h>

#include <xdc/runtime/Error.h>
#include <xdc/runtime/System.h>

#include <ti/sysbios/BIOS.h>
#include <ti/sysbios/knl/Task.h>
#include <ti/sysbios/knl/Clock.h>
#include <ti/sysbios/knl/Semaphore.h>
#include <ti/sysbios/knl/Queue.h>

#include <driverlib/ioc.h>

#include "gap.h"
#include "gatt.h"
#include "hci_tl.h"
#include "linkdb.h"
#include "util.h"

#include "gattservapp.h"
#include "multi.h"
#include "gapbondmgr.h"

#include "osal_snv.h"
#include "icall_apimsg.h"

/*********************************************************************
 * MACROS
 */

/*********************************************************************
 * CONSTANTS
 */
// Profile Events
#define START_ADVERTISING_EVT         0x0001  // Start Advertising

#define DEFAULT_ADVERT_OFF_TIME       30000   // 30 seconds

#define DEFAULT_MIN_CONN_INTERVAL     0x0006  // 100 milliseconds
#define DEFAULT_MAX_CONN_INTERVAL     0x0C80  // 4 seconds

#define MIN_CONN_INTERVAL             0x0006
#define MAX_CONN_INTERVAL             0x0C80

#define DEFAULT_TIMEOUT_MULTIPLIER    1000

#define CONN_INTERVAL_MULTIPLIER      6

#define MIN_SLAVE_LATENCY             0
#define MAX_SLAVE_LATENCY             500

#define MIN_TIMEOUT_MULTIPLIER        0x000a
#define MAX_TIMEOUT_MULTIPLIER        0x0c80

#define MAX_TIMEOUT_VALUE             0xFFFF

// Task configuration
#define GAPROLE_TASK_PRIORITY         3

#ifndef GAPROLE_TASK_STACK_SIZE
#define GAPROLE_TASK_STACK_SIZE       440
#endif

#ifndef MAX_SCAN_RESPONSES
#define MAX_SCAN_RESPONSES 3
#endif

/*********************************************************************
 * TYPEDEFS
 */

// App event structure
typedef struct
{
  uint8_t  event;  // event type
  uint8_t  status; // event status
} gapRoleEvt_t;

// set/get param event structure
typedef struct
{
  uint16_t  connHandle;  
  uint16_t  value; 
} gapRoleInfoParam_t;

typedef struct
{
  uint8_t paramUpdateEnable;
  uint16_t minConnInterval;
  uint16_t maxConnInterval;
  uint16_t slaveLatency;
  uint16_t timeoutMultiplier;
  uint16_t connHandle;
} gapRole_updateConnParams_t;


/*********************************************************************
 * GLOBAL VARIABLES
 */

// array to store index to connection handle map
uint16_t connHandleMap[MAX_NUM_BLE_CONNS];

// Link DB maximum number of connections
uint8 linkDBNumConns;

/*********************************************************************
 * EXTERNAL VARIABLES
 */

/*********************************************************************
 * EXTERNAL FUNCTIONS
 */

/*********************************************************************
 * LOCAL VARIABLES
 */
// Entity ID globally used to check for source and/or destination of messages
static ICall_EntityID selfEntity;

// Semaphore globally used to post events to the application thread
static ICall_Semaphore sem;

// Queue object used for app messages
static Queue_Struct appMsg;

// Clock object used to signal timeout
static Clock_Struct startAdvClock;

// Task pending events
static uint16_t events = 0;

// Task setup
Task_Struct gapRoleTask;
Char gapRoleTaskStack[GAPROLE_TASK_STACK_SIZE];

/*********************************************************************
 * Profile Parameters - reference GAPROLE_PROFILE_PARAMETERS for
 * descriptions
 */
static uint8_t  gapRole_profileRole;
static uint8_t  gapRole_IRK[KEYLEN];
static uint8_t  gapRole_SRK[KEYLEN];
static uint32_t gapRole_signCounter;
static uint8_t  gapRole_bdAddr[B_ADDR_LEN];
static uint8_t  gapRole_AdvEnabled = TRUE;
static uint8_t  gapRole_AdvNonConnEnabled = FALSE;
static uint16_t gapRole_AdvertOffTime = DEFAULT_ADVERT_OFF_TIME;
static uint8_t  gapRole_AdvertDataLen = 3;

static uint8_t  gapRole_AdvertData[B_MAX_ADV_LEN] =
{
  0x02,   // length of this data
  GAP_ADTYPE_FLAGS,   // AD Type = Flags
  // Limited Discoverable & BR/EDR not supported
  (GAP_ADTYPE_FLAGS_GENERAL | GAP_ADTYPE_FLAGS_BREDR_NOT_SUPPORTED),
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// Connection parameter update parameters.
static gapRole_updateConnParams_t gapRole_updateConnParams =
{
  .paramUpdateEnable = FALSE,
  .minConnInterval = DEFAULT_MIN_CONN_INTERVAL,
  .maxConnInterval = DEFAULT_MAX_CONN_INTERVAL,
  .slaveLatency = MIN_SLAVE_LATENCY,
  .timeoutMultiplier = DEFAULT_TIMEOUT_MULTIPLIER
};
static uint8_t paramUpdateNoSuccessOption = GAPROLE_NO_ACTION;
static uint16_t udpateConnHandle = 0xFFFE;

static uint8_t  gapRole_ScanRspDataLen = 0;
static uint8_t  gapRole_ScanRspData[B_MAX_ADV_LEN] = {0};
static uint8_t  gapRole_AdvEventType;
static uint8_t  gapRole_AdvDirectType;
static uint8_t  gapRole_AdvDirectAddr[B_ADDR_LEN] = {0};
static uint8_t  gapRole_AdvChanMap;
static uint8_t  gapRole_AdvFilterPolicy;

//these are the same for each connection
static uint16_t gapRole_MinConnInterval = DEFAULT_MIN_CONN_INTERVAL;
static uint16_t gapRole_MaxConnInterval = DEFAULT_MAX_CONN_INTERVAL;
static uint16_t gapRole_SlaveLatency = MIN_SLAVE_LATENCY;
static uint16_t gapRole_TimeoutMultiplier = DEFAULT_TIMEOUT_MULTIPLIER;

static uint8_t  gapRoleMaxScanRes = 0;

// Application callbacks
static gapRolesCBs_t *pGapRoles_AppCGs = NULL;

/*********************************************************************
 * Profile Attributes - variables
 */

/*********************************************************************
 * Profile Attributes - Table
 */

/*********************************************************************
 * LOCAL FUNCTIONS
 */
static void gapRole_init(void);
static void gapRole_taskFxn(UArg a0, UArg a1);

static uint8 gapRole_processStackMsg(ICall_Hdr *pMsg);
static uint8_t gapRole_processGAPMsg(gapEventHdr_t *pMsg);
static void gapRole_SetupGAP(void);
static void gapRole_setEvent(uint32_t event);

static bStatus_t gapRole_startConnUpdate(uint8_t handleFailure, 
                                       gapRole_updateConnParams_t *pConnParams);
static void      gapRole_HandleParamUpdateNoSuccess(void);

// for debugging
static void gapRole_abort(void);

/*********************************************************************
 * CALLBACKS
 */
void gapRole_clockHandler(UArg a0);

/*********************************************************************
 * PUBLIC FUNCTIONS
 */

/*********************************************************************
 * @brief   Set a GAP Role parameter.
 *
 * Public function defined in peripheral.h.
 */
bStatus_t GAPRole_SetParameter(uint16_t param, uint8_t len, void *pValue, uint8 connHandle)
{
  bStatus_t ret = SUCCESS;
  switch (param)
  {
    case GAPROLE_IRK:
      if (len == KEYLEN)
      {
        VOID memcpy(gapRole_IRK, pValue, KEYLEN) ;
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_SRK:
      if (len == KEYLEN)
      {
        VOID memcpy(gapRole_SRK, pValue, KEYLEN) ;
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_SIGNCOUNTER:
      if (len == sizeof (uint32_t))
      {
        gapRole_signCounter = *((uint32_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADVERT_ENABLED:  //connectable advertising
      if (len == sizeof(uint8_t))
      {
        // Non-connectable advertising must be disabled.
        if (gapRole_AdvNonConnEnabled != TRUE)
        {
          uint8_t oldAdvEnabled = gapRole_AdvEnabled;
          gapRole_AdvEnabled = *((uint8_t*)pValue);

          if ((oldAdvEnabled) && (gapRole_AdvEnabled == FALSE))
          {
            return GAP_EndDiscoverable(selfEntity);
          }
          else if ((oldAdvEnabled == FALSE) && (gapRole_AdvEnabled))
          {
            // Turn on advertising.
            if (linkDB_NumActive() >= MAX_NUM_BLE_CONNS) //don't do conn adv if we don't have any avilable links
            {
              return bleNoResources; // no more available links
            }
            else
            {
              gapRole_setEvent(START_ADVERTISING_EVT);
            }
          }
        }
        else
        {
          ret = bleIncorrectMode;
        }
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;
      
    case GAPROLE_ADV_NONCONN_ENABLED:
      if (len == sizeof(uint8_t))
      {
        // Connectable advertising must be disabled.
        if (gapRole_AdvEnabled != TRUE)
        {
          uint8_t oldAdvEnabled = gapRole_AdvNonConnEnabled;
          gapRole_AdvNonConnEnabled = *((uint8_t*)pValue);
          
          if ((oldAdvEnabled) && (gapRole_AdvNonConnEnabled == FALSE))
          {
            VOID GAP_EndDiscoverable(selfEntity);
          }
          else if ((oldAdvEnabled == FALSE) && (gapRole_AdvNonConnEnabled))
          {  //turnon advertising
            gapRole_setEvent(START_ADVERTISING_EVT);
          }
        }
        else
        {
          ret = bleIncorrectMode;
        }
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADVERT_OFF_TIME:
      if (len == sizeof (uint16_t))
      {
        gapRole_AdvertOffTime = *((uint16_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADVERT_DATA:
      if (len <= B_MAX_ADV_LEN)
      {
        VOID memset(gapRole_AdvertData, 0, B_MAX_ADV_LEN);
        VOID memcpy(gapRole_AdvertData, pValue, len);
        gapRole_AdvertDataLen = len;
        
        // Update the advertising data
        ret = GAP_UpdateAdvertisingData(selfEntity,
                              TRUE, gapRole_AdvertDataLen, gapRole_AdvertData);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_SCAN_RSP_DATA:
      if (len <= B_MAX_ADV_LEN)
      {
        VOID memset(gapRole_ScanRspData, 0, B_MAX_ADV_LEN);
        VOID memcpy(gapRole_ScanRspData, pValue, len);
        gapRole_ScanRspDataLen = len;
        
        // Update the Response Data
        ret = GAP_UpdateAdvertisingData(selfEntity,
                              FALSE, gapRole_ScanRspDataLen, gapRole_ScanRspData);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADV_EVENT_TYPE:
      if ((len == sizeof (uint8_t)) && (*((uint8_t*)pValue) <= GAP_ADTYPE_ADV_LDC_DIRECT_IND))
      {
        gapRole_AdvEventType = *((uint8_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADV_DIRECT_TYPE:
      if ((len == sizeof (uint8_t)) && (*((uint8_t*)pValue) <= ADDRMODE_PRIVATE_RESOLVE)) 
      {
        gapRole_AdvDirectType = *((uint8_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADV_DIRECT_ADDR:
      if (len == B_ADDR_LEN)
      {
        VOID memcpy(gapRole_AdvDirectAddr, pValue, B_ADDR_LEN) ;
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADV_CHANNEL_MAP:
      if ((len == sizeof (uint8_t)) && (*((uint8_t*)pValue) <= 0x07))
      {
        gapRole_AdvChanMap = *((uint8_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_ADV_FILTER_POLICY:
      if ((len == sizeof (uint8_t)) && (*((uint8_t*)pValue) <= GAP_FILTER_POLICY_WHITE))
      {
        gapRole_AdvFilterPolicy = *((uint8_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;

    case GAPROLE_MIN_CONN_INTERVAL:
      {
        uint16_t newInterval = *((uint16_t*)pValue);
        if (  len == sizeof (uint16_t)           &&
             (newInterval >= MIN_CONN_INTERVAL) &&
             (newInterval <= MAX_CONN_INTERVAL))
        {
          gapRole_updateConnParams.minConnInterval = newInterval;
        }
        else
        {
          ret = bleInvalidRange;
        }
      }
      break;

    case GAPROLE_MAX_CONN_INTERVAL:
      {
        uint16_t newInterval = *((uint16_t*)pValue);
        if (  len == sizeof (uint16_t)          &&
             (newInterval >= MIN_CONN_INTERVAL) &&
             (newInterval <= MAX_CONN_INTERVAL))
        {
          gapRole_updateConnParams.maxConnInterval = newInterval;
        }
        else
        {
          ret = bleInvalidRange;
        }
      }
      break;

    case GAPROLE_SLAVE_LATENCY:
      {
        uint16_t latency = *((uint16_t*)pValue);
        if (len == sizeof (uint16_t) && (latency < MAX_SLAVE_LATENCY))
        {
          gapRole_updateConnParams.slaveLatency = latency;
        }
        else
        {
          ret = bleInvalidRange;
        }
      }
      break;

    case GAPROLE_TIMEOUT_MULTIPLIER:
      {
        uint16_t newTimeout = *((uint16_t*)pValue);
        if (len == sizeof (uint16_t)
            && (newTimeout >= MIN_TIMEOUT_MULTIPLIER) && (newTimeout <= MAX_TIMEOUT_MULTIPLIER))
        {
          gapRole_updateConnParams.timeoutMultiplier = newTimeout;
        }
        else
        {
          ret = bleInvalidRange;
        }
      }
      break;
    
  case GAPROLE_MAX_SCAN_RES:
      if (len == sizeof (uint8_t))
      {
        gapRoleMaxScanRes = *((uint8_t*)pValue);
      }
      else
      {
        ret = bleInvalidRange;
      }
      break;        
  
    default:
      // The param value isn't part of this profile, try the GAP.
      if ((param < TGAP_PARAMID_MAX) && (len == sizeof (uint16_t)))
      {
        ret = GAP_SetParamValue(param, *((uint16_t*)pValue));
      }
      else
      {
        ret = INVALIDPARAMETER;
      }
      break;
  }

  return (ret);
}

/*********************************************************************
 * @brief   Get a GAP Role parameter.
 *
 * Public function defined in peripheral.h.
 */
bStatus_t GAPRole_GetParameter(uint16_t param, void *pValue, uint8_t connHandle)
{
  bStatus_t ret = SUCCESS;
  linkDBInfo_t pInfo;
  
  switch (param)
  {
    case GAPROLE_PROFILEROLE:
      *((uint8_t*)pValue) = gapRole_profileRole;
      break;

    case GAPROLE_IRK:
      VOID memcpy(pValue, gapRole_IRK, KEYLEN) ;
      break;

    case GAPROLE_SRK:
      VOID memcpy(pValue, gapRole_SRK, KEYLEN) ;
      break;

    case GAPROLE_SIGNCOUNTER:
      *((uint32_t*)pValue) = gapRole_signCounter;
      break;

    case GAPROLE_BD_ADDR:
      VOID memcpy(pValue, gapRole_bdAddr, B_ADDR_LEN) ;
      break;

    case GAPROLE_ADVERT_ENABLED:
      *((uint8_t*)pValue) = gapRole_AdvEnabled;
      break;

    case GAPROLE_ADV_NONCONN_ENABLED:
      *((uint8_t*)pValue) = gapRole_AdvNonConnEnabled;
      break;
      
    case GAPROLE_ADVERT_OFF_TIME:
      *((uint16_t*)pValue) = gapRole_AdvertOffTime;
      break;

    case GAPROLE_ADVERT_DATA:
      VOID memcpy(pValue , gapRole_AdvertData, gapRole_AdvertDataLen);
      break;

    case GAPROLE_SCAN_RSP_DATA:
      VOID memcpy(pValue, gapRole_ScanRspData, gapRole_ScanRspDataLen) ;
      break;

    case GAPROLE_ADV_EVENT_TYPE:
      *((uint8_t*)pValue) = gapRole_AdvEventType;
      break;

    case GAPROLE_ADV_DIRECT_TYPE:
      *((uint8_t*)pValue) = gapRole_AdvDirectType;
      break;

    case GAPROLE_ADV_DIRECT_ADDR:
      VOID memcpy(pValue, gapRole_AdvDirectAddr, B_ADDR_LEN) ;
      break;

    case GAPROLE_ADV_CHANNEL_MAP:
      *((uint8_t*)pValue) = gapRole_AdvChanMap;
      break;

    case GAPROLE_ADV_FILTER_POLICY:
      *((uint8_t*)pValue) = gapRole_AdvFilterPolicy;
      break;
      
    case GAPROLE_MIN_CONN_INTERVAL:
      *((uint16_t*)pValue) = gapRole_updateConnParams.minConnInterval;
      break;

    case GAPROLE_MAX_CONN_INTERVAL:
      *((uint16_t*)pValue) = gapRole_updateConnParams.maxConnInterval;
      break;

    case GAPROLE_SLAVE_LATENCY:
      *((uint16_t*)pValue) = gapRole_updateConnParams.slaveLatency;
      break;

    case GAPROLE_TIMEOUT_MULTIPLIER:
      *((uint16_t*)pValue) = gapRole_updateConnParams.timeoutMultiplier;
      break;

    case GAPROLE_CONN_BD_ADDR:
      linkDB_GetInfo(connHandle, &pInfo);
      VOID memcpy(pValue, pInfo.addr, B_ADDR_LEN) ;        
      break;

    case GAPROLE_CONN_INTERVAL:
      linkDB_GetInfo(connHandle, &pInfo);
      *((uint16_t*)pValue) = pInfo.connInterval;

    case GAPROLE_CONN_LATENCY:
    //wait for linkdb update
//      linkDB_GetInfo(connHandle, &pInfo);
//      *((uint16_t*)pValue) = pInfo.connLatency;    
      break;

    case GAPROLE_CONN_TIMEOUT:
    //wait for linkdb update
//      linkDB_GetInfo(connHandle, &pInfo);
//      *((uint16_t*)pValue) = pInfo.connTimeout;    
      break;
      
    case GAPROLE_MAX_SCAN_RES:
      *((uint8_t*)pValue) = gapRoleMaxScanRes;
      break;      
    
    default:
      // The param value isn't part of this profile, try the GAP.
      if (param < TGAP_PARAMID_MAX)
      {
        *((uint16_t*)pValue) = GAP_GetParamValue(param);
      }
      else
      {
        ret = INVALIDPARAMETER;
      }
      break;
  }

  return (ret);
}

/*********************************************************************
 * @brief   Does the device initialization.
 *
 * Public function defined in peripheral.h.
 */
bStatus_t GAPRole_StartDevice(gapRolesCBs_t *pAppCallbacks)
{
  // Clear all of the Application callbacks
  if (pAppCallbacks)
  {
    pGapRoles_AppCGs = pAppCallbacks;
  }
  
  // Start the GAP
  gapRole_SetupGAP();
  
  return (SUCCESS);
}

/*********************************************************************
 * @brief   Terminates the existing connection.
 *
 * Public function defined in peripheral.h.
 */
bStatus_t GAPRole_TerminateConnection(uint16_t connHandle)
{
    return (GAP_TerminateLinkReq(selfEntity, connHandle, 
                                 HCI_DISCONNECT_REMOTE_USER_TERM));
}

/*********************************************************************
 * @fn      GAPRole_createTask
 *
 * @brief   Task creation function for the GAP Peripheral Role.
 *
 * @param   none
 *
 * @return  none
 */
void GAPRole_createTask(void)
{
  Task_Params taskParams;

  // Configure task
  Task_Params_init(&taskParams);
  taskParams.stack = gapRoleTaskStack;
  taskParams.stackSize = GAPROLE_TASK_STACK_SIZE;
  taskParams.priority = GAPROLE_TASK_PRIORITY;
  
  Task_construct(&gapRoleTask, gapRole_taskFxn, &taskParams, NULL);
}

/*********************************************************************
 * LOCAL FUNCTION PROTOTYPES
 */

/*********************************************************************
 * @fn      gapRole_init
 *
 * @brief   Initialization function for the GAP Role Task.
 *
 * @param   none
 *
 * @return  none
 */
static void gapRole_init(void)
{ 
  // Register the current thread as an ICall dispatcher application
  // so that the application can send and receive messages.
  ICall_registerApp(&selfEntity, &sem);
  
  // Get link DB maximum number of connections
  linkDBNumConns = linkDB_NumConns();

  // Setup timers as one-shot timers
  Util_constructClock(&startAdvClock, gapRole_clockHandler, 
                      0, 0, false, START_ADVERTISING_EVT);
  
  // Initialize the Profile Advertising and Connection Parameters
  gapRole_profileRole = GAP_PROFILE_PERIPHERAL | GAP_PROFILE_CENTRAL;
  VOID memset(gapRole_IRK, 0, KEYLEN);
  VOID memset(gapRole_SRK, 0, KEYLEN);
  gapRole_signCounter = 0;
  gapRole_AdvEventType = GAP_ADTYPE_ADV_IND;
  gapRole_AdvDirectType = ADDRTYPE_PUBLIC;
  gapRole_AdvChanMap = GAP_ADVCHAN_ALL;
  gapRole_AdvFilterPolicy = GAP_FILTER_POLICY_ALL;

  // Restore Items from NV
  VOID osal_snv_read(BLE_NVID_IRK, KEYLEN, gapRole_IRK);
  VOID osal_snv_read(BLE_NVID_CSRK, KEYLEN, gapRole_SRK);
  VOID osal_snv_read(BLE_NVID_SIGNCOUNTER, sizeof(uint32_t), 
                     &gapRole_signCounter);
}


/**
 * @brief   Terminate a link.
 *
 * Public function defined in central.h.
 */
bStatus_t GAPRole_TerminateLink(uint16_t connHandle)
{
  return GAP_TerminateLinkReq(selfEntity, connHandle, HCI_DISCONNECT_REMOTE_USER_TERM) ;
}

/**
 * @brief   Establish a link to a peer device.
 *
 * Public function defined in central.h.
 */
bStatus_t GAPRole_EstablishLink(uint8_t highDutyCycle, uint8_t whiteList,
                                        uint8_t addrTypePeer, uint8_t *peerAddr)
{
  gapEstLinkReq_t params;
  
  params.taskID = ICall_getLocalMsgEntityId(ICALL_SERVICE_CLASS_BLE_MSG, 
                                            selfEntity);
  params.highDutyCycle = highDutyCycle;
  params.whiteList = whiteList;
  params.addrTypePeer = addrTypePeer;
  VOID memcpy(params.peerAddr, peerAddr, B_ADDR_LEN);

  return GAP_EstablishLinkReq(&params);
}

/**
 * @brief   Start a device discovery scan.
 *
 * Public function defined in central.h.
 */
bStatus_t GAPRole_StartDiscovery(uint8_t mode, uint8_t activeScan, uint8_t whiteList)
{
  gapDevDiscReq_t params;

  params.taskID = ICall_getLocalMsgEntityId(ICALL_SERVICE_CLASS_BLE_MSG, 
                                            selfEntity);
  params.mode = mode;
  params.activeScan = activeScan;
  params.whiteList = whiteList;

  return GAP_DeviceDiscoveryRequest(&params);
}

/**
 * @brief   Cancel a device discovery scan.
 *
 * Public function defined in central.h.
 */
bStatus_t GAPRole_CancelDiscovery(void)
{
  return GAP_DeviceDiscoveryCancel(selfEntity);
}

/*********************************************************************
 * @fn      gapRole_taskFxn
 *
 * @brief   Task entry point for the GAP Peripheral Role.
 *
 * @param   a0 - first argument
 * @param   a1 - second argument
 *
 * @return  none
 */
static void gapRole_taskFxn(UArg a0, UArg a1)
{  
  // Initialize profile
  gapRole_init();
  
  // Profile main loop
  for (;;)
  {
    // Waits for a signal to the semaphore associated with the calling thread.
    // Note that the semaphore associated with a thread is signaled when a
    // message is queued to the message receive queue of the thread or when
    // ICall_signal() function is called onto the semaphore.
    ICall_Errno errno = ICall_wait(ICALL_TIMEOUT_FOREVER);

    if (errno == ICALL_ERRNO_SUCCESS)
    {
      ICall_EntityID dest;
      ICall_ServiceEnum src;
      ICall_HciExtEvt *pMsg = NULL;
      
      if (ICall_fetchServiceMsg(&src, &dest, 
                                (void **)&pMsg) == ICALL_ERRNO_SUCCESS)
      {
        uint8_t safeToDealloc = TRUE;
        
        if ((src == ICALL_SERVICE_CLASS_BLE) && (dest == selfEntity))
        {
          ICall_Stack_Event *pEvt = (ICall_Stack_Event *)pMsg;
          
          // Check for BLE stack events first
          if (pEvt->signature == 0xffff)
          {
            if (pEvt->event_flag & GAP_EVENT_SIGN_COUNTER_CHANGED)
            {
              // Sign counter changed, save it to NV
              VOID osal_snv_write(BLE_NVID_SIGNCOUNTER, sizeof(uint32_t), 
                                  &gapRole_signCounter);
            }
          }
          else
          {
            // Process inter-task message
            safeToDealloc = gapRole_processStackMsg((ICall_Hdr *)pMsg);
          }
        }

        if (pMsg && safeToDealloc)
        {
          ICall_freeMsg(pMsg);
        }
      }
    }

    if (events & START_ADVERTISING_EVT)
    { 
      events &= ~START_ADVERTISING_EVT;
      
      if (gapRole_AdvEnabled || gapRole_AdvNonConnEnabled)
      {
        gapAdvertisingParams_t params;

        // Setup advertisement parameters
        if (gapRole_AdvNonConnEnabled)
        {
          // Only advertise non-connectable undirected.
          params.eventType = GAP_ADTYPE_ADV_NONCONN_IND;
        }
        else
        {
          params.eventType = gapRole_AdvEventType;
          params.initiatorAddrType = gapRole_AdvDirectType;
          VOID memcpy(params.initiatorAddr, gapRole_AdvDirectAddr, B_ADDR_LEN);
        }
        
        params.channelMap = gapRole_AdvChanMap;
        params.filterPolicy = gapRole_AdvFilterPolicy;

        if (GAP_MakeDiscoverable(selfEntity, &params) != SUCCESS)
        {
          gapRole_abort();
        }
      }
    }
  } // for
}

/*********************************************************************
 * @fn      gapRole_processStackMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
static uint8_t gapRole_processStackMsg(ICall_Hdr *pMsg)
{
  uint8_t safeToDealloc = TRUE;
  
  switch (pMsg->event)
  {
    case GAP_MSG_EVENT:
      safeToDealloc = gapRole_processGAPMsg((gapEventHdr_t *)pMsg);
      break;

    default:
      break;
  }
  
  return (safeToDealloc);
}

/*********************************************************************
 * @fn      gapRole_processGAPMsg
 *
 * @brief   Process an incoming task message.
 *
 * @param   pMsg - message to process
 *
 * @return  none
 */
#pragma optimize=none
static uint8_t gapRole_processGAPMsg(gapEventHdr_t *pMsg)
{
  uint8_t notify = FALSE;   // State changed notify the app? (default no)

  switch (pMsg->opcode)
  {
    case GAP_DEVICE_INIT_DONE_EVENT:
      {
        gapDeviceInitDoneEvent_t *pPkt = (gapDeviceInitDoneEvent_t *)pMsg;
        bStatus_t stat = pPkt->hdr.status;

        if (stat == SUCCESS)
        {
          // Save off the generated keys
          VOID osal_snv_write(BLE_NVID_IRK, KEYLEN, gapRole_IRK);
          VOID osal_snv_write(BLE_NVID_CSRK, KEYLEN, gapRole_SRK);

          // Save off the information
          VOID memcpy(gapRole_bdAddr, pPkt->devAddr, B_ADDR_LEN);

          // Update the advertising data
          stat = GAP_UpdateAdvertisingData(selfEntity,
                              TRUE, gapRole_AdvertDataLen, gapRole_AdvertData);
        }

        if (stat != SUCCESS)
        {
          gapRole_abort();
        }

        notify = TRUE;
      }
      break;

    case GAP_ADV_DATA_UPDATE_DONE_EVENT:
      {
        gapAdvDataUpdateEvent_t *pPkt = (gapAdvDataUpdateEvent_t *)pMsg;

        if (pPkt->hdr.status == SUCCESS)
        {
          if (pPkt->adType)
          {
            // Setup the Response Data
            pPkt->hdr.status = GAP_UpdateAdvertisingData(selfEntity,
                              FALSE, gapRole_ScanRspDataLen, gapRole_ScanRspData);
          }
          else if (Util_isActive(&startAdvClock) == FALSE)
          {
            // Start advertising
            gapRole_setEvent(START_ADVERTISING_EVT);
          }
        }

        if (pPkt->hdr.status != SUCCESS)
        {
          // Set into Error state
          gapRole_abort();
          notify = TRUE;
        }
      }
      break;

    case GAP_MAKE_DISCOVERABLE_DONE_EVENT:
    case GAP_END_DISCOVERABLE_DONE_EVENT:
      {
        gapMakeDiscoverableRspEvent_t *pPkt = (gapMakeDiscoverableRspEvent_t *)pMsg;

        if (pPkt->hdr.status == SUCCESS)
        {
          if (pMsg->opcode == GAP_MAKE_DISCOVERABLE_DONE_EVENT)
          {
            gapRole_AdvEnabled = TRUE;
          }
          else // GAP_END_DISCOVERABLE_DONE_EVENT
          {
            if (gapRole_AdvertOffTime != 0) //restart advertising if param is set
            {
              if ((gapRole_AdvEnabled) || (gapRole_AdvNonConnEnabled))
              {
                Util_restartClock(&startAdvClock, gapRole_AdvertOffTime);
              }
            }
            else
            {
              // Since gapRole_AdvertOffTime is set to 0, the device should not
              // automatically become discoverable again after a period of time.
              // Set enabler to FALSE; device will become discoverable again when
              // this value gets set to TRUE
              if (gapRole_AdvEnabled == TRUE)
              {
                gapRole_AdvEnabled = FALSE;
              }
            }
          }
          notify = TRUE;
        }
        else if (pPkt->hdr.status == LL_STATUS_ERROR_COMMAND_DISALLOWED) //we're already advertising
        {
          notify = FALSE;
        }
        else
        {
          gapRole_abort();
        }
      }
      break;

    case GAP_LINK_ESTABLISHED_EVENT:
      {
        gapEstLinkReqEvent_t *pPkt = (gapEstLinkReqEvent_t *)pMsg;

//        if (pPkt->hdr.status == SUCCESS)
//        {
//          // advertising will stop when a connection forms in the peripheral role
//          if (pPkt->connRole == GAP_PROFILE_PERIPHERAL)
//          {
//            gapRole_AdvEnabled = FALSE;
//          }
//        }
//        else if (pPkt->hdr.status == bleGAPConnNotAcceptable)
        if (pPkt->hdr.status == bleGAPConnNotAcceptable)
        {
          // Set enabler to FALSE; device will become discoverable again when
          // this value gets set to TRUE
          gapRole_AdvEnabled = FALSE;
        }
//        else
//        {
//          gapRole_abort();
//        }
        notify = TRUE;
      }
      break;

    case GAP_LINK_TERMINATED_EVENT:
      {
        gapTerminateLinkEvent_t *pPkt = (gapTerminateLinkEvent_t *)pMsg;

        linkDBInfo_t pInfo;
        linkDB_GetInfo(pPkt->connectionHandle, &pInfo);
        
        // notify bond manager
        GAPBondMgr_LinkTerm(pPkt->connectionHandle);

        notify = TRUE;
      }
      break;

    case GAP_SLAVE_REQUESTED_SECURITY_EVENT:
      {
        uint16_t connHandle = ((gapSlaveSecurityReqEvent_t *)pMsg)->connectionHandle;
        uint8_t authReq = ((gapSlaveSecurityReqEvent_t *)pMsg)->authReq;
        
        GAPBondMgr_SlaveReqSecurity(connHandle, authReq);
      }
      break;     
      
    default:
      notify = TRUE;
      break;
  }

  if (notify == TRUE) //app needs to take further action
  {
    if (pGapRoles_AppCGs && pGapRoles_AppCGs->pfnPassThrough)
    {
      return (pGapRoles_AppCGs->pfnPassThrough((gapMultiRoleEvent_t *)pMsg));
    }
  }
  
  return TRUE;
    
}

/*********************************************************************
 * @fn      gapRole_HandleParamUpdateNoSuccess
 *
 * @brief   Handle unsuccessful connection parameters update.
 *
 * @param   none
 *
 * @return  none
 */
static void gapRole_HandleParamUpdateNoSuccess(void)
{
  // See which option was chosen for unsuccessful updates
  switch (paramUpdateNoSuccessOption)
  {
    case GAPROLE_RESEND_PARAM_UPDATE:
      GAPRole_SendUpdateParam(gapRole_MinConnInterval, gapRole_MaxConnInterval,
                              gapRole_SlaveLatency, gapRole_TimeoutMultiplier,
                              GAPROLE_RESEND_PARAM_UPDATE, udpateConnHandle);
      break;

    case GAPROLE_TERMINATE_LINK:
      GAPRole_TerminateConnection(udpateConnHandle);
      break;

    case GAPROLE_NO_ACTION:
      // fall through
    default:
      //do nothing
      break;
  }
}

/********************************************************************
 * @fn          gapRole_startConnUpdate
 *
 * @brief       Start the connection update procedure
 *
 * @param       handleFailure - what to do if the update does not occur.
 *              Method may choose to terminate connection, try again,
 *              or take no action
 * @param       pConnParams   - connection parameters to use
 *
 * @return      SUCCESS: operation was successful.
 *              INVALIDPARAMETER: Data can not fit into one packet.
 *              MSG_BUFFER_NOT_AVAIL: No HCI buffer is available.
 *              bleInvalidRange: 
 *              bleIncorrectMode: invalid profile role.
 *              bleAlreadyInRequestedMode: already updating link parameters.
 *              bleNotConnected: Connection is down
 *              bleMemAllocError: Memory allocation error occurred.
 *              bleNoResources: No available resource
 */
static bStatus_t gapRole_startConnUpdate(uint8_t handleFailure, 
                                        gapRole_updateConnParams_t *pConnParams)
{
  bStatus_t status;
#ifndef BROKEN
  // First check the current connection parameters versus the configured parameters
  if ((gapRole_ConnInterval < pConnParams->minConnInterval)   ||
       (gapRole_ConnInterval > pConnParams->maxConnInterval)   ||
       (gapRole_ConnSlaveLatency != pConnParams->slaveLatency) ||
       (gapRole_ConnTimeout  != pConnParams->timeoutMultiplier))  
  {
    uint16_t timeout = GAP_GetParamValue(TGAP_CONN_PARAM_TIMEOUT);
#if defined(L2CAP_CONN_UPDATE)
    l2capParamUpdateReq_t updateReq;

    updateReq.intervalMin = pConnParams->minConnInterval;
    updateReq.intervalMax = pConnParams->maxConnInterval;
    updateReq.slaveLatency = pConnParams->slaveLatency;
    updateReq.timeoutMultiplier = pConnParams->timeoutMultiplier;

    status =  L2CAP_ConnParamUpdateReq(gapRole_ConnectionHandle, &updateReq, selfEntity);
#else
    gapUpdateLinkParamReq_t linkParams;

    linkParams.connectionHandle = gapRole_ConnectionHandle;
    linkParams.intervalMin = pConnParams->minConnInterval;
    linkParams.intervalMax = pConnParams->maxConnInterval;
    linkParams.connLatency = pConnParams->slaveLatency;
    linkParams.connTimeout = pConnParams->timeoutMultiplier;

    status = GAP_UpdateLinkParamReq( &linkParams );
#endif // L2CAP_CONN_UPDATE

    if(status == SUCCESS)
    {
      paramUpdateNoSuccessOption = handleFailure;
      // Let's wait either for L2CAP Connection Parameters Update Response or
      // for Controller to update connection parameters
      Util_restartClock(&updateTimeoutClock, timeout);
    }
  }
  else
  {
    status = bleInvalidRange;
  }
#endif // BROKEN
  
  return status;
}

/********************************************************************
 * @fn          GAPRole_SendUpdateParam
 *
 * @brief       Update the parameters of an existing connection
 *
 * @param       minConnInterval - the new min connection interval
 * @param       maxConnInterval - the new max connection interval
 * @param       latency - the new slave latency
 * @param       connTimeout - the new timeout value
 * @param       handleFailure - what to do if the update does not occur.
 *              Method may choose to terminate connection, try again,
 *              or take no action
 *
 * @return      SUCCESS: operation was successful.
 *              INVALIDPARAMETER: Data can not fit into one packet.
 *              MSG_BUFFER_NOT_AVAIL: No HCI buffer is available.
 *              bleInvalidRange: 
 *              bleIncorrectMode: invalid profile role.
 *              bleAlreadyInRequestedMode: already updating link parameters.
 *              bleNotConnected: Connection is down
 *              bleMemAllocError: Memory allocation error occurred.
 *              bleNoResources: No available resource
 */
bStatus_t GAPRole_SendUpdateParam(uint16_t minConnInterval,
                                  uint16_t maxConnInterval,
                                  uint16_t latency, uint16_t connTimeout,
                                  uint8_t handleFailure, uint16_t connHandle)
{
  // If there is no existing connection no update need be sent
  if (linkDB_NumActive())
  {
    return (bleNotConnected);
  }
  else
  {
    gapRole_updateConnParams_t paramUpdate;
    
    paramUpdate.minConnInterval = minConnInterval;
    paramUpdate.maxConnInterval = maxConnInterval;
    paramUpdate.slaveLatency = latency;
    paramUpdate.timeoutMultiplier = connTimeout;
    paramUpdate.connHandle = connHandle;

#ifndef BROKEN
    // Connection update requested by app, cancel such pending procedure (if active)
    Util_stopClock(&startUpdateClock);
#endif // BROKEN    

    // Start connection update procedure
    return gapRole_startConnUpdate(handleFailure, &paramUpdate);
  }
}

/*********************************************************************
 * @fn      gapRole_SetupGAP
 *
 * @brief   Call the GAP Device Initialization function using the
 *          Profile Parameters.
 *
 * @param   none
 *
 * @return  none
 */
static void gapRole_SetupGAP(void)
{
  VOID GAP_DeviceInit(selfEntity, gapRole_profileRole, MAX_SCAN_RESPONSES, gapRole_IRK,
                      gapRole_SRK, (uint32*)&gapRole_signCounter);
}

/*********************************************************************
 * @fn      gapRole_setEvent
 *
 * @brief   Set an event
 *
 * @param   event - event to be set
 *
 * @return  none
 */
static void gapRole_setEvent(uint32_t event)
{
  events |= event;
      
  // Wake up the application thread when it waits for clock event
  Semaphore_post(sem);
}

/*********************************************************************
 * @fn      gapRole_clockHandler
 *
 * @brief   Clock handler function
 *
 * @param   a0 - event
 *
 * @return  none
 */
void gapRole_clockHandler(UArg a0)
{
  gapRole_setEvent(a0);
}


/*********************************************************************
 * @fn      gapRole_abort
 *
 * @brief   to catch errors during debugging
 *
 * @return  none
 */
static void gapRole_abort(void)
{
#ifdef GAP_ROLE_ABORT  
  while(1);
#endif  
}