/*
 * Copyright (C) 2010 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * File            : com_trustedlogic_trustednfc_android_internal_NativeNfcManager.c
 * Original-Author : Trusted Logic S.A. (Jeremie Corbier)
 * Created         : 20-08-2009
 */

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>

#include "trustednfc_jni.h"

#define ERROR_BUFFER_TOO_SMALL -12
#define ERROR_INSUFFICIENT_RESOURCES -9

static phLibNfc_sConfig_t   gDrvCfg;
static void                 *gHWRef;
static phNfc_sData_t gInputParam;
static phNfc_sData_t gOutputParam;

static phLibNfc_Handle              hLlcpHandle;
static NFCSTATUS                    lastErrorStatus = NFCSTATUS_FAILED;
static phLibNfc_Llcp_eLinkStatus_t  g_eLinkStatus = phFriNfc_LlcpMac_eLinkDefault;

static sem_t trustednfc_jni_manager_sem;
static sem_t trustednfc_jni_llcp_sem;
static sem_t trustednfc_jni_open_sem;
static sem_t trustednfc_jni_init_sem;

static NFCSTATUS            trustednfc_jni_cb_status = NFCSTATUS_FAILED;

static jmethodID cached_NfcManager_notifyNdefMessageListeners;
static jmethodID cached_NfcManager_notifyTransactionListeners;
static jmethodID cached_NfcManager_notifyLlcpLinkActivation;
static jmethodID cached_NfcManager_notifyLlcpLinkDeactivated;
static jmethodID cached_NfcManager_notifyTargetDeselected;

