/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-peer-jni.h"

#include <jni.h>
#include <stdlib.h>

static JavaVM* sJavaVM;
static jclass sManagerClass;
static jmethodID sSendFrameMethod;
static jmethodID sShowDialogMethod;
static jmethodID sSyncConnectionMethod;

static JNIEnv* _getEnv(bool* attached) {
    JNIEnv* env = NULL;
    *attached = false;
    if (!sJavaVM)
        return NULL;

    jint result = (*sJavaVM)->GetEnv(sJavaVM, (void**) &env, JNI_VERSION_1_6);
    if (result == JNI_OK)
        return env;
    if (result != JNI_EDETACHED)
        return NULL;
    if ((*sJavaVM)->AttachCurrentThread(sJavaVM, (void**) &env, NULL) != JNI_OK)
        return NULL;
    *attached = true;
    return env;
}

bool mAndroidPeerJniSendFrame(const uint8_t* frame, size_t size) {
    if (!frame || !size || size > 8192 || !sManagerClass || !sSendFrameMethod)
        return false;

    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return false;

    jbyteArray data = (*env)->NewByteArray(env, (jsize) size);
    if (!data) {
        if (attached)
            (*sJavaVM)->DetachCurrentThread(sJavaVM);
        return false;
    }
    (*env)->SetByteArrayRegion(env, data, 0, (jsize) size, (const jbyte*) frame);
    jboolean sent = (*env)->CallStaticBooleanMethod(env, sManagerClass, sSendFrameMethod, data);
    bool exception = (*env)->ExceptionCheck(env);
    if (exception)
        (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, data);

    if (attached)
        (*sJavaVM)->DetachCurrentThread(sJavaVM);
    return sent == JNI_TRUE && !exception;
}

void mAndroidPeerJniSyncConnection(void) {
    if (!sManagerClass || !sSyncConnectionMethod)
        return;
    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return;
    (*env)->CallStaticVoidMethod(env, sManagerClass, sSyncConnectionMethod);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (attached)
        (*sJavaVM)->DetachCurrentThread(sJavaVM);
}

void mAndroidPeerJniShowDialog(void) {
    if (!sManagerClass || !sShowDialogMethod)
        return;
    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return;
    (*env)->CallStaticVoidMethod(env, sManagerClass, sShowDialogMethod);
    if ((*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (attached)
        (*sJavaVM)->DetachCurrentThread(sJavaVM);
}

JNIEXPORT void JNICALL Java_org_libsdl_app_PeerLinkManager_nativeInitialize(JNIEnv* env, jclass cls) {
    if ((*env)->GetJavaVM(env, &sJavaVM) != JNI_OK)
        return;
    if (sManagerClass)
        (*env)->DeleteGlobalRef(env, sManagerClass);
    sManagerClass = (jclass) (*env)->NewGlobalRef(env, cls);
    sSendFrameMethod = (*env)->GetStaticMethodID(env, sManagerClass, "sendFrame", "([B)Z");
    sShowDialogMethod = (*env)->GetStaticMethodID(env, sManagerClass, "showPairingDialog", "()V");
    sSyncConnectionMethod = (*env)->GetStaticMethodID(env, sManagerClass, "syncNativeConnection", "()V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        sSendFrameMethod = NULL;
        sShowDialogMethod = NULL;
        sSyncConnectionMethod = NULL;
    }
}

JNIEXPORT void JNICALL Java_org_libsdl_app_PeerLinkManager_nativePeerConnected(JNIEnv* env, jobject instance, jboolean hostRole) {
    (void) env;
    (void) instance;
    mAndroidPeerLinkSetConnected(hostRole == JNI_TRUE);
}

JNIEXPORT void JNICALL Java_org_libsdl_app_PeerLinkManager_nativePeerDisconnected(JNIEnv* env, jobject instance) {
    (void) env;
    (void) instance;
    mAndroidPeerLinkSetDisconnected();
}

JNIEXPORT void JNICALL Java_org_libsdl_app_PeerLinkManager_nativePeerFrame(JNIEnv* env, jobject instance, jbyteArray frame) {
    (void) instance;
    if (!frame)
        return;
    jsize size = (*env)->GetArrayLength(env, frame);
    if (size <= 0 || size > 8192)
        return;
    uint8_t* bytes = malloc((size_t) size);
    if (!bytes)
        return;
    (*env)->GetByteArrayRegion(env, frame, 0, size, (jbyte*) bytes);
    if (!(*env)->ExceptionCheck(env))
        mAndroidPeerLinkReceiveFrame(bytes, (size_t) size);
    else
        (*env)->ExceptionClear(env);
    free(bytes);
}
