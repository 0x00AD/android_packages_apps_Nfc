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
 * File            : trustednfc_jni.h
 * Original-Author : Trusted Logic S.A. (Jeremie Corbier)
 * Created         : 26-08-2009
 */
#ifndef __TRUSTEDNFC_JNI_H__
#define __TRUSTEDNFC_JNI_H__

#define LOG_TAG "Trusted_NFC_JNI"

#include <JNIHelp.h>
#include <jni.h>

#include <pthread.h>

extern "C" {
#include <phNfcStatus.h>
#include <phNfcTypes.h>
#include <phNfcIoctlCode.h>
#include <phLibNfc.h>
#include <phDal4Nfc_messageQueueLib.h>
}
#include <cutils/properties.h> // for property_get

/* Discovery modes -- keep in sync with NFCManager.DISCOVERY_MODE_* */
#define DISCOVERY_MODE_TAG_READER         0
#define DISCOVERY_MODE_NFCIP1             1
#define DISCOVERY_MODE_CARD_EMULATION     2

#define DISCOVERY_MODE_TABLE_SIZE         3

#define DISCOVERY_MODE_DISABLED           0
#define DISCOVERY_MODE_ENABLED            1

#define MODE_P2P_TARGET                   0
#define MODE_P2P_INITIATOR                1

/* Properties values */
#define PROPERTY_LLCP_LTO                 0
#define PROPERTY_LLCP_MIU                 1
#define PROPERTY_LLCP_WKS                 2
#define PROPERTY_LLCP_OPT                 3
#define PROPERTY_NFC_DISCOVERY_A          4
#define PROPERTY_NFC_DISCOVERY_B          5  
#define PROPERTY_NFC_DISCOVERY_F          6
#define PROPERTY_NFC_DISCOVERY_15693      7
#define PROPERTY_NFC_DISCOVERY_NCFIP      8                     

/* Error codes */
#define ERROR_BUFFER_TOO_SMALL            -12
#define ERROR_INSUFFICIENT_RESOURCES      -9

/* Name strings for target types */
#define TARGET_TYPE_ISO14443        "Iso14443"
#define TARGET_TYPE_MIFARE_UL       "MifareUL"
#define TARGET_TYPE_MIFARE_1K       "Mifare1K"
#define TARGET_TYPE_MIFARE_4K       "Mifare4K"
#define TARGET_TYPE_MIFARE_DESFIRE  "MifareDESFIRE"
#define TARGET_TYPE_MIFARE_UNKNOWN  "Unknown Mifare"
#define TARGET_TYPE_FELICA          "Felica"
#define TARGET_TYPE_JEWEL           "Jewel"
#define TARGET_TYPE_UNKNOWN         "Unknown Type"

/* Utility macros for logging */
#define GET_LEVEL(status) ((status)==NFCSTATUS_SUCCESS)?ANDROID_LOG_DEBUG:ANDROID_LOG_WARN
#define LOG_CALLBACK(funcName, status)  LOG_PRI(GET_LEVEL(status), LOG_TAG, "Callback: %s() - status=0x%04x[%s]", funcName, status, trustednfc_jni_get_status_name(status));

struct trustednfc_jni_native_data
{
   /* Thread handle */
   pthread_t thread;
   int running;

   /* Our VM */
   JavaVM *vm;
   int env_version;

   /* Reference to the NFCManager instance */
   jobject manager;

   /* Cached objects */
   jobject cached_NfcTag;
   jobject cached_P2pDevice;

   /* Target discovery configuration */
   int discovery_modes_state[DISCOVERY_MODE_TABLE_SIZE];
   phLibNfc_sADD_Cfg_t discovery_cfg;
   phLibNfc_Registry_Info_t registry_info;
   
   /* Secure Element selected */
   int seId;
   
   /* LLCP params */
   int lto;
   int miu;
   int wks;
   int opt;

   /* Tag detected */
   jobject tag;

   /* Lib Status */
   NFCSTATUS status;
   
};

typedef struct trustednfc_jni_native_monitor
{
   /* Mutex protecting native library against reentrance */
   pthread_mutex_t reentrance_mutex;

   /* Mutex protecting native library against concurrency */
   pthread_mutex_t concurrency_mutex;

} trustednfc_jni_native_monitor_t;

/* TODO: treat errors and add traces */
#define REENTRANCE_LOCK()        pthread_mutex_lock(&trustednfc_jni_get_monitor()->reentrance_mutex)
#define REENTRANCE_UNLOCK()      pthread_mutex_unlock(&trustednfc_jni_get_monitor()->reentrance_mutex)
#define CONCURRENCY_LOCK()       pthread_mutex_lock(&trustednfc_jni_get_monitor()->concurrency_mutex)
#define CONCURRENCY_UNLOCK()     pthread_mutex_unlock(&trustednfc_jni_get_monitor()->concurrency_mutex)

namespace android {

const char* trustednfc_jni_get_status_name(NFCSTATUS status);
int trustednfc_jni_cache_object(JNIEnv *e, const char *clsname,
   jobject *cached_obj);
struct trustednfc_jni_native_data* trustednfc_jni_get_nat(JNIEnv *e, jobject o);
struct trustednfc_jni_native_data* trustednfc_jni_get_nat_ext(JNIEnv *e);
trustednfc_jni_native_monitor_t* trustednfc_jni_init_monitor(void);
trustednfc_jni_native_monitor_t* trustednfc_jni_get_monitor(void);

/* P2P */   
phLibNfc_Handle trustednfc_jni_get_p2p_device_handle(JNIEnv *e, jobject o);
jshort trustednfc_jni_get_p2p_device_mode(JNIEnv *e, jobject o);

/* TAG */
phLibNfc_Handle trustednfc_jni_get_nfc_tag_handle(JNIEnv *e, jobject o);
jstring trustednfc_jni_get_nfc_tag_type(JNIEnv *e, jobject o);

/* LLCP */
phLibNfc_Handle trustednfc_jni_get_nfc_socket_handle(JNIEnv *e, jobject o);

int register_com_trustedlogic_trustednfc_android_internal_NativeNfcManager(JNIEnv *e);
int register_com_trustedlogic_trustednfc_android_internal_NativeNfcTag(JNIEnv *e);

int register_com_trustedlogic_trustednfc_android_internal_NativeNdefTag(JNIEnv *e);
int register_com_trustedlogic_trustednfc_android_NdefMessage(JNIEnv *e);
int register_com_trustedlogic_trustednfc_android_NdefRecord(JNIEnv *e);

int register_com_trustedlogic_trustednfc_android_internal_NativeP2pDevice(JNIEnv *e);

int register_com_trustedlogic_trustednfc_android_internal_NativeLlcpConnectionlessSocket(JNIEnv *e);
int register_com_trustedlogic_trustednfc_android_internal_NativeLlcpServiceSocket(JNIEnv *e);
int register_com_trustedlogic_trustednfc_android_internal_NativeLlcpSocket(JNIEnv *e);

} // namespace android

#endif