namespace android {

phLibNfc_Handle     hIncommingLlcpSocket;
sem_t               trustednfc_jni_llcp_listen_sem;

struct trustednfc_jni_native_data *exported_nat = NULL;

/* Internal functions declaration */
static void *trustednfc_jni_client_thread(void *arg);
static void trustednfc_jni_init_callback(void *pContext, NFCSTATUS status);
static void trustednfc_jni_deinit_callback(void *pContext, NFCSTATUS status);
static void trustednfc_jni_discover_callback(void *pContext, NFCSTATUS status);
static void trustednfc_jni_se_set_mode_callback(void *context,
   phLibNfc_Handle handle, NFCSTATUS status);
static void trustednfc_jni_start_discovery(struct trustednfc_jni_native_data *nat);

static phLibNfc_eConfigLinkType parseLinkType(const char* link_name)
{
   struct link_name_entry {
      phLibNfc_eConfigLinkType   value;
      const char *               name;
   };
   const struct link_name_entry sLinkNameTable[] = {
      {ENUM_LINK_TYPE_COM1, "COM1"},
      {ENUM_LINK_TYPE_COM2, "COM2"},
      {ENUM_LINK_TYPE_COM3, "COM3"},
      {ENUM_LINK_TYPE_COM4, "COM4"},
      {ENUM_LINK_TYPE_COM5, "COM5"},
      {ENUM_LINK_TYPE_COM6, "COM6"},
      {ENUM_LINK_TYPE_COM7, "COM7"},
      {ENUM_LINK_TYPE_COM8, "COM8"},
      {ENUM_LINK_TYPE_I2C,  "I2C"},
      {ENUM_LINK_TYPE_USB,  "USB"},
   };
   phLibNfc_eConfigLinkType ret;
   unsigned int i;

   /* NOTE: ENUM_LINK_TYPE_NB corresponds to undefined link name  */

   if (link_name == NULL)
   {
      return ENUM_LINK_TYPE_NB;
   }

   ret = ENUM_LINK_TYPE_NB;
   for (i=0 ; i<sizeof(sLinkNameTable)/sizeof(link_name_entry) ; i++)
   {
      if (strcmp(sLinkNameTable[i].name, link_name) == 0)
      {
         ret = sLinkNameTable[i].value;
         break;
      }
   }

   return ret;
}


/*
 * Deferred callback called when client thread must be exited
 */
static void client_kill_deferred_call(void* arg)
{
   struct trustednfc_jni_native_data *nat = (struct trustednfc_jni_native_data *)arg;
   
   nat->running = FALSE;
}

static void kill_client(trustednfc_jni_native_data *nat)
{
   phDal4Nfc_Message_Wrapper_t  wrapper;
   phLibNfc_DeferredCall_t     *pMsg;
   
   LOGD("Terminating client thead...");
    
   pMsg = (phLibNfc_DeferredCall_t*)malloc(sizeof(phLibNfc_DeferredCall_t));
   pMsg->pCallback = client_kill_deferred_call;
   pMsg->pParameter = (void*)nat;
   
   wrapper.msg.eMsgType = PH_LIBNFC_DEFERREDCALL_MSG;
   wrapper.msg.pMsgData = pMsg;
   wrapper.msg.Size     = sizeof(phLibNfc_DeferredCall_t);

   phDal4Nfc_msgsnd(gDrvCfg.nClientId, (struct msgbuf *)&wrapper, sizeof(phLibNfc_Message_t), 0);
}


/* Initialization function */
static int trustednfc_jni_initialize(struct trustednfc_jni_native_data *nat)
{
   struct timespec ts;
   NFCSTATUS status;
   phLibNfc_StackCapabilities_t caps;
   char value[PROPERTY_VALUE_MAX];
   int result = FALSE;
   phLibNfc_SE_List_t SE_List[PHLIBNFC_MAXNO_OF_SE];
   uint8_t i, No_SE = PHLIBNFC_MAXNO_OF_SE, SmartMX_index=0, SmartMX_detected = 0;
   
   LOGD("Start Initialization\n");

   /* Configure hardware link */
   gDrvCfg.nClientId = phDal4Nfc_msgget(0, 0600);
   
   property_get("ro.nfc.port", value, "unknown");
   gDrvCfg.nLinkType = parseLinkType(value);

   LOGD("phLibNfc_Mgt_ConfigureDriver(0x%08x, 0x%08x)", gDrvCfg.nClientId, gDrvCfg.nLinkType);
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_ConfigureDriver(&gDrvCfg, &gHWRef);
   REENTRANCE_UNLOCK();
   if(status == NFCSTATUS_ALREADY_INITIALISED)
   {
      LOGW("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }
   else if(status != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   LOGD("phLibNfc_Mgt_ConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   
   /* TODO: here would be a good place to perform HW reset of the chip */
   
   if(pthread_create(&(nat->thread), NULL, trustednfc_jni_client_thread,
         nat) != 0)
   {
      LOGE("pthread_create failed");
      goto clean_and_return;
   }
     
   LOGD("phLibNfc_Mgt_Initialize()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_Initialize(gHWRef, trustednfc_jni_init_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_Mgt_Initialize() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   LOGD("phLibNfc_Mgt_Initialize returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
  
   /* Wait for callback response */
   sem_wait(&trustednfc_jni_init_sem);

   /* Initialization Status */
   if(nat->status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_GetstackCapabilities(&caps, (void *)nat);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_SUCCESS)
   {
      LOGW("phLibNfc_Mgt_GetstackCapabilities returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }
   else
   {
      LOGD("NFC capabilities: HAL = %x, FW = %x, HW = %x, Model = %x, HCI = %x",
         caps.psDevCapabilities.hal_version,
         caps.psDevCapabilities.fw_version,
         caps.psDevCapabilities.hw_version,
         caps.psDevCapabilities.model_id,
         caps.psDevCapabilities.hci_version);
   }

      
      /* Get Secure Element List */
      REENTRANCE_LOCK();
      LOGD("phLibNfc_SE_GetSecureElementList()");
      status = phLibNfc_SE_GetSecureElementList(SE_List, &No_SE);
      REENTRANCE_UNLOCK();
      if (status == NFCSTATUS_SUCCESS)
      {   
        LOGD("\n> Number of Secure Element(s) : %d\n", No_SE);
        /* Display Secure Element information */
        for (i = 0; i<No_SE; i++)
        {
          if (SE_List[i].eSE_Type == phLibNfc_SE_Type_SmartMX)
          {
            LOGD("phLibNfc_SE_GetSecureElementList(): SMX detected"); 
          }
          else if(SE_List[i].eSE_Type == phLibNfc_SE_Type_UICC)
          {
            LOGD("phLibNfc_SE_GetSecureElementList(): UICC detected"); 
          }
          
          /* Set SE mode - Off */
          LOGD("******  Initialize Secure Element ******");
          REENTRANCE_LOCK();
          status = phLibNfc_SE_SetMode(SE_List[i].hSecureElement,phLibNfc_SE_ActModeOff, trustednfc_jni_se_set_mode_callback,(void *)nat);
          REENTRANCE_UNLOCK();
       
          LOGD("phLibNfc_SE_SetMode for SE 0x%02x returned 0x%02x",SE_List[i].hSecureElement, status);
          if(status != NFCSTATUS_PENDING)
          {
            LOGE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
            goto clean_and_return;
          }
          LOGD("phLibNfc_SE_SetMode() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

          /* Wait for callback response */
          sem_wait(&trustednfc_jni_manager_sem);
        }
      }
      else
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): Error");
      }
      
   LOGI("NFC Initialized");

   result = TRUE;

clean_and_return:
   if (result != TRUE)
   {
      if(nat)
      {
         kill_client(nat);
      }
   }
   
   return result;
}


/* Deinitialization function */
static void trustednfc_jni_deinitialize(struct trustednfc_jni_native_data *nat)
{
   struct timespec ts;
   NFCSTATUS status;
   int bStackReset = FALSE;

   /* Clear previous configuration */
   memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));
   
   LOGD("phLibNfc_Mgt_DeInitialize() - 0x%08x", nat);
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_DeInitialize(gHWRef, trustednfc_jni_deinit_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if (status == NFCSTATUS_PENDING)
   {
      LOGD("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 10; 
  
      /* Wait for callback response */
      if(sem_timedwait(&trustednfc_jni_manager_sem, &ts) == -1)
      {
         LOGW("Operation timed out");
         bStackReset = TRUE;
      }
   }
   else
   {
      LOGW("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      bStackReset = TRUE;
   }

   if(bStackReset == TRUE)
   {
      /* Complete deinit. failed, try minimal reset (clean internal structures and free memory) */
      LOGW("Reseting stack...");
      REENTRANCE_LOCK();
      status = phLibNfc_Mgt_DeInitialize(gHWRef, NULL, NULL);
      REENTRANCE_UNLOCK();
      if (status != NFCSTATUS_SUCCESS)
      {
         /* NOTE: by design, this could not happen */
         LOGE("Reset failed [0x%08x]", status);
      }
      /* Force result to success (deinit shall not fail!) */
      nat->status = NFCSTATUS_SUCCESS;
   }

   /* Unconfigure driver */
   LOGD("phLibNfc_Mgt_UnConfigureDriver()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_UnConfigureDriver(gHWRef);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }
   else
   {
      LOGD("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }

   LOGI("NFC Deinitialized");
}

/*
 * Last-chance fallback when there is no clean way to recover
 * Performs a software reset
  */
void emergency_recovery(struct trustednfc_jni_native_data *nat)
{
   phLibNfc_sADD_Cfg_t discovery_cfg;
   phLibNfc_Registry_Info_t registration_cfg;
   
   LOGW("Emergency recovery called");
   
   /* Save current polling loop configuration */
   memcpy(&discovery_cfg, &nat->discovery_cfg, sizeof(phLibNfc_sADD_Cfg_t));
   memcpy(&registration_cfg, &nat->registry_info, sizeof(phLibNfc_Registry_Info_t));
   
   /* Deinit */
   trustednfc_jni_deinitialize(nat);
   
   /* Reinit */
   trustednfc_jni_initialize(nat);

   /* Restore polling loop configuration */
   memcpy(&nat->discovery_cfg, &discovery_cfg, sizeof(phLibNfc_sADD_Cfg_t));
   memcpy(&nat->registry_info, &registration_cfg, sizeof(phLibNfc_Registry_Info_t));

   /* Restart polling loop */
   trustednfc_jni_start_discovery(nat);
}

/*
 * Restart the polling loop when unable to perform disconnect
  */
void trustednfc_jni_restart_discovery(struct trustednfc_jni_native_data *nat)
{
   int ret;

   LOGW("Restarting polling loop");
   
   /* Restart Polling loop */
   LOGD("******  Start NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_RESUME,nat->discovery_cfg, trustednfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      nat->discovery_cfg.NfcIP_Mode, nat->discovery_cfg.Duration, ret);
   
   if (ret != NFCSTATUS_PENDING)
   {
   	emergency_recovery(nat);
   }     
}

/*
 *  Utility to get target type name from its specs
 */
static const char* get_target_type_name(phNfc_eRemDevType_t type, uint8_t sak)
{
   switch (type)
   {
      case phNfc_eISO14443_4A_PICC:
      case phNfc_eISO14443_A_PICC:
        {
          return TARGET_TYPE_ISO14443;
        }break;
        
      case phNfc_eISO14443_4B_PICC:
      case phNfc_eISO14443_B_PICC:
        {
          return TARGET_TYPE_ISO14443;
        }break;
        
      case phNfc_eMifare_PICC:
        {
          switch(sak)
          {
            case 0:
              {
                return TARGET_TYPE_MIFARE_UL;
              }break;
            case 8:
              {
                return TARGET_TYPE_MIFARE_1K;
              }break;
            case 24:
              {
                return TARGET_TYPE_MIFARE_4K;
              }break;
              
            case 32:
              {
                return TARGET_TYPE_MIFARE_DESFIRE;
              }break;
              
            default:
              {
                return TARGET_TYPE_MIFARE_UNKNOWN;
              }break;
          }
        }break;
      case phNfc_eFelica_PICC:
        {
          return TARGET_TYPE_FELICA;
        }break; 
      case phNfc_eJewel_PICC:
        {
          return TARGET_TYPE_JEWEL;
        }break; 
   }

   return TARGET_TYPE_UNKNOWN;
}

/*
 * NFC stack message processing
 */
static void *trustednfc_jni_client_thread(void *arg)
{
   struct trustednfc_jni_native_data *nat;
   JNIEnv *e;
   JavaVMAttachArgs thread_args;
   phDal4Nfc_Message_Wrapper_t wrapper;

   nat = (struct trustednfc_jni_native_data *)arg;

   thread_args.name = "NFC Message Loop";
   thread_args.version = nat->env_version;
   thread_args.group = NULL;

   nat->vm->AttachCurrentThread(&e, &thread_args);

   LOGI("NFC client started");
   nat->running = TRUE;
   while(nat->running == TRUE)
   {
      /* Fetch next message from the NFC stack message queue */
      if(phDal4Nfc_msgrcv(gDrvCfg.nClientId, (void *)&wrapper,
         sizeof(phLibNfc_Message_t), 0, 0) == -1)
      {
         LOGE("NFC client received bad message");
         continue;
      }

      switch(wrapper.msg.eMsgType)
      {
         case PH_LIBNFC_DEFERREDCALL_MSG:
         {
            phLibNfc_DeferredCall_t *msg =
               (phLibNfc_DeferredCall_t *)(wrapper.msg.pMsgData);

            REENTRANCE_LOCK();
            msg->pCallback(msg->pParameter);
            REENTRANCE_UNLOCK();

            break;
         }
      }
   }
   LOGI("NFC client stopped");
   
   nat->vm->DetachCurrentThread();

   return NULL;
}

extern uint8_t trustednfc_jni_is_ndef;
extern uint8_t *trustednfc_jni_ndef_buf;
extern uint32_t trustednfc_jni_ndef_buf_len;

static phLibNfc_sNfcIPCfg_t trustednfc_jni_nfcip1_cfg =
{
   3,
   { 0x46, 0x66, 0x6D }
}; 

/*
 * Callbacks
 */

/* P2P - LLCP callbacks */
static void trustednfc_jni_llcp_linkStatus_callback(void *pContext,
                                                    phFriNfc_LlcpMac_eLinkStatus_t   eLinkStatus)
{
   phFriNfc_Llcp_sLinkParameters_t  sLinkParams;
   JNIEnv *e;
   struct trustednfc_jni_native_data *nat;

   nat = (struct trustednfc_jni_native_data *)pContext;
   
   LOGD("Callback: trustednfc_jni_llcp_linkStatus_callback()");

   nat->vm->GetEnv( (void **)&e, nat->env_version);
   
   /* Update link status */
   g_eLinkStatus = eLinkStatus;

   if(eLinkStatus == phFriNfc_LlcpMac_eLinkActivated)
   {
      REENTRANCE_LOCK();
      phLibNfc_Llcp_GetRemoteInfo(hLlcpHandle, &sLinkParams);
      REENTRANCE_UNLOCK();
      LOGI("LLCP Link activated (LTO=%d, MIU=%d, OPTION=0x%02x, WKS=0x%02x)",sLinkParams.lto,
                                                                             sLinkParams.miu,
                                                                             sLinkParams.option,
                                                                             sLinkParams.wks);
   }
   else if(eLinkStatus == phFriNfc_LlcpMac_eLinkDeactivated)
   {
      LOGI("LLCP Link deactivated");
      /* Notify manager that the LLCP is lost or deactivated */
      e->CallVoidMethod(nat->manager, cached_NfcManager_notifyLlcpLinkDeactivated);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
      } 
   }
}

static void trustednfc_jni_checkLlcp_callback(void *context,
                                              NFCSTATUS status)
{
   trustednfc_jni_cb_status = status;
   
   PHNFC_UNUSED_VARIABLE(context);

   LOG_CALLBACK("trustednfc_jni_checkLlcp_callback", status);

   if(status == NFCSTATUS_SUCCESS)
   {
      LOGD("%s return status = 0x%x\n", __func__, status);

      sem_post(&trustednfc_jni_llcp_sem);
   }
}

static void trustednfc_jni_llcpcfg_callback(void *pContext, NFCSTATUS status)
{
   trustednfc_jni_cb_status = status;
   
   LOG_CALLBACK("trustednfc_jni_llcpcfg_callback", status);

   sem_post(&trustednfc_jni_manager_sem);
}

static void trustednfc_jni_p2pcfg_callback(void *pContext, NFCSTATUS status)
{
   trustednfc_jni_cb_status = status;

   LOG_CALLBACK("trustednfc_jni_p2pcfg_callback", status);

   sem_post(&trustednfc_jni_manager_sem);
}

static void trustednfc_jni_llcp_transport_listen_socket_callback(void              *pContext,
                                                                 phLibNfc_Handle   IncomingSocket)
{
   PHNFC_UNUSED_VARIABLE(pContext);

   LOGD("Callback: trustednfc_jni_llcp_transport_listen_socket_callback()");

   if(IncomingSocket != 0)
   {
      LOGD("Listen CB \n");
      hIncommingLlcpSocket = IncomingSocket;
      sem_post(&trustednfc_jni_llcp_listen_sem);  
   }
   else
   {
      LOGW("Listen KO");
   }
}

void trustednfc_jni_llcp_transport_socket_err_callback(void*      pContext,
                                                       uint8_t    nErrCode)
{
   PHNFC_UNUSED_VARIABLE(pContext);

   LOGD("Callback: trustednfc_jni_llcp_transport_socket_err_callback()");

   if(nErrCode == PHFRINFC_LLCP_ERR_FRAME_REJECTED)
   {
      LOGW("Frame Rejected - Disconnected");
   }
   else if(nErrCode == PHFRINFC_LLCP_ERR_DISCONNECTED)
   {
      LOGD("Socket Disconnected");
   }
}

static void trustednfc_jni_connect_callback(void *pContext,
   phLibNfc_Handle hRemoteDev,
   phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_connect_callback", status);
}


static void trustednfc_jni_discover_callback(void *pContext, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_discover_callback", status);

   //sem_post(&trustednfc_jni_manager_sem);
}


static void trustednfc_jni_ioctl_callback(void *pContext, phNfc_sData_t *pOutput, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_ioctl_callback", status);
}


static void trustednfc_jni_Discovery_notification_callback(void *pContext,
   phLibNfc_RemoteDevList_t *psRemoteDevList,
   uint8_t uNofRemoteDev, NFCSTATUS status)
{
   JNIEnv *e;
   NFCSTATUS ret;
   jclass tag_cls = NULL;
   jobject target_array;
   jobject tag;
   jmethodID ctor;
   jfieldID f;
   const char * typeName;
   jbyteArray tagUid;
   jbyteArray generalBytes = NULL;
   struct trustednfc_jni_native_data *nat;
   struct timespec ts;
   int i;

   nat = (struct trustednfc_jni_native_data *)pContext;
   
   nat->vm->GetEnv( (void **)&e, nat->env_version);
   
   if(status == NFCSTATUS_DESELECTED)
   {
      LOG_CALLBACK("trustednfc_jni_Discovery_notification_callback: Target deselected", status); 
         
      /* Notify manager that a target was deselected */
      e->CallVoidMethod(nat->manager, cached_NfcManager_notifyTargetDeselected);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
      } 
   }
   else
   {
      LOG_CALLBACK("trustednfc_jni_Discovery_notification_callback", status);
      LOGI("Discovered %d tags", uNofRemoteDev);
      
      if((psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
          || (psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Target))
      {
         tag_cls = e->GetObjectClass(nat->cached_P2pDevice);
         if(e->ExceptionCheck())
         {
            LOGE("Get Object Class Error"); 
            kill_client(nat);
            return;
         } 
         
         /* New target instance */
         ctor = e->GetMethodID(tag_cls, "<init>", "()V");
         tag = e->NewObject(tag_cls, ctor);
         
         /* Set P2P Target mode */
         f = e->GetFieldID(tag_cls, "mMode", "I"); 
         
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            LOGD("Discovered P2P Initiator");
            e->SetIntField(tag, f, (jint)MODE_P2P_INITIATOR);
         }
         else
         {    
            LOGD("Discovered P2P Target");
            e->SetIntField(tag, f, (jint)MODE_P2P_TARGET);
         }
          
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            /* Set General Bytes */
            f = e->GetFieldID(tag_cls, "mGeneralBytes", "[B");
   
           LOGD("General Bytes length =");
           for(i=0;i<psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length;i++)
           {
               LOGD("%02x ", psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo[i]);          
           }
       
            generalBytes = e->NewByteArray(psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length);   
             
            e->SetByteArrayRegion(generalBytes, 0, 
                                  psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length, 
                                  (jbyte *)psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo);
             
            e->SetObjectField(tag, f, generalBytes);        
        } 
      }
      else
      {
         tag_cls = e->GetObjectClass(nat->cached_NfcTag);
         if(e->ExceptionCheck())
         {
            kill_client(nat);
            return;
         }
      
         /* New tag instance */
         ctor = e->GetMethodID(tag_cls, "<init>", "()V");
         tag = e->NewObject(tag_cls, ctor);
         
         /* Set tag UID */
         f = e->GetFieldID(tag_cls, "mUid", "[B");
         tagUid = e->NewByteArray(psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.UidLength);
         e->SetByteArrayRegion(tagUid, 0, psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.UidLength,(jbyte *)psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Uid);      
         e->SetObjectField(tag, f, tagUid); 
      
         /* Set tag type */
         typeName = get_target_type_name( psRemoteDevList->psRemoteDevInfo->RemDevType,
                                          psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak);
         LOGD("Discovered tag: type=0x%08x[%s]", psRemoteDevList->psRemoteDevInfo->RemDevType, typeName);
         f = e->GetFieldID(tag_cls, "mType", "Ljava/lang/String;");
         e->SetObjectField(tag, f, e->NewStringUTF(typeName));
      }
      
      /* Set tag handle */
      f = e->GetFieldID(tag_cls, "mHandle", "I");
      e->SetIntField(tag, f,(jint)psRemoteDevList->hTargetDev);
      LOGD("Target handle = 0x%08x",psRemoteDevList->hTargetDev);
      
      nat->tag = e->NewGlobalRef(tag);
   
   
      /* Notify the service */   
      LOGD("Notify Nfc Service");
      if((psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
          || (psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Target))
      {
         /* Store the hanlde of the P2P device */
         hLlcpHandle = psRemoteDevList->hTargetDev;
         
         /* Notify manager that new a P2P device was found */
         e->CallVoidMethod(nat->manager, cached_NfcManager_notifyLlcpLinkActivation, tag);
         if(e->ExceptionCheck())
         {
            LOGE("Exception occured");
            kill_client(nat);
         }     
      }
      else
      {
         /* Notify manager that new a tag was found */
         e->CallVoidMethod(nat->manager, cached_NfcManager_notifyNdefMessageListeners, tag);
         if(e->ExceptionCheck())
         {
            LOGE("Exception occured");
            kill_client(nat);
         }     
      }
      e->DeleteLocalRef(tag);
   } 
}

static void trustednfc_jni_open_notification_callback(void *pContext,
   phLibNfc_RemoteDevList_t *psRemoteDevList,
   uint8_t uNofRemoteDev, NFCSTATUS status)
{
   JNIEnv *e;
   jclass tag_cls = NULL;
   jobject tag;
   jmethodID ctor;
   jfieldID f;
   jbyteArray tagUid;
   jstring type = NULL;
   jbyteArray generalBytes = NULL;
   const char * typeName;
   struct trustednfc_jni_native_data *nat;
   NFCSTATUS ret;
   int i;
   
   nat = (struct trustednfc_jni_native_data *)pContext;
   
   nat->vm->GetEnv( (void **)&e, nat->env_version);
   
   if(status == NFCSTATUS_DESELECTED)
   {
      LOG_CALLBACK("trustednfc_jni_open_notification_callback: Target deselected", status); 
         
      /* Notify manager that a target was deselected */
      e->CallVoidMethod(nat->manager, cached_NfcManager_notifyTargetDeselected);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
      } 
   }
   else
   {   
      LOG_CALLBACK("trustednfc_jni_open_notification_callback", status);
      LOGI("Discovered %d tags", uNofRemoteDev);
      
      
      if((psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
          || (psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Target))
      {
         LOGD("P2P Device detected\n");
         
         tag_cls = e->GetObjectClass(nat->cached_P2pDevice);
         if(e->ExceptionCheck())
         {
            kill_client(nat);
            return;
         } 
         
         /* New target instance */
         ctor = e->GetMethodID(tag_cls, "<init>", "()V");
         tag = e->NewObject(tag_cls, ctor);
      
         /* Set P2P Target mode */
         f = e->GetFieldID(tag_cls, "mMode", "I"); 
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            LOGD("P2P Initiator\n");  
            e->SetIntField(tag, f, (jint)MODE_P2P_INITIATOR);  
         }
         else
         {    
            LOGD("P2P Target\n");
            e->SetIntField(tag, f, (jint)MODE_P2P_TARGET);     
         }
          
         if(psRemoteDevList->psRemoteDevInfo->RemDevType == phNfc_eNfcIP1_Initiator)
         {
            /* Set General Bytes */
            f = e->GetFieldID(tag_cls, "mGeneralBytes", "[B");

            LOGD("General Bytes length =");
            for(i=0;i<psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length;i++)
            {
               LOGD("0x%02x ", psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo[i]);          
            }
    
            generalBytes = e->NewByteArray(psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length);   
             
            e->SetByteArrayRegion(generalBytes, 0, 
                                  psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length, 
                                  (jbyte *)psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo);
             
            e->SetObjectField(tag, f, generalBytes);        
         }   
      }
      else
      {
         LOGD("Tag detected\n");
         tag_cls = e->GetObjectClass(nat->cached_NfcTag);
         if(e->ExceptionCheck())
         {
            kill_client(nat);
            return;
         } 
         
         /* New tag instance */
         ctor = e->GetMethodID(tag_cls, "<init>", "()V");
         tag = e->NewObject(tag_cls, ctor);
         
         /* Set tag UID */
         LOGD("Set Tag UID\n");
         f = e->GetFieldID(tag_cls, "mUid", "[B");
         tagUid = e->NewByteArray(psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.UidLength);
         e->SetByteArrayRegion(tagUid, 0, psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.UidLength,(jbyte *)psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Uid);      
         e->SetObjectField(tag, f, tagUid); 
               
         /* Set tag type */
         typeName = get_target_type_name( psRemoteDevList->psRemoteDevInfo->RemDevType,
                                          psRemoteDevList->psRemoteDevInfo->RemoteDevInfo.Iso14443A_Info.Sak);
         LOGD("Discovered tag: type=0x%08x[%s]", psRemoteDevList->psRemoteDevInfo->RemDevType, typeName);
         f = e->GetFieldID(tag_cls, "mType", "Ljava/lang/String;");
         e->SetObjectField(tag, f, e->NewStringUTF(typeName));
      }
       
      /* Set tag handle */
      LOGD("Tag Handle: 0x%08x",psRemoteDevList->hTargetDev);
      f = e->GetFieldID(tag_cls, "mHandle", "I");
      e->SetIntField(tag, f,(jint)psRemoteDevList->hTargetDev);
      
      nat->tag = e->NewGlobalRef(tag); 
      
      e->DeleteLocalRef(tag);
      
      sem_post(&trustednfc_jni_open_sem);
   }
}

static void trustednfc_jni_init_callback(void *pContext, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_init_callback", status);

   struct trustednfc_jni_native_data *nat;

   nat = (struct trustednfc_jni_native_data *)pContext;

   nat->status = status;

   sem_post(&trustednfc_jni_init_sem);
}

static void trustednfc_jni_deinit_callback(void *pContext, NFCSTATUS status)
{
   struct trustednfc_jni_native_data *nat =
      (struct trustednfc_jni_native_data *)pContext;

   LOG_CALLBACK("trustednfc_jni_deinit_callback", status);

   nat->status = status;
   kill_client(nat);

   sem_post(&trustednfc_jni_manager_sem);
}

/* Set Secure Element mode callback*/
static void trustednfc_jni_smartMX_setModeCb (void*            pContext,
							                                phLibNfc_Handle  hSecureElement,
                                              NFCSTATUS        status)
{

  struct trustednfc_jni_native_data *nat =
    (struct trustednfc_jni_native_data *)pContext;
      
	if(status==NFCSTATUS_SUCCESS)
	{
		LOGD("SE Set Mode is Successful");
		LOGD("SE Handle: %lu", hSecureElement);		
	}
	else
	{
    LOGD("SE Set Mode is failed\n ");
  }
	
	nat->status = status;
	sem_post(&trustednfc_jni_open_sem);
}

/* Card Emulation callback */
static void trustednfc_jni_transaction_callback(void *context,
   phLibNfc_eSE_EvtType_t evt_type, phLibNfc_Handle handle,
   phLibNfc_uSeEvtInfo_t *evt_info, NFCSTATUS status)
{
   JNIEnv *e;
   jobject aid_array;
   struct trustednfc_jni_native_data *nat;
   phNfc_sData_t *aid;

   LOG_CALLBACK("trustednfc_jni_transaction_callback", status);

   nat = (struct trustednfc_jni_native_data *)context;

   nat->vm->GetEnv( (void **)&e, nat->env_version);

   aid = &(evt_info->UiccEvtInfo.aid);

   aid_array = NULL;

   if(aid != NULL)
   {
      aid_array = e->NewByteArray(aid->length);
      if(e->ExceptionCheck())
      {
         LOGE("Exception occured");
         kill_client(nat);
         return;
      }

      e->SetByteArrayRegion((jbyteArray)aid_array, 0, aid->length, (jbyte *)aid->buffer);
   }
   
   LOGD("Notify Nfc Service\n");
   /* Notify manager that a new event occurred on a SE */
   e->CallVoidMethod(nat->manager,
      cached_NfcManager_notifyTransactionListeners, aid_array);
      
   if(e->ExceptionCheck())
   {
      LOGE("Notification Exception occured");
      kill_client(nat);
   }

   e->DeleteLocalRef(aid_array);
}

static void trustednfc_jni_se_set_mode_callback(void *context,
   phLibNfc_Handle handle, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_se_set_mode_callback", status);

   sem_post(&trustednfc_jni_manager_sem);
}

/*
 * NFCManager methods
 */
 
 /* Discovery Method */
static void trustednfc_jni_start_tag_discovery(struct trustednfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   struct timespec ts;
   
   /* Discovery */
#if 0
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693  = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation = FALSE;
   nat->discovery_cfg.Duration = 0x1388;
#endif

   /* Registery */   
   nat->registry_info.MifareUL = TRUE;
   nat->registry_info.MifareStd = TRUE;
   nat->registry_info.ISO14443_4A = TRUE;
   nat->registry_info.ISO14443_4B = TRUE;
   nat->registry_info.Jewel = TRUE;
   nat->registry_info.Felica = TRUE;  
   nat->registry_info.NFC = FALSE;   

   LOGD("******  NFC Config Mode TAG Reader ******"); 
   
   /* Register for the reader mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_RemoteDev_NtfRegister(&nat->registry_info, trustednfc_jni_open_notification_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_RemoteDev_NtfRegister(%s-%s-%s-%s-%s-%s-%s-%s) returned 0x%x\n",
      nat->registry_info.Jewel==TRUE?"J":"",
      nat->registry_info.MifareUL==TRUE?"UL":"",
      nat->registry_info.MifareStd==TRUE?"Mi":"",
      nat->registry_info.Felica==TRUE?"F":"",
      nat->registry_info.ISO14443_4A==TRUE?"4A":"",
      nat->registry_info.ISO14443_4B==TRUE?"4B":"",
      nat->registry_info.NFC==TRUE?"P2P":"",
      nat->registry_info.ISO15693==TRUE?"R":"", ret);
   if(ret != NFCSTATUS_SUCCESS)
      return; 

   /* Start Polling loop */
#if 0
   LOGD("******  Start NFC Discovery ******");
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,nat->discovery_cfg, trustednfc_jni_discover_callback, (void *)nat);
   LOGD("phLibNfc_Mgt_ConfigureDiscovery returned 0x%x\n", ret);
#endif

}

static void trustednfc_jni_start_p2p_discovery(struct trustednfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   
   LOGD("******  NFC Config Mode P2P Reader ******"); 
   
   /* Clear previous configuration */
   //memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   //memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));
   
   /* Discovery */
#if 0
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693 = FALSE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = TRUE;
   nat->discovery_cfg.NfcIP_Mode = phNfc_ePassive212;
   nat->discovery_cfg.Duration = 0x1388;
#endif
 
   /* Registery */
   nat->registry_info.MifareUL      = FALSE;
   nat->registry_info.MifareStd     = FALSE;
   nat->registry_info.ISO14443_4A   = FALSE;
   nat->registry_info.ISO14443_4B   = FALSE;
   nat->registry_info.Jewel         = FALSE;
   nat->registry_info.Felica        = FALSE;  
   nat->registry_info.NFC           = TRUE;


   /*REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_SetP2P_ConfigParams(&trustednfc_jni_nfcip1_cfg,trustednfc_jni_p2pcfg_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_Mgt_SetP2P_ConfigParams returned 0x%x\n", ret);
   if(ret != NFCSTATUS_PENDING)
      return;*/

   /* Wait for callback response */
   //sem_wait(&trustednfc_jni_manager_sem);

   /* Register for the p2p mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_RemoteDev_NtfRegister(&nat->registry_info, trustednfc_jni_open_notification_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_RemoteDev_NtfRegister(%s-%s-%s-%s-%s-%s-%s-%s) returned 0x%x\n",
      nat->registry_info.Jewel==TRUE?"J":"",
      nat->registry_info.MifareUL==TRUE?"UL":"",
      nat->registry_info.MifareStd==TRUE?"Mi":"",
      nat->registry_info.Felica==TRUE?"F":"",
      nat->registry_info.ISO14443_4A==TRUE?"4A":"",
      nat->registry_info.ISO14443_4B==TRUE?"4B":"",
      nat->registry_info.NFC==TRUE?"P2P":"",
      nat->registry_info.ISO15693==TRUE?"R":"", ret);
   if(ret != NFCSTATUS_SUCCESS)
      return;
      
   /* Start Polling loop */
#if 0
   LOGD("******  Start NFC Discovery ******");
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,nat->discovery_cfg, trustednfc_jni_discover_callback, (void *)nat);
   LOGD("phLibNfc_Mgt_ConfigureDiscovery returned 0x%x\n", ret);
#endif
}


static void trustednfc_jni_start_card_emu_discovery(struct trustednfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   
   LOGD("******  NFC Config Mode Card Emulation ******");   

   /* Register for the card emulation mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_NtfRegister(trustednfc_jni_transaction_callback,(void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_SE_NtfRegister returned 0x%x\n", ret);
   if(ret != NFCSTATUS_SUCCESS)
       return;
}


static void trustednfc_jni_start_discovery(struct trustednfc_jni_native_data *nat)
{
   NFCSTATUS ret;
   phLibNfc_Llcp_sLinkParameters_t LlcpConfigInfo;

   /* Clear previous configuration */
   //memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   //memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));

#if 0
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693  = TRUE;
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = FALSE;
#endif
   
   nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation = FALSE;
   nat->discovery_cfg.NfcIP_Mode = phNfc_ePassive212;//phNfc_eP2P_ALL;
   nat->discovery_cfg.Duration = 300000; /* in ms */


   nat->registry_info.MifareUL = TRUE;
   nat->registry_info.MifareStd = TRUE;
   nat->registry_info.ISO14443_4A = TRUE;
   nat->registry_info.ISO14443_4B = TRUE;
   nat->registry_info.Jewel = TRUE;
   nat->registry_info.Felica = TRUE;
   nat->registry_info.NFC = TRUE;   
   LOGD("******  NFC Config Mode Reader ******");
      
   /* LLCP Params */
   LOGD("******  NFC Config Mode NFCIP1 - LLCP ******"); 
   LlcpConfigInfo.miu    = nat->miu;
   LlcpConfigInfo.lto    = nat->lto;
   LlcpConfigInfo.wks    = nat->wks;
   LlcpConfigInfo.option = nat->opt;
    
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_SetLlcp_ConfigParams(&LlcpConfigInfo,
                                           trustednfc_jni_llcpcfg_callback,
                                           (void *)nat);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_PENDING)
      return;

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_manager_sem);
   
   /* Register for the reader mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_RemoteDev_NtfRegister(&nat->registry_info, trustednfc_jni_Discovery_notification_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_RemoteDev_NtfRegister(%s-%s-%s-%s-%s-%s-%s-%s) returned 0x%x\n",
      nat->registry_info.Jewel==TRUE?"J":"",
      nat->registry_info.MifareUL==TRUE?"UL":"",
      nat->registry_info.MifareStd==TRUE?"Mi":"",
      nat->registry_info.Felica==TRUE?"F":"",
      nat->registry_info.ISO14443_4A==TRUE?"4A":"",
      nat->registry_info.ISO14443_4B==TRUE?"4B":"",
      nat->registry_info.NFC==TRUE?"P2P":"",
      nat->registry_info.ISO15693==TRUE?"R":"", ret);
   if(ret != NFCSTATUS_SUCCESS)
      return; 
   
   /* Register for the card emulation mode */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_NtfRegister(trustednfc_jni_transaction_callback,(void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_SE_NtfRegister returned 0x%x\n", ret);
   if(ret != NFCSTATUS_SUCCESS)
       return;
   
   /* Start Polling loop */
   LOGD("******  Start NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,nat->discovery_cfg, trustednfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      nat->discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      nat->discovery_cfg.NfcIP_Mode, nat->discovery_cfg.Duration, ret);
} 

static void trustednfc_jni_stop_discovery(struct trustednfc_jni_native_data *nat)
{
   phLibNfc_sADD_Cfg_t discovery_cfg;
   NFCSTATUS ret;

   discovery_cfg.PollDevInfo.PollEnabled = 0;
   discovery_cfg.Duration = 0xffffffff;
   /*discovery_cfg.NfcIP_Mode = phNfc_eInvalidP2PMode;*/
   discovery_cfg.NfcIP_Mode = phNfc_eDefaultP2PMode;
   discovery_cfg.NfcIP_Tgt_Disable = TRUE;
 
   /* Start Polling loop */
   LOGD("******  Stop NFC Discovery ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_ConfigureDiscovery(NFC_DISCOVERY_CONFIG,discovery_cfg, trustednfc_jni_discover_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_Mgt_ConfigureDiscovery(%s-%s-%s-%s-%s-%s, %s-%x-%x) returned 0x%08x\n",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A==TRUE?"3A":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B==TRUE?"3B":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212==TRUE?"F2":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424==TRUE?"F4":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive==TRUE?"NFC":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693==TRUE?"RFID":"",
      discovery_cfg.PollDevInfo.PollCfgInfo.DisableCardEmulation==FALSE?"CE":"",
      discovery_cfg.NfcIP_Mode, discovery_cfg.Duration, ret);
} 

static void trustednfc_jni_reader_discovery(struct trustednfc_jni_native_data *nat)
{
   static unsigned char ioctl[2] = {03,00};
   static unsigned char resp[16];
   NFCSTATUS ret;

   gInputParam.length = 2;
   gInputParam.buffer = ioctl;
   gOutputParam.length = 16;
   gOutputParam.buffer = resp;

   LOGD("******  Start PRBS Test ******");
   REENTRANCE_LOCK();
   ret = phLibNfc_Mgt_IoCtl(gHWRef, DEVMGMT_PRBS_TEST, &gInputParam, &gOutputParam, trustednfc_jni_ioctl_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   LOGD("phLibNfc_Mgt_IoCtl(PRBS Test) returned 0x%08x\n", ret);
} 

static void com_trustedlogic_trustednfc_android_internal_NfcManager_readerDiscovery(JNIEnv *e, jobject o)
{
    struct trustednfc_jni_native_data *nat;

    CONCURRENCY_LOCK();

    /* Retrieve native structure address */
    nat = trustednfc_jni_get_nat(e, o);
   
    trustednfc_jni_reader_discovery(nat);

    CONCURRENCY_UNLOCK();
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_disableDiscovery(JNIEnv *e, jobject o)
{
    struct trustednfc_jni_native_data *nat;

    CONCURRENCY_LOCK();

    /* Retrieve native structure address */
    nat = trustednfc_jni_get_nat(e, o);
   
    trustednfc_jni_stop_discovery(nat);

    CONCURRENCY_UNLOCK();
}
    
static void com_trustedlogic_trustednfc_android_internal_NfcManager_enableDiscovery(
   JNIEnv *e, jobject o, jint mode)
{
   NFCSTATUS ret;
   struct trustednfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 
   
   if(mode == DISCOVERY_MODE_TAG_READER)
   {
      trustednfc_jni_start_discovery(nat);  
   }
   else if(DISCOVERY_MODE_CARD_EMULATION)
   {
      trustednfc_jni_start_card_emu_discovery(nat);    
   }

   CONCURRENCY_UNLOCK();
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_disableDiscoveryMode(
   JNIEnv *e, jobject o, jint mode)
{
   struct trustednfc_jni_native_data *nat;

   if((mode < 0) || (mode >= DISCOVERY_MODE_TABLE_SIZE))
      return;

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 

   nat->discovery_modes_state[mode] = DISCOVERY_MODE_DISABLED;
   
}

static jboolean com_trustedlogic_trustednfc_android_internal_NfcManager_init_native_struc(JNIEnv *e, jobject o)
{
   NFCSTATUS status;
   struct trustednfc_jni_native_data *nat = NULL;
   jclass cls;
   jobject obj;
   jfieldID f;

   LOGD("******  Init Native Structure ******"); 

   /* Initialize native structure */
   nat = (trustednfc_jni_native_data*)malloc(sizeof(struct trustednfc_jni_native_data));
   if(nat == NULL)
   {
      LOGD("Native Structure initialization failed 0x%08x[%s]", nat->status, trustednfc_jni_get_status_name(nat->status));
      if(nat)
         kill_client(nat);
      return FALSE;   
   }
   
   e->GetJavaVM(&(nat->vm));
   nat->env_version = e->GetVersion();
   nat->manager = e->NewGlobalRef(o);
      
   cls = e->GetObjectClass(o);
   f = e->GetFieldID(cls, "mNative", "I");
   e->SetIntField(o, f, (jint)nat);
                 
   /* Initialize native cached references */
   cached_NfcManager_notifyNdefMessageListeners = e->GetMethodID(cls,
      "notifyNdefMessageListeners","(Lcom/trustedlogic/trustednfc/android/internal/NativeNfcTag;)V");

   cached_NfcManager_notifyTransactionListeners = e->GetMethodID(cls,
      "notifyTransactionListeners", "([B)V");
         
   cached_NfcManager_notifyLlcpLinkActivation = e->GetMethodID(cls,
      "notifyLlcpLinkActivation","(Lcom/trustedlogic/trustednfc/android/internal/NativeP2pDevice;)V"); 
         
   cached_NfcManager_notifyLlcpLinkDeactivated = e->GetMethodID(cls,
      "notifyLlcpLinkDeactivated","()V"); 
      
   cached_NfcManager_notifyTargetDeselected = e->GetMethodID(cls,
      "notifyTargetDeselected","()V"); 
      
      
   if(trustednfc_jni_cache_object(e,"com/trustedlogic/trustednfc/android/internal/NativeNfcTag",&(nat->cached_NfcTag)) == -1)
   {
      LOGD("Native Structure initialization failed [0x%08x]",nat->status);
      return FALSE;   
   }
         
   if(trustednfc_jni_cache_object(e,"com/trustedlogic/trustednfc/android/internal/NativeP2pDevice",&(nat->cached_P2pDevice)) == -1)
   {
      LOGD("Native Structure initialization failed [0x%08x]",nat->status);
      return FALSE;   
   }

   LOGD("****** Init Native Structure OK ******"); 
   return TRUE;
}
 
/* Init/Deinit method */
static jboolean com_trustedlogic_trustednfc_android_internal_NfcManager_initialize(JNIEnv *e, jobject o)
{
   struct trustednfc_jni_native_data *nat = NULL;
   int init_result = JNI_FALSE;
#ifdef TNFC_EMULATOR_ONLY
   char value[PROPERTY_VALUE_MAX];
#endif
   jboolean result;
   
   CONCURRENCY_LOCK();

#ifdef TNFC_EMULATOR_ONLY
   if (!property_get("ro.kernel.qemu", value, 0))
   {
      LOGE("NFC Initialization failed: not running in an emulator\n");
      goto clean_and_return;
   }
#endif

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 
   exported_nat = nat;

   /* Perform the initialization */
   init_result = trustednfc_jni_initialize(nat);

clean_and_return:
   CONCURRENCY_UNLOCK();

   /* Convert the result and return */
   return (init_result==TRUE)?JNI_TRUE:JNI_FALSE;
}

static jboolean com_trustedlogic_trustednfc_android_internal_NfcManager_deinitialize(JNIEnv *e, jobject o)
{
   struct timespec ts;
   NFCSTATUS status;
   struct trustednfc_jni_native_data *nat;
   int bStackReset = FALSE;

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 

   /* Clear previous configuration */
   memset(&nat->discovery_cfg, 0, sizeof(phLibNfc_sADD_Cfg_t));
   memset(&nat->registry_info, 0, sizeof(phLibNfc_Registry_Info_t));
   
   LOGD("phLibNfc_Mgt_DeInitialize()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_DeInitialize(gHWRef, trustednfc_jni_deinit_callback, (void *)nat);
   REENTRANCE_UNLOCK();
   if (status == NFCSTATUS_PENDING)
   {
      LOGD("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += 10; 
  
      /* Wait for callback response */
      if(sem_timedwait(&trustednfc_jni_manager_sem, &ts) == -1)
      {
         LOGW("Operation timed out");
         bStackReset = TRUE;
      }
   }
   else
   {
      LOGW("phLibNfc_Mgt_DeInitialize() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      bStackReset = TRUE;
   }

   if(bStackReset == TRUE)
   {
      /* Complete deinit. failed, try minimal reset (clean internal structures and free memory) */
      LOGW("Reseting stack...");
      REENTRANCE_LOCK();
      status = phLibNfc_Mgt_DeInitialize(gHWRef, NULL, NULL);
      REENTRANCE_UNLOCK();
      if (status != NFCSTATUS_SUCCESS)
      {
         /* NOTE: by design, this could not happen */
         LOGE("Reset failed [0x%08x]", status);
      }
      /* Force result to success (deinit shall not fail!) */
      nat->status = NFCSTATUS_SUCCESS;
   }

   /* Unconfigure driver */
   LOGD("phLibNfc_Mgt_UnConfigureDriver()");
   REENTRANCE_LOCK();
   status = phLibNfc_Mgt_UnConfigureDriver(gHWRef);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }
   else
   {
      LOGD("phLibNfc_Mgt_UnConfigureDriver() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
   }

   LOGI("NFC Deinitialized");

   return TRUE;
}

/* Secure Element methods */
static jintArray com_trustedlogic_trustednfc_android_internal_NfcManager_doGetSecureElementList(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   jintArray list= NULL;
   phLibNfc_SE_List_t se_list[PHLIBNFC_MAXNO_OF_SE];
   uint8_t i, se_count = PHLIBNFC_MAXNO_OF_SE;
    
   LOGD("******  Get Secure Element List ******");  
   
   LOGD("phLibNfc_SE_GetSecureElementList()");
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_GetSecureElementList(se_list, &se_count);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_SE_GetSecureElementList() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      return list;  
   }
   LOGD("phLibNfc_SE_GetSecureElementList() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));

   LOGD("Nb SE: %d", se_count);
   list =e->NewIntArray(se_count);
   for(i=0;i<se_count;i++)
   {
      if (se_list[i].eSE_Type == phLibNfc_SE_Type_SmartMX)
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): SMX detected"); 
        LOGD("SE ID #%d: 0x%08x", i, se_list[i].hSecureElement);
      }
      else if(se_list[i].eSE_Type == phLibNfc_SE_Type_UICC)
      {
        LOGD("phLibNfc_SE_GetSecureElementList(): UICC detected");
        LOGD("SE ID #%d: 0x%08x", i, se_list[i].hSecureElement); 
      }
      
      e->SetIntArrayRegion(list, i, 1, (jint*)&se_list[i].hSecureElement);
   }

   e->DeleteLocalRef(list);
  
   return list;
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_doSelectSecureElement(JNIEnv *e, jobject o, jint seID)
{
   NFCSTATUS ret;
   struct trustednfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   nat->seId = seID;

   LOGD("******  Select Secure Element ******"); 

   LOGD("phLibNfc_SE_SetMode(0x%08x, ...)", seID);
   /* Set SE mode - Virtual */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_SetMode(seID,phLibNfc_SE_ActModeVirtual, trustednfc_jni_se_set_mode_callback,(void *)nat);    
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_PENDING)
   {
      LOGD("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   LOGD("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_manager_sem);

clean_and_return:
   CONCURRENCY_UNLOCK();
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_doDeselectSecureElement(JNIEnv *e, jobject o, jint seID)
{
   NFCSTATUS ret;
   struct trustednfc_jni_native_data *nat;

   CONCURRENCY_LOCK();
   
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   nat->seId = 0;

   LOGD("******  Deselect Secure Element ******"); 

   LOGD("phLibNfc_SE_SetMode(0x%02x, ...)", seID);
   /* Set SE mode - Off */
   REENTRANCE_LOCK();
   ret = phLibNfc_SE_SetMode(seID,phLibNfc_SE_ActModeOff, trustednfc_jni_se_set_mode_callback,(void *)nat);
   REENTRANCE_UNLOCK();
       
   LOGD("phLibNfc_SE_SetMode for SE 0x%02x returned 0x%02x",seID, ret);
   if(ret != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   LOGD("phLibNfc_SE_SetMode() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_manager_sem);

clean_and_return:
   CONCURRENCY_UNLOCK();
}

/* Open Tag/P2p Methods */
static jobject com_trustedlogic_trustednfc_android_internal_NfcManager_doOpenP2pConnection(JNIEnv *e, jobject o, jint timeout)
{
   NFCSTATUS ret;
   struct timespec ts;
   struct trustednfc_jni_native_data *nat;
   jobject p2pDevice = NULL;
   int semResult;

   CONCURRENCY_LOCK();
   
   LOGD("Open P2p");

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   
   trustednfc_jni_start_p2p_discovery(nat);
  
   /* Timeout */
   if(timeout != 0)
   {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout; 
      semResult = sem_timedwait(&trustednfc_jni_open_sem, &ts);
   }
   else
   {  
      semResult = sem_wait(&trustednfc_jni_open_sem);
   }

   if (semResult)
   {
      LOGW("P2P opening aborted");
      goto clean_and_return;
   }

   if(nat->status != NFCSTATUS_SUCCESS)
   {
      LOGE("P2P opening failed");
      goto clean_and_return;
   }
      
   p2pDevice = nat->tag;

clean_and_return:

   CONCURRENCY_UNLOCK();

   return p2pDevice;
}

static jobject com_trustedlogic_trustednfc_android_internal_NfcManager_doOpenTagConnection(JNIEnv *e, jobject o, jint timeout)
{
   NFCSTATUS ret;
   struct timespec ts;
   jobject nfcTag = NULL;
   struct trustednfc_jni_native_data *nat;
   int semResult;

   CONCURRENCY_LOCK();

   LOGD("Open Tag");

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   
   trustednfc_jni_start_tag_discovery(nat);

   /* Timeout */
   if(timeout != 0)
   {
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += timeout; 
      semResult = sem_timedwait(&trustednfc_jni_open_sem, &ts);
   }
   else
   {  
      semResult = sem_wait(&trustednfc_jni_open_sem);
   }

   if (semResult)
   {
      LOGW("P2P opening aborted");
      goto clean_and_return;
   }

   if(nat->status != NFCSTATUS_SUCCESS)
   {
      LOGE("P2P opening failed");
      goto clean_and_return;
   }

   nfcTag = nat->tag;   
   
clean_and_return:

   CONCURRENCY_UNLOCK();

   return nfcTag;
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_doCancel(JNIEnv *e, jobject o)
{
   struct trustednfc_jni_native_data *nat;

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
  
   nat->status = NFCSTATUS_FAILED;
   sem_post(&trustednfc_jni_open_sem);
}

/* Llcp methods */

static jboolean com_trustedlogic_trustednfc_android_internal_NfcManager_doCheckLlcp(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   jboolean result = JNI_FALSE;
   struct trustednfc_jni_native_data *nat;
   
   CONCURRENCY_LOCK();

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   
   /* Check LLCP compliancy */
   LOGD("phLibNfc_Llcp_CheckLlcp(hLlcpHandle=0x%08x)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_CheckLlcp(hLlcpHandle,
                                 trustednfc_jni_checkLlcp_callback,
                                 trustednfc_jni_llcp_linkStatus_callback,
                                 (void*)nat);
   REENTRANCE_UNLOCK();
   /* In case of a NFCIP return NFCSTATUS_SUCCESS and in case of an another protocol NFCSTATUS_PENDING */
   if(ret != NFCSTATUS_PENDING && ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_CheckLlcp() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      goto clean_and_return;
   }
   LOGD("phLibNfc_Llcp_CheckLlcp() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
                                    
   /* Wait for callback response */
   sem_wait(&trustednfc_jni_llcp_sem);

   if(trustednfc_jni_cb_status == NFCSTATUS_SUCCESS)
   {
      result = JNI_TRUE;
   }

clean_and_return:
   CONCURRENCY_UNLOCK();
   return result;
}

static jboolean com_trustedlogic_trustednfc_android_internal_NfcManager_doActivateLlcp(JNIEnv *e, jobject o)
{
   NFCSTATUS ret;
   LOGD("phLibNfc_Llcp_Activate(hRemoteDevice=0x%08x)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Activate(hLlcpHandle);
   REENTRANCE_UNLOCK();
   if(ret == NFCSTATUS_SUCCESS)
   {
      LOGD("phLibNfc_Llcp_Activate() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      return JNI_TRUE;
   }
   else
   {
      LOGE("phLibNfc_Llcp_Activate() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      return JNI_FALSE;   
   }
}



static jobject com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpConnectionlessSocket(JNIEnv *e, jobject o, jint nSap)
{
   NFCSTATUS ret;
   jobject connectionlessSocket = NULL;
   phLibNfc_Handle hLlcpSocket;
   struct trustednfc_jni_native_data *nat;
   jclass clsNativeConnectionlessSocket;
   jfieldID f;
   
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 
   
   /* Create socket */
   LOGD("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionLess, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionLess,
                              NULL,
                              NULL,
                              &hLlcpSocket,
                              trustednfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();
 
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      return NULL;
   }
   LOGD("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
   
   
   /* Bind socket */
   LOGD("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      /* Close socket created */
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      REENTRANCE_UNLOCK();
      return NULL;
   }
   LOGD("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
 
   
   /* Create new NativeLlcpConnectionlessSocket object */
   if(trustednfc_jni_cache_object(e,"com/trustedlogic/trustednfc/android/internal/NativeLlcpConnectionlessSocket",&(connectionlessSocket)) == -1)
   {
      return NULL;           
   } 
   
   /* Get NativeConnectionless class object */
   clsNativeConnectionlessSocket = e->GetObjectClass(connectionlessSocket);
   if(e->ExceptionCheck())
   {
      return NULL;  
   }
   
   /* Set socket handle */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mHandle", "I");
   e->SetIntField(connectionlessSocket, f,(jint)hLlcpSocket);
   LOGD("Connectionless socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set the miu link of the connectionless socket */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mLinkMiu", "I");
   e->SetIntField(connectionlessSocket, f,(jint)PHFRINFC_LLCP_MIU_DEFAULT);
   LOGD("Connectionless socket Link MIU = %d\n",PHFRINFC_LLCP_MIU_DEFAULT);  
   
   /* Set socket SAP */
   f = e->GetFieldID(clsNativeConnectionlessSocket, "mSap", "I");
   e->SetIntField(connectionlessSocket, f,(jint)nSap);
   LOGD("Connectionless socket SAP = %d\n",nSap);  
   
   return connectionlessSocket;
}

static jobject com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpServiceSocket(JNIEnv *e, jobject o, jint nSap, jstring sn, jint miu, jint rw, jint linearBufferLength)
{
   NFCSTATUS ret;
   phLibNfc_Handle hLlcpSocket;
   phLibNfc_Llcp_sSocketOptions_t sOptions;
   phNfc_sData_t sWorkingBuffer;
   phNfc_sData_t serviceName;
   struct trustednfc_jni_native_data *nat;
   jobject serviceSocket = NULL;
   jclass clsNativeLlcpServiceSocket;
   jfieldID f;  
  
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 
   
   /* Set Connection Oriented socket options */
   sOptions.miu = miu;
   sOptions.rw  = rw;  
  
   /* Allocate Working buffer length */
   sWorkingBuffer.length = (miu*rw)+ miu + linearBufferLength;
   sWorkingBuffer.buffer = (uint8_t*)malloc(sWorkingBuffer.length);

   
   /* Create socket */
   LOGD("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionOriented, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionOriented,
                              &sOptions,
                              &sWorkingBuffer,
                              &hLlcpSocket,
                              trustednfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();
                                                     
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      return NULL;
   }
   LOGD("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
   
   /* Bind socket */
   LOGD("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
   REENTRANCE_UNLOCK();
   if(ret != NFCSTATUS_SUCCESS)
   {
      lastErrorStatus = ret;
      LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      /* Close socket created */
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      return NULL;
   }
   LOGD("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
   
   /* Service socket */
   serviceName.buffer = (uint8_t*)e->GetStringUTFChars(sn, NULL);
   serviceName.length = (uint32_t)e->GetStringUTFLength(sn);
   
   LOGD("phLibNfc_Llcp_Listen(hSocket=0x%08x, ...)", hLlcpSocket);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Listen( hLlcpSocket,
                               &serviceName,
                               trustednfc_jni_llcp_transport_listen_socket_callback,
                               (void*)nat);
   REENTRANCE_UNLOCK();
                               
   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Listen() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      /* Close created socket */
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Close(hLlcpSocket); 
      REENTRANCE_UNLOCK();
      return NULL;
   }                         
   LOGD("phLibNfc_Llcp_Listen() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
   
   /* Create new NativeLlcpServiceSocket object */
   if(trustednfc_jni_cache_object(e,"com/trustedlogic/trustednfc/android/internal/NativeLlcpServiceSocket",&(serviceSocket)) == -1)
   {
      LOGE("Llcp Socket object creation error");
      return NULL;           
   } 
   
   /* Get NativeLlcpServiceSocket class object */
   clsNativeLlcpServiceSocket = e->GetObjectClass(serviceSocket);
   if(e->ExceptionCheck())
   {
      LOGE("Llcp Socket get object class error");
      return NULL;  
   } 
   
   /* Set socket handle */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mHandle", "I");
   e->SetIntField(serviceSocket, f,(jint)hLlcpSocket);
   LOGD("Service socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set socket linear buffer length */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalLinearBufferLength", "I");
   e->SetIntField(serviceSocket, f,(jint)linearBufferLength);
   LOGD("Service socket Linear buffer length = %02x\n",linearBufferLength);  
   
   /* Set socket MIU */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalMiu", "I");
   e->SetIntField(serviceSocket, f,(jint)miu);
   LOGD("Service socket MIU = %d\n",miu);  
   
   /* Set socket RW */
   f = e->GetFieldID(clsNativeLlcpServiceSocket, "mLocalRw", "I");
   e->SetIntField(serviceSocket, f,(jint)rw);
   LOGD("Service socket RW = %d\n",rw);   
  
   return serviceSocket;
}

static jobject com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpSocket(JNIEnv *e, jobject o, jint nSap, jint miu, jint rw, jint linearBufferLength)
{
   jobject clientSocket = NULL;
   NFCSTATUS ret;
   phLibNfc_Handle hLlcpSocket;
   phLibNfc_Llcp_sSocketOptions_t sOptions;
   phNfc_sData_t sWorkingBuffer;
   struct trustednfc_jni_native_data *nat;
   jclass clsNativeLlcpSocket;
   jfieldID f;
   
   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o); 
   
   /* Set Connection Oriented socket options */
   sOptions.miu = miu;
   sOptions.rw  = rw;
   
   /* Allocate Working buffer length */
   sWorkingBuffer.length = (miu*rw)+ miu + linearBufferLength;
   sWorkingBuffer.buffer = (uint8_t*)malloc(sWorkingBuffer.length);

   /* Create socket */
   LOGD("phLibNfc_Llcp_Socket(hRemoteDevice=0x%08x, eType=phFriNfc_LlcpTransport_eConnectionOriented, ...)", hLlcpHandle);
   REENTRANCE_LOCK();
   ret = phLibNfc_Llcp_Socket(hLlcpHandle,
                              phFriNfc_LlcpTransport_eConnectionOriented,
                              &sOptions,
                              &sWorkingBuffer,
                              &hLlcpSocket,
                              trustednfc_jni_llcp_transport_socket_err_callback,
                              (void*)nat);
   REENTRANCE_UNLOCK();

   if(ret != NFCSTATUS_SUCCESS)
   {
      LOGE("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      lastErrorStatus = ret;
      return NULL;
   }
   LOGD("phLibNfc_Llcp_Socket() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
   
   /* Create new NativeLlcpSocket object */
   if(trustednfc_jni_cache_object(e,"com/trustedlogic/trustednfc/android/internal/NativeLlcpSocket",&(clientSocket)) == -1)
   {
      LOGE("Llcp socket object creation error");  
      return NULL;           
   } 
   
   /* Get NativeConnectionless class object */
   clsNativeLlcpSocket = e->GetObjectClass(clientSocket);
   if(e->ExceptionCheck())
   {
      LOGE("Get class object error");    
      return NULL;  
   }
   
   /* Test if an SAP number is present */
   if(nSap != 0)
   {
      /* Bind socket */
      LOGD("phLibNfc_Llcp_Bind(hSocket=0x%08x, nSap=0x%02x)", hLlcpSocket, nSap);
      REENTRANCE_LOCK();
      ret = phLibNfc_Llcp_Bind(hLlcpSocket,nSap);
      REENTRANCE_UNLOCK();
      if(ret != NFCSTATUS_SUCCESS)
      {
         lastErrorStatus = ret;
         LOGE("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
         /* Close socket created */
         REENTRANCE_LOCK();
         ret = phLibNfc_Llcp_Close(hLlcpSocket); 
         REENTRANCE_UNLOCK();
         return NULL;
      }
      LOGD("phLibNfc_Llcp_Bind() returned 0x%04x[%s]", ret, trustednfc_jni_get_status_name(ret));
      
      /* Set socket SAP */
      f = e->GetFieldID(clsNativeLlcpSocket, "mSap", "I");
      e->SetIntField(clientSocket, f,(jint)nSap);
      LOGD("socket SAP = %d\n",nSap);    
   }  
      
   /* Set socket handle */
   f = e->GetFieldID(clsNativeLlcpSocket, "mHandle", "I");
   e->SetIntField(clientSocket, f,(jint)hLlcpSocket);
   LOGD("socket Handle = %02x\n",hLlcpSocket);  
   
   /* Set socket MIU */
   f = e->GetFieldID(clsNativeLlcpSocket, "mLocalMiu", "I");
   e->SetIntField(clientSocket, f,(jint)miu);
   LOGD("socket MIU = %d\n",miu);  
   
   /* Set socket RW */
   f = e->GetFieldID(clsNativeLlcpSocket, "mLocalRw", "I");
   e->SetIntField(clientSocket, f,(jint)rw);
   LOGD("socket RW = %d\n",rw);   
   
  
   return clientSocket;
}

static jint com_trustedlogic_trustednfc_android_internal_NfcManager_doGetLastError(JNIEnv *e, jobject o)
{
   LOGW("Last Error Status = 0x%02x",lastErrorStatus);
   
   if(lastErrorStatus == NFCSTATUS_BUFFER_TOO_SMALL)
   {
      return ERROR_BUFFER_TOO_SMALL;
   }
   else if(lastErrorStatus == NFCSTATUS_INSUFFICIENT_RESOURCES)
   {
      return  ERROR_INSUFFICIENT_RESOURCES;
   }
   else
   {
      return lastErrorStatus;
   }
}

static void com_trustedlogic_trustednfc_android_internal_NfcManager_doSetProperties(JNIEnv *e, jobject o, jint param, jint value)
{  
   NFCSTATUS ret;
   struct trustednfc_jni_native_data *nat;

   /* Retrieve native structure address */
   nat = trustednfc_jni_get_nat(e, o);
   
   switch(param)
   {
   case PROPERTY_LLCP_LTO:
      {
         LOGD("> Set LLCP LTO to %d",value); 
         nat->lto = value;
      }break;
      
   case PROPERTY_LLCP_MIU:
      {
         LOGD("> Set LLCP MIU to %d",value);  
         nat->miu = value;
      }break;
      
   case PROPERTY_LLCP_WKS:
      {
         LOGD("> Set LLCP WKS to %d",value); 
         nat->wks = value;
      }break;
      
   case PROPERTY_LLCP_OPT:
      {
         LOGD("> Set LLCP OPT to %d",value); 
         nat->opt = value;    
      }break;
      
   case PROPERTY_NFC_DISCOVERY_A:
      {
         LOGD("> Set NFC DISCOVERY A to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443A = value;  
      }break;
      
   case PROPERTY_NFC_DISCOVERY_B:
      {
         LOGD("> Set NFC DISCOVERY B to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso14443B = value;    
      }break;
      
   case PROPERTY_NFC_DISCOVERY_F:
      {
         LOGD("> Set NFC DISCOVERY F to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica212 = value;
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableFelica424 = value;
      }break;
      
   case PROPERTY_NFC_DISCOVERY_15693:
      {
         LOGD("> Set NFC DISCOVERY 15693 to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableIso15693 = value; 
      }break;
      
   case PROPERTY_NFC_DISCOVERY_NCFIP:
      {
         LOGD("> Set NFC DISCOVERY NFCIP to %d",value); 
         nat->discovery_cfg.PollDevInfo.PollCfgInfo.EnableNfcActive = value; 
      }break;
   default:
      {
         LOGW("> Unknown Property "); 
      }break;
   }
   

}
/*
 * JNI registration.
 */
static JNINativeMethod gMethods[] =
{
   {"initializeNativeStructure", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_init_native_struc},
      
   {"initialize", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_initialize},
 
   {"deinitialize", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_deinitialize},
      
   {"enableDiscovery", "(I)V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_enableDiscovery},
      
   {"disableDiscoveryMode", "(I)V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_disableDiscoveryMode},
      
   {"doGetSecureElementList", "()[I",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doGetSecureElementList},
      
   {"doSelectSecureElement", "(I)V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doSelectSecureElement},
      
   {"doDeselectSecureElement", "(I)V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doDeselectSecureElement},
      
   {"doOpenP2pConnection", "(I)Lcom/trustedlogic/trustednfc/android/internal/NativeP2pDevice;",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doOpenP2pConnection},  
      
   {"doOpenTagConnection", "(I)Lcom/trustedlogic/trustednfc/android/internal/NativeNfcTag;",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doOpenTagConnection},  
      
   {"doCancel", "()V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doCancel},
      
   {"doCheckLlcp", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doCheckLlcp},        
      
   {"doActivateLlcp", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doActivateLlcp},
            
   {"doCreateLlcpConnectionlessSocket", "(I)Lcom/trustedlogic/trustednfc/android/internal/NativeLlcpConnectionlessSocket;",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpConnectionlessSocket},
        
   {"doCreateLlcpServiceSocket", "(ILjava/lang/String;III)Lcom/trustedlogic/trustednfc/android/internal/NativeLlcpServiceSocket;",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpServiceSocket},   
      
   {"doCreateLlcpSocket", "(IIII)Lcom/trustedlogic/trustednfc/android/internal/NativeLlcpSocket;",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doCreateLlcpSocket},  
      
   {"doGetLastError", "()I",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doGetLastError},
      
   {"doSetProperties", "(II)V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_doSetProperties},

   {"disableDiscovery", "()V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_disableDiscovery},

   {"readerDiscovery", "()V",
      (void *)com_trustedlogic_trustednfc_android_internal_NfcManager_readerDiscovery},
};   
  
      
int register_com_trustedlogic_trustednfc_android_internal_NativeNfcManager(JNIEnv *e)
{
   trustednfc_jni_native_monitor_t *trustednfc_jni_native_monitor;

   trustednfc_jni_native_monitor = trustednfc_jni_init_monitor();
   if(trustednfc_jni_native_monitor == NULL)
   {
      LOGE("NFC Manager cannot recover native monitor %x\n", errno);
      return -1;
   }

   if(sem_init(&trustednfc_jni_manager_sem, 0, 0) == -1)
   {
      LOGE("NFC Manager Semaphore creation %x\n", errno);
      return -1;
   }
   
   if(sem_init(&trustednfc_jni_open_sem, 0, 0) == -1)
   {
      LOGE("NFC Open Semaphore creation %x\n", errno);
      return -1;
   }
   
   if(sem_init(&trustednfc_jni_init_sem, 0, 0) == -1)
   {
      LOGE("NFC Init Semaphore creation %x\n", errno);
      return -1;
   }
   
   if(sem_init(&trustednfc_jni_llcp_listen_sem, 0, 0) == -1)
   {
      LOGE("NFC Listen Semaphore creation %x\n", errno);
      return -1;
   }
   
   
   return jniRegisterNativeMethods(e,
      "com/trustedlogic/trustednfc/android/internal/NativeNfcManager",
      gMethods, NELEM(gMethods));
}

} /* namespace android */
