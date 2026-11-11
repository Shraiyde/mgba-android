/* Copyright (c) 2026
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-peer-jni.h"

#include <jni.h>
#include <pthread.h>
#include <stdlib.h>

static JavaVM* sJavaVM;
static jclass sManagerClass;
static jmethodID sSendFrameMethod;
static jmethodID sShowDialogMethod;
static jmethodID sSyncConnectionMethod;
static pthread_mutex_t sJniLock = PTHREAD_MUTEX_INITIALIZER;

static JNIEnv* _getEnv(bool* attached) {
    JNIEnv* env = NULL;
    *attached = false;

    pthread_mutex_lock(&sJniLock);
    JavaVM* vm = sJavaVM;
    pthread_mutex_unlock(&sJniLock);
    if (!vm)
        return NULL;

    jint result = (*vm)->GetEnv(vm, (void**) &env, JNI_VERSION_1_6);
    if (result == JNI_OK)
        return env;
    if (result != JNI_EDETACHED)
        return NULL;
    if ((*vm)->AttachCurrentThread(vm, (void**) &env, NULL) != JNI_OK)
        return NULL;
    *attached = true;
    return env;
}

bool mAndroidPeerJniSendFrame(const uint8_t* frame, size_t size) {
    if (!frame || !size || size > 8192)
        return false;

    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return false;

    jmethodID method;
    jclass managerClass;
    pthread_mutex_lock(&sJniLock);
    method = sSendFrameMethod;
    managerClass = NULL;
    if (sManagerClass && method)
        managerClass = (jclass) (*env)->NewLocalRef(env, sManagerClass);
    pthread_mutex_unlock(&sJniLock);
    if (!managerClass) {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        if (attached) {
            pthread_mutex_lock(&sJniLock);
            JavaVM* vm = sJavaVM;
            pthread_mutex_unlock(&sJniLock);
            if (vm) (*vm)->DetachCurrentThread(vm);
        }
        return false;
    }

    jbyteArray data = (*env)->NewByteArray(env, (jsize) size);
    if (!data) {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, managerClass);
        if (attached) {
            pthread_mutex_lock(&sJniLock);
            JavaVM* vm = sJavaVM;
            pthread_mutex_unlock(&sJniLock);
            if (vm) (*vm)->DetachCurrentThread(vm);
        }
        return false;
    }
    (*env)->SetByteArrayRegion(env, data, 0, (jsize) size, (const jbyte*) frame);
    bool exception = (*env)->ExceptionCheck(env);
    jboolean sent = JNI_FALSE;
    if (exception) {
        (*env)->ExceptionClear(env);
    } else {
        sent = (*env)->CallStaticBooleanMethod(env, managerClass, method, data);
        exception = (*env)->ExceptionCheck(env);
        if (exception)
            (*env)->ExceptionClear(env);
    }
    (*env)->DeleteLocalRef(env, data);
    (*env)->DeleteLocalRef(env, managerClass);

    if (attached) {
        pthread_mutex_lock(&sJniLock);
        JavaVM* vm = sJavaVM;
        pthread_mutex_unlock(&sJniLock);
        if (vm) (*vm)->DetachCurrentThread(vm);
    }
    return sent == JNI_TRUE && !exception;
}

void mAndroidPeerJniSyncConnection(void) {
    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return;

    jmethodID method;
    jclass managerClass;
    pthread_mutex_lock(&sJniLock);
    method = sSyncConnectionMethod;
    managerClass = NULL;
    if (sManagerClass && method)
        managerClass = (jclass) (*env)->NewLocalRef(env, sManagerClass);
    pthread_mutex_unlock(&sJniLock);
    if (!managerClass && (*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (managerClass) {
        (*env)->CallStaticVoidMethod(env, managerClass, method);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, managerClass);
    }
    if (attached) {
        pthread_mutex_lock(&sJniLock);
        JavaVM* vm = sJavaVM;
        pthread_mutex_unlock(&sJniLock);
        if (vm) (*vm)->DetachCurrentThread(vm);
    }
}

void mAndroidPeerJniShowDialog(void) {
    bool attached;
    JNIEnv* env = _getEnv(&attached);
    if (!env)
        return;

    jmethodID method;
    jclass managerClass;
    pthread_mutex_lock(&sJniLock);
    method = sShowDialogMethod;
    managerClass = NULL;
    if (sManagerClass && method)
        managerClass = (jclass) (*env)->NewLocalRef(env, sManagerClass);
    pthread_mutex_unlock(&sJniLock);
    if (!managerClass && (*env)->ExceptionCheck(env))
        (*env)->ExceptionClear(env);
    if (managerClass) {
        (*env)->CallStaticVoidMethod(env, managerClass, method);
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, managerClass);
    }
    if (attached) {
        pthread_mutex_lock(&sJniLock);
        JavaVM* vm = sJavaVM;
        pthread_mutex_unlock(&sJniLock);
        if (vm) (*vm)->DetachCurrentThread(vm);
    }
}

JNIEXPORT void JNICALL Java_org_libsdl_app_PeerLinkManager_nativeInitialize(JNIEnv* env, jclass cls) {
    JavaVM* vm = NULL;
    if ((*env)->GetJavaVM(env, &vm) != JNI_OK || !vm)
        return;

    jmethodID sendFrame = (*env)->GetStaticMethodID(env, cls, "sendFrame", "([B)Z");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return;
    }
    jmethodID showDialog = (*env)->GetStaticMethodID(env, cls, "showPairingDialog", "()V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return;
    }
    jmethodID syncConnection = (*env)->GetStaticMethodID(env, cls, "syncNativeConnection", "()V");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        return;
    }

    jclass globalClass = (jclass) (*env)->NewGlobalRef(env, cls);
    if (!globalClass) {
        if ((*env)->ExceptionCheck(env))
            (*env)->ExceptionClear(env);
        return;
    }

    pthread_mutex_lock(&sJniLock);
    jclass oldClass = sManagerClass;
    sJavaVM = vm;
    sManagerClass = globalClass;
    sSendFrameMethod = sendFrame;
    sShowDialogMethod = showDialog;
    sSyncConnectionMethod = syncConnection;
    pthread_mutex_unlock(&sJniLock);

    if (oldClass)
        (*env)->DeleteGlobalRef(env, oldClass);
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
