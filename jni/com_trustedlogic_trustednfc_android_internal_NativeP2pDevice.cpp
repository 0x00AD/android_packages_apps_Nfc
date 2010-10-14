
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
 * File            : com_trustedlogic_trustednfc_android_internal_NativeP2pDevice.c
 * Original-Author : Trusted Logic S.A. (Daniel Tomas)
 * Created         : 04-03-2010
 */
#include <semaphore.h>

#include "trustednfc_jni.h"

static sem_t trustednfc_jni_peer_sem;
static NFCSTATUS trustednfc_jni_cb_status = NFCSTATUS_FAILED;

uint8_t trustednfc_jni_p2p_presence_check = 0;

namespace android {

static phNfc_sData_t sGeneralBytes;

/*
 * Callbacks
 */
static void trustednfc_jni_presence_check_callback(void* pContext, NFCSTATUS status)
{   
   LOG_CALLBACK("trustednfc_jni_presence_check_callback", status);

   if(status != NFCSTATUS_SUCCESS)
   {
      trustednfc_jni_p2p_presence_check = 1; 
   }

   sem_post(&trustednfc_jni_peer_sem);
}
 
static void trustednfc_jni_connect_callback(void *pContext,
                                            phLibNfc_Handle hRemoteDev,
                                            phLibNfc_sRemoteDevInformation_t *psRemoteDevInfo, NFCSTATUS status)
{   
   

   
   LOG_CALLBACK("trustednfc_jni_connect_callback", status);
   
   if(status == NFCSTATUS_SUCCESS)
   {
      sGeneralBytes.length = psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length;
      sGeneralBytes.buffer = (uint8_t*)malloc(psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo_Length); 
      sGeneralBytes.buffer = psRemoteDevInfo->RemoteDevInfo.NfcIP_Info.ATRInfo;             
   }
   
   trustednfc_jni_cb_status = status;

   sem_post(&trustednfc_jni_peer_sem);
}

static void trustednfc_jni_disconnect_callback(void *pContext,
   phLibNfc_Handle hRemoteDev, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_disconnect_callback", status);
      
   trustednfc_jni_cb_status = status;

   sem_post(&trustednfc_jni_peer_sem);
}

static void trustednfc_jni_receive_callback(void *pContext,
   phNfc_sData_t *data, NFCSTATUS status)
{
   phNfc_sData_t **ptr = (phNfc_sData_t **)pContext;

   LOG_CALLBACK("trustednfc_jni_receive_callback", status);

   trustednfc_jni_cb_status = status;

   if(status == NFCSTATUS_SUCCESS)
      *ptr = data;
   else
      *ptr = NULL;

   sem_post(&trustednfc_jni_peer_sem);
}

static void trustednfc_jni_send_callback(void *pContext, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_send_callback", status);

   trustednfc_jni_cb_status = status;

   sem_post(&trustednfc_jni_peer_sem);
}

/*
 * Functions
 */

static phNfc_sData_t *trustednfc_jni_transceive_buffer;

static void trustednfc_jni_transceive_callback(void *pContext,
  phLibNfc_Handle handle, phNfc_sData_t *pResBuffer, NFCSTATUS status)
{
   LOG_CALLBACK("trustednfc_jni_transceive_callback", status);

   trustednfc_jni_cb_status = status;
   trustednfc_jni_transceive_buffer = pResBuffer;

   sem_post(&trustednfc_jni_peer_sem);
}

static jboolean com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doConnect(JNIEnv *e, jobject o)
{
   phLibNfc_Handle handle = 0;
   NFCSTATUS status;
   jboolean result = JNI_FALSE;

   jclass target_cls = NULL;
   jobject tag;
   jmethodID ctor;
   jfieldID f;
   jbyteArray generalBytes = NULL;
   int i;

   CONCURRENCY_LOCK();

   handle = trustednfc_jni_get_p2p_device_handle(e, o);

   LOGD("phLibNfc_RemoteDev_Connect(P2P)");
   REENTRANCE_LOCK();
   status = phLibNfc_RemoteDev_Connect(handle, trustednfc_jni_connect_callback, (void*)e);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_RemoteDev_Connect(P2P) returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   LOGD("phLibNfc_RemoteDev_Connect(P2P) returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_peer_sem);

   if(trustednfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   /* Set General Bytes */
   target_cls = e->GetObjectClass(o);
   
   f = e->GetFieldID(target_cls, "mGeneralBytes", "[B");
   
   LOGD("General Bytes Length = %d", sGeneralBytes.length); 
   LOGD("General Bytes =");
   for(i=0;i<sGeneralBytes.length;i++)
   {
      LOGD("0x%02x ", sGeneralBytes.buffer[i]);          
   }
       
   
   generalBytes = e->NewByteArray(sGeneralBytes.length);   
             
   e->SetByteArrayRegion(generalBytes, 0, 
                         sGeneralBytes.length, 
                         (jbyte *)sGeneralBytes.buffer);
             
   e->SetObjectField(o, f, generalBytes);   

   result = JNI_TRUE;

clean_and_return:
   CONCURRENCY_UNLOCK();
   return result;
}

static jboolean com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doDisconnect(JNIEnv *e,
   jobject o)
{
   phLibNfc_Handle   handle = 0;
   jboolean          result = JNI_FALSE;
   
   CONCURRENCY_LOCK();
   
   handle = trustednfc_jni_get_p2p_device_handle(e, o);
   
   /* Disconnect */
   LOGD("Disconnecting from target (handle = 0x%x)", handle);
   
   /* Presence Check */
   while(trustednfc_jni_p2p_presence_check == 0)
   {
      REENTRANCE_LOCK();
      phLibNfc_RemoteDev_CheckPresence(handle,trustednfc_jni_presence_check_callback,(void *)e);
      REENTRANCE_UNLOCK();

      /* Wait for callback response */
      sem_wait(&trustednfc_jni_peer_sem);
   }
   
   trustednfc_jni_p2p_presence_check = 0;
   LOGD("Target removed from the RF Field\n");

   REENTRANCE_LOCK();
   phLibNfc_RemoteDev_Disconnect(handle, NFC_DISCOVERY_CONTINUE,
      trustednfc_jni_disconnect_callback, (void *)e);
   REENTRANCE_UNLOCK();

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_peer_sem);

   if(trustednfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   result = JNI_TRUE;

clean_and_return:
   CONCURRENCY_UNLOCK();
   return result;
}

static jbyteArray com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doTransceive(JNIEnv *e,
   jobject o, jbyteArray data)
{
   NFCSTATUS status;
   uint8_t offset = 2;
   uint8_t *buf;
   uint32_t buflen;
   phLibNfc_sTransceiveInfo_t transceive_info;
   jbyteArray result = NULL;
   phLibNfc_Handle handle = trustednfc_jni_get_p2p_device_handle(e, o);
   
   CONCURRENCY_LOCK();

   /* Transceive*/
   LOGD("Transceive data to target (handle = 0x%x)", handle);

   buf = (uint8_t *)e->GetByteArrayElements(data, NULL);
   buflen = (uint32_t)e->GetArrayLength(data);
   
   LOGD("Buffer Length = %d\n", buflen);

   transceive_info.sSendData.buffer = buf; //+ offset;
   transceive_info.sSendData.length = buflen; //- offset;
   transceive_info.sRecvData.buffer = (uint8_t*)malloc(1024);
   transceive_info.sRecvData.length = 1024;

   if(transceive_info.sRecvData.buffer == NULL)
   {
      goto clean_and_return;
   }

   LOGD("phLibNfc_RemoteDev_Transceive(P2P)");
   REENTRANCE_LOCK();
   status = phLibNfc_RemoteDev_Transceive(handle, &transceive_info, trustednfc_jni_transceive_callback, (void *)e);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_RemoteDev_Transceive(P2P) returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;
   }
   LOGD("phLibNfc_RemoteDev_Transceive(P2P) returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_peer_sem);

   if(trustednfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   /* Copy results back to Java */
   result = e->NewByteArray(trustednfc_jni_transceive_buffer->length);
   if(result != NULL)
      e->SetByteArrayRegion(result, 0,
         trustednfc_jni_transceive_buffer->length,
         (jbyte *)trustednfc_jni_transceive_buffer->buffer);

clean_and_return:
   LOGD("P2P Transceive status = 0x%08x",trustednfc_jni_cb_status);
   if(transceive_info.sRecvData.buffer != NULL)
      free(transceive_info.sRecvData.buffer);

   e->ReleaseByteArrayElements(data,
      (jbyte *)transceive_info.sSendData.buffer, JNI_ABORT);

   CONCURRENCY_UNLOCK();

   return result;
}


static jbyteArray com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doReceive(
   JNIEnv *e, jobject o)
{
   NFCSTATUS status;
   struct timespec ts;
   phLibNfc_Handle handle;
   jbyteArray buf = NULL;
   static phNfc_sData_t *data;

   CONCURRENCY_LOCK();

   handle = trustednfc_jni_get_p2p_device_handle(e, o);
   
   /* Receive */
   LOGD("phLibNfc_RemoteDev_Receive()");
   REENTRANCE_LOCK();
   status = phLibNfc_RemoteDev_Receive(handle, trustednfc_jni_receive_callback,(void *)&data);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_RemoteDev_Receive() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;   
   }
   LOGD("phLibNfc_RemoteDev_Receive() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

   /* Wait for callback response */
   if(sem_wait(&trustednfc_jni_peer_sem) == -1)
   {
      goto clean_and_return;   
   }

   if(data == NULL)
   {
      goto clean_and_return;
   }

   buf = e->NewByteArray(data->length);
   e->SetByteArrayRegion(buf, 0, data->length, (jbyte *)data->buffer);

clean_and_return:
   CONCURRENCY_UNLOCK();
   return buf;
}

static jboolean com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doSend(
   JNIEnv *e, jobject o, jbyteArray buf)
{
   NFCSTATUS status;
   phNfc_sData_t data;
   jboolean result = JNI_FALSE;
   
   phLibNfc_Handle handle = trustednfc_jni_get_p2p_device_handle(e, o);
   
   CONCURRENCY_LOCK();

   /* Send */
   LOGD("Send data to the Initiator (handle = 0x%x)", handle);

   data.length = (uint32_t)e->GetArrayLength(buf);
   data.buffer = (uint8_t *)e->GetByteArrayElements(buf, NULL);

   LOGD("phLibNfc_RemoteDev_Send()");
   REENTRANCE_LOCK();
   status = phLibNfc_RemoteDev_Send(handle, &data, trustednfc_jni_send_callback,(void *)e);
   REENTRANCE_UNLOCK();
   if(status != NFCSTATUS_PENDING)
   {
      LOGE("phLibNfc_RemoteDev_Send() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));
      goto clean_and_return;   
   }
   LOGD("phLibNfc_RemoteDev_Send() returned 0x%04x[%s]", status, trustednfc_jni_get_status_name(status));

   /* Wait for callback response */
   sem_wait(&trustednfc_jni_peer_sem);

   if(trustednfc_jni_cb_status != NFCSTATUS_SUCCESS)
   {
      goto clean_and_return;
   }

   result = JNI_TRUE;

clean_and_return:
   if (result != JNI_TRUE)
   {
      e->ReleaseByteArrayElements(buf, (jbyte *)data.buffer, JNI_ABORT);
   }
   CONCURRENCY_UNLOCK();
   return result;
}

/*
 * JNI registration.
 */
static JNINativeMethod gMethods[] =
{
   {"doConnect", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doConnect},
   {"doDisconnect", "()Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doDisconnect},
   {"doTransceive", "([B)[B",
      (void *)com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doTransceive},
   {"doReceive", "()[B",
      (void *)com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doReceive},
   {"doSend", "([B)Z",
      (void *)com_trustedlogic_trustednfc_android_internal_NativeP2pDevice_doSend},
};

int register_com_trustedlogic_trustednfc_android_internal_NativeP2pDevice(JNIEnv *e)
{
   if(sem_init(&trustednfc_jni_peer_sem, 0, 0) == -1)
      return -1;

   return jniRegisterNativeMethods(e,
      "com/trustedlogic/trustednfc/android/internal/NativeP2pDevice",
      gMethods, NELEM(gMethods));
}

} // namepspace android
