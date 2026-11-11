/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-peer-link.h"
#include "android-peer-jni.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define PEER_MAGIC 0x4D504C4BU
#define PEER_VERSION 1
#define PEER_QUEUE_CAPACITY 64
#define PEER_AUTH 1
#define PEER_AUTH_RESULT 2

struct PeerFrameHeader {
	uint32_t magic;
	uint16_t version;
	uint16_t type;
	uint32_t length;
	uint32_t sequence;
};

struct mPeerLink {
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_mutex_t sendLock;
	pthread_cond_t sioCond;
	atomic_bool running;
	atomic_bool connected;
	atomic_bool localSioActive;
	atomic_bool remoteSioActive;
	int socketFd;
	int listenFd;
	atomic_bool host;
	char* address;
	uint16_t port;
	bool threadStarted;
	bool javaTransport;
	char pin[MPEER_PIN_LENGTH + 1];
	uint32_t sendSequence;
	struct mPeerSioWord {
		uint32_t seq;
		uint16_t value;
	} sioQueue[32];
	uint8_t sioQueueRead;
	uint8_t sioQueueWrite;
	uint8_t sioQueueCount;
	bool sioQueueOverflow;
	uint32_t sioNextSend;
	uint32_t sioNextRecv;
	uint32_t sioWaitGen;
	uint32_t resyncGen;
	uint32_t epoch;
	struct mPeerEvent queue[PEER_QUEUE_CAPACITY];
	size_t queueRead;
	size_t queueWrite;
	size_t queueCount;
	bool queueOverflow;
};

static bool _validPin(const char* pin) {
	int i;
	if (!pin || strlen(pin) != MPEER_PIN_LENGTH) return false;
	for (i = 0; i < MPEER_PIN_LENGTH; ++i) {
		if (pin[i] < '0' || pin[i] > '9') return false;
	}
	return true;
}

static bool _readAll(int fd, void* buffer, size_t size) {
	uint8_t* out = buffer;
	while (size) {
		ssize_t n = recv(fd, out, size, 0);
		if (n == 0) return false;
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		out += n;
		size -= (size_t) n;
	}
	return true;
}

static bool _writeAll(int fd, const void* buffer, size_t size) {
	const uint8_t* in = buffer;
	while (size) {
		ssize_t n = send(fd, in, size, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR) continue;
			return false;
		}
		if (n == 0) return false;
		in += n;
		size -= (size_t) n;
	}
	return true;
}

static bool _sendFrame(struct mPeerLink* link, int fd, uint16_t type, const void* data, uint32_t size) {
	struct PeerFrameHeader header;
	if (size > MPEER_MAX_PAYLOAD) return false;
	header.magic = htonl(PEER_MAGIC);
	header.version = htons(PEER_VERSION);
	header.type = htons(type);
	header.length = htonl(size);
	header.sequence = htonl(++link->sendSequence);
	return _writeAll(fd, &header, sizeof(header)) && (!size || _writeAll(fd, data, size));
}

static bool _receiveFrame(int fd, uint16_t* type, uint8_t* data, uint32_t* size) {
	struct PeerFrameHeader header;
	if (!_readAll(fd, &header, sizeof(header))) return false;
	if (ntohl(header.magic) != PEER_MAGIC || ntohs(header.version) != PEER_VERSION) return false;
	*type = ntohs(header.type);
	*size = ntohl(header.length);
	if (*size > MPEER_MAX_PAYLOAD) return false;
	return !*size || _readAll(fd, data, *size);
}

static void _queueEvent(struct mPeerLink* link, enum mPeerEventType type, uint16_t messageType, const void* data, uint32_t size, int error) {
	pthread_mutex_lock(&link->lock);
	if (link->queueCount < PEER_QUEUE_CAPACITY) {
		struct mPeerEvent* event = &link->queue[link->queueWrite];
		memset(event, 0, sizeof(*event));
		event->type = type;
		event->messageType = messageType;
		event->size = size;
		event->error = error;
		if (data && size) memcpy(event->data, data, size);
		link->queueWrite = (link->queueWrite + 1) % PEER_QUEUE_CAPACITY;
		++link->queueCount;
	} else {
		// Surface queue loss to the emulator thread instead of silently dropping it.
		link->queueOverflow = true;
	}
	pthread_mutex_unlock(&link->lock);
}

static void _clearSioLocked(struct mPeerLink* link) {
	link->sioQueueRead = 0;
	link->sioQueueWrite = 0;
	link->sioQueueCount = 0;
	link->sioQueueOverflow = false;
	link->sioNextSend = 1;
	link->sioNextRecv = 1;
	link->sioWaitGen = 0;
	++link->resyncGen;
	++link->epoch;
}

static void _setRemoteSioActive(struct mPeerLink* link, bool active) {
	bool wasActive = atomic_exchange(&link->remoteSioActive, active);
	if (wasActive != active) {
		pthread_mutex_lock(&link->lock);
		pthread_cond_broadcast(&link->sioCond);
		pthread_mutex_unlock(&link->lock);
	}
}

static uint32_t _minQueuedSeq(const struct mPeerLink* link) {
	uint32_t minSeq = UINT32_MAX;
	uint8_t i;
	if (!link->sioQueueCount)
		return 0;
	for (i = 0; i < link->sioQueueCount; ++i) {
		uint32_t seq = link->sioQueue[(link->sioQueueRead + i) % 32].seq;
		if (seq < minSeq)
			minSeq = seq;
	}
	return minSeq;
}

static void _enqueueSioLocked(struct mPeerLink* link, uint32_t seq, uint16_t value) {
	uint8_t i;
	if (seq < link->sioNextRecv)
		return;
	for (i = 0; i < link->sioQueueCount; ++i) {
		if (link->sioQueue[(link->sioQueueRead + i) % 32].seq == seq)
			return;
	}
	if (link->sioQueueCount == 32) {
		/* Keep the newer words. The waiter resyncs to the first seq still queued. */
		link->sioQueueRead = (link->sioQueueRead + 1) % 32;
		--link->sioQueueCount;
	}
	link->sioQueue[link->sioQueueWrite].seq = seq;
	link->sioQueue[link->sioQueueWrite].value = value;
	link->sioQueueWrite = (link->sioQueueWrite + 1) % 32;
	++link->sioQueueCount;
}

static bool _takeSeqLocked(struct mPeerLink* link, uint32_t seq, uint16_t* value) {
	uint8_t i;
	for (i = 0; i < link->sioQueueCount; ++i) {
		uint8_t index = (link->sioQueueRead + i) % 32;
		if (link->sioQueue[index].seq != seq)
			continue;
		*value = link->sioQueue[index].value;
		for (; i + 1 < link->sioQueueCount; ++i) {
			uint8_t from = (link->sioQueueRead + i + 1) % 32;
			uint8_t to = (link->sioQueueRead + i) % 32;
			link->sioQueue[to] = link->sioQueue[from];
		}
		--link->sioQueueCount;
		link->sioQueueWrite = (link->sioQueueRead + link->sioQueueCount) % 32;
		return true;
	}
	return false;
}

static void _applyResyncLocked(struct mPeerLink* link, uint32_t jump) {
	struct mPeerSioWord keep[32];
	uint8_t i, kept = 0;
	for (i = 0; i < link->sioQueueCount; ++i) {
		uint8_t index = (link->sioQueueRead + i) % 32;
		if (link->sioQueue[index].seq >= jump && kept < 32)
			keep[kept++] = link->sioQueue[index];
	}
	for (i = 0; i < kept; ++i)
		link->sioQueue[i] = keep[i];
	link->sioQueueRead = 0;
	link->sioQueueCount = kept;
	link->sioQueueWrite = kept % 32;
	link->sioQueueOverflow = false;
	if (link->sioNextRecv < jump)
		link->sioNextRecv = jump;
	if (link->sioNextSend < jump)
		link->sioNextSend = jump;
	++link->resyncGen;
}

static bool _sendSioWord(struct mPeerLink* link, uint32_t seq, uint16_t value) {
	uint8_t payload[8];
	uint32_t networkSeq = htonl(seq);
	uint16_t networkValue = htons(value);
	uint16_t networkFlags = 0;
	memcpy(payload, &networkSeq, 4);
	memcpy(payload + 4, &networkValue, 2);
	memcpy(payload + 6, &networkFlags, 2);
	return mPeerLinkSend(link, mPEER_MSG_SIO_MULTI, payload, sizeof(payload));
}

static void _sendResync(struct mPeerLink* link, uint32_t jump) {
	uint32_t networkSeq = htonl(jump);
	mPeerLinkSend(link, mPEER_MSG_SIO_RESYNC, &networkSeq, sizeof(networkSeq));
}

static uint64_t _monoNs(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t) now.tv_sec * 1000000000ULL + (uint64_t) now.tv_nsec;
}

static void _processPayload(struct mPeerLink* link, uint16_t type, const uint8_t* payload, uint32_t size) {
	if (type == mPEER_MSG_SIO_RESYNC && size == sizeof(uint32_t)) {
		uint32_t networkSeq;
		memcpy(&networkSeq, payload, sizeof(networkSeq));
		pthread_mutex_lock(&link->lock);
		_applyResyncLocked(link, ntohl(networkSeq));
		pthread_cond_broadcast(&link->sioCond);
		pthread_mutex_unlock(&link->lock);
		return;
	}
	if (type == mPEER_MSG_SIO_MULTI && size == 8) {
		uint32_t networkSeq;
		uint16_t networkValue;
		memcpy(&networkSeq, payload, sizeof(networkSeq));
		memcpy(&networkValue, payload + 4, sizeof(networkValue));
		pthread_mutex_lock(&link->lock);
		_enqueueSioLocked(link, ntohl(networkSeq), ntohs(networkValue));
		pthread_cond_broadcast(&link->sioCond);
		pthread_mutex_unlock(&link->lock);
		return;
	}
	if (type == mPEER_MSG_SIO_MULTI) {
		/* A word with no sequence number cannot be matched to a transfer. */
		return;
	}
	{
		if (type == mPEER_MSG_POSITION && size == 2 * sizeof(uint32_t)) {
			uint32_t networkSioActive;
			memcpy(&networkSioActive, payload + sizeof(uint32_t), sizeof(networkSioActive));
			_setRemoteSioActive(link, ntohl(networkSioActive) != 0);
		}
		_queueEvent(link, mPEER_EVENT_MESSAGE, type, payload, size, 0);
	}
}

static int _createListener(uint16_t port) {
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	int yes = 1;
	int no = 0;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &no, sizeof(no));
	struct sockaddr_in6 address;
	memset(&address, 0, sizeof(address));
	address.sin6_family = AF_INET6;
	address.sin6_addr = in6addr_any;
	address.sin6_port = htons(port);
	if (bind(fd, (struct sockaddr*) &address, sizeof(address)) < 0 || listen(fd, 1) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int _connectPeer(const char* host, uint16_t port) {
	char service[8];
	snprintf(service, sizeof(service), "%u", (unsigned) port);
	struct addrinfo hints, *results = NULL, *it;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, service, &hints, &results) != 0) return -1;
	int fd = -1;
	for (it = results; it; it = it->ai_next) {
		fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
		if (fd < 0) continue;
		if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(results);
	return fd;
}

static bool _authenticate(struct mPeerLink* link, int fd) {
	uint16_t type = 0;
	uint32_t size = 0;
	uint8_t payload[MPEER_MAX_PAYLOAD];
	if (atomic_load(&link->host)) {
		if (!_receiveFrame(fd, &type, payload, &size) || type != PEER_AUTH || size != MPEER_PIN_LENGTH) return false;
		uint8_t accepted = memcmp(payload, link->pin, MPEER_PIN_LENGTH) == 0 ? 1 : 0;
		if (!_sendFrame(link, fd, PEER_AUTH_RESULT, &accepted, 1)) return false;
		return accepted != 0;
	}
	if (!_sendFrame(link, fd, PEER_AUTH, link->pin, MPEER_PIN_LENGTH)) return false;
	if (!_receiveFrame(fd, &type, payload, &size) || type != PEER_AUTH_RESULT || size != 1) return false;
	return payload[0] == 1;
}

static void* _peerThread(void* opaque) {
	struct mPeerLink* link = opaque;
	int fd = -1;
	if (atomic_load(&link->host)) {
		link->listenFd = _createListener(link->port);
		if (link->listenFd < 0) {
			_queueEvent(link, mPEER_EVENT_ERROR, 0, NULL, 0, errno);
			goto done;
		}
		while (atomic_load(&link->running)) {
			fd = accept(link->listenFd, NULL, NULL);
			if (fd >= 0) break;
			if (errno != EINTR) goto done;
		}
	} else {
		fd = _connectPeer(link->address, link->port);
	}
	if (fd < 0 || !atomic_load(&link->running)) goto done;
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (!_authenticate(link, fd)) {
		_queueEvent(link, mPEER_EVENT_ERROR, 0, NULL, 0, EACCES);
		goto done;
	}
	link->socketFd = fd;
	atomic_store(&link->connected, true);
	_queueEvent(link, mPEER_EVENT_CONNECTED, 0, NULL, 0, 0);
	while (atomic_load(&link->running)) {
		uint16_t type;
		uint32_t size;
		uint8_t payload[MPEER_MAX_PAYLOAD];
		if (!_receiveFrame(fd, &type, payload, &size)) break;
		_processPayload(link, type, payload, size);
	}
done:
	atomic_store(&link->connected, false);
	if (atomic_load(&link->running))
		_queueEvent(link, mPEER_EVENT_DISCONNECTED, 0, NULL, 0, 0);
	atomic_store(&link->running, false);
	pthread_mutex_lock(&link->lock);
	_clearSioLocked(link);
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
	if (fd >= 0) close(fd);
	link->socketFd = -1;
	if (link->listenFd >= 0) {
		close(link->listenFd);
		link->listenFd = -1;
	}
	return NULL;
}

struct mPeerLink* mPeerLinkCreate(void) {
	struct mPeerLink* link = calloc(1, sizeof(*link));
	if (!link) return NULL;
	pthread_mutex_init(&link->lock, NULL);
	pthread_mutex_init(&link->sendLock, NULL);
	pthread_cond_init(&link->sioCond, NULL);
	atomic_init(&link->running, false);
	atomic_init(&link->connected, false);
	atomic_init(&link->localSioActive, false);
	atomic_init(&link->remoteSioActive, false);
	atomic_init(&link->host, false);
	link->socketFd = -1;
	link->listenFd = -1;
	link->sioNextSend = 1;
	link->sioNextRecv = 1;
	return link;
}

bool mPeerLinkHost(struct mPeerLink* link, uint16_t port, const char* pin) {
	if (!link || atomic_load(&link->running) || !_validPin(pin)) return false;
	atomic_store(&link->host, true);
	link->port = port ? port : MPEER_DEFAULT_PORT;
	memcpy(link->pin, pin, MPEER_PIN_LENGTH + 1);
	pthread_mutex_lock(&link->lock);
	_clearSioLocked(link);
	pthread_mutex_unlock(&link->lock);
	link->sendSequence = 0;
	atomic_store(&link->running, true);
	if (pthread_create(&link->thread, NULL, _peerThread, link) != 0) {
		atomic_store(&link->running, false);
		return false;
	}
	link->threadStarted = true;
	return true;
}

bool mPeerLinkJoin(struct mPeerLink* link, const char* address, uint16_t port, const char* pin) {
	if (!link || !address || !address[0] || atomic_load(&link->running) || !_validPin(pin)) return false;
	atomic_store(&link->host, false);
	link->port = port ? port : MPEER_DEFAULT_PORT;
	link->address = strdup(address);
	if (!link->address) return false;
	memcpy(link->pin, pin, MPEER_PIN_LENGTH + 1);
	pthread_mutex_lock(&link->lock);
	_clearSioLocked(link);
	pthread_mutex_unlock(&link->lock);
	link->sendSequence = 0;
	atomic_store(&link->running, true);
	if (pthread_create(&link->thread, NULL, _peerThread, link) != 0) {
		atomic_store(&link->running, false);
		free(link->address);
		link->address = NULL;
		return false;
	}
	link->threadStarted = true;
	return true;
}

bool mPeerLinkSend(struct mPeerLink* link, uint16_t type, const void* data, uint32_t size) {
	if (!link || !atomic_load(&link->connected) || size > MPEER_MAX_PAYLOAD || (size && !data)) return false;
	pthread_mutex_lock(&link->sendLock);
	bool ok = false;
	if (link->javaTransport) {
		struct PeerFrameHeader header;
		uint8_t frame[sizeof(header) + MPEER_MAX_PAYLOAD];
		header.magic = htonl(PEER_MAGIC);
		header.version = htons(PEER_VERSION);
		header.type = htons(type);
		header.length = htonl(size);
		header.sequence = htonl(++link->sendSequence);
		memcpy(frame, &header, sizeof(header));
		if (size)
			memcpy(frame + sizeof(header), data, size);
		ok = mAndroidPeerJniSendFrame(frame, sizeof(header) + size);
	} else {
		int fd = link->socketFd;
		ok = fd >= 0 && _sendFrame(link, fd, type, data, size);
		if (!ok && fd >= 0)
			shutdown(fd, SHUT_RDWR);
	}
	pthread_mutex_unlock(&link->sendLock);
	return ok;
}

bool mPeerLinkPoll(struct mPeerLink* link, struct mPeerEvent* event) {
	if (!link || !event) return false;
	pthread_mutex_lock(&link->lock);
	if (link->queueOverflow) {
		memset(event, 0, sizeof(*event));
		event->type = mPEER_EVENT_ERROR;
		event->error = ENOBUFS;
		link->queueOverflow = false;
		pthread_mutex_unlock(&link->lock);
		return true;
	}
	if (!link->queueCount) {
		pthread_mutex_unlock(&link->lock);
		return false;
	}
	*event = link->queue[link->queueRead];
	link->queueRead = (link->queueRead + 1) % PEER_QUEUE_CAPACITY;
	--link->queueCount;
	pthread_mutex_unlock(&link->lock);
	return true;
}

bool mPeerLinkIsConnected(struct mPeerLink* link) {
	return link && atomic_load(&link->connected);
}

uint32_t mPeerLinkEpoch(const struct mPeerLink* link) {
	return link ? link->epoch : 0;
}

static enum mPeerSioResult _resetBoth(struct mPeerLink* link, uint32_t jump) {
	pthread_mutex_lock(&link->lock);
	_applyResyncLocked(link, jump);
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
	_sendResync(link, jump);
	return mPEER_SIO_RESET;
}

enum mPeerSioResult mPeerLinkSioPoll(struct mPeerLink* link, uint16_t localValue, uint32_t* seq, uint64_t startNs, uint16_t* remoteValue) {
	bool live;
	bool timedOut;
	uint64_t now;
	uint32_t minSeq = 0;
	uint32_t waitGen;
	if (!link || !seq || !remoteValue || !atomic_load(&link->connected))
		return mPEER_SIO_OFFLINE;
	now = _monoNs();
	live = atomic_load(&link->localSioActive) && atomic_load(&link->remoteSioActive);
	timedOut = startNs && now >= startNs && (now - startNs) >= (live ? MPEER_SIO_TIMEOUT_NS : MPEER_SIO_HOLD_NS);

	if (*seq == 0) {
		uint32_t sendSeq;
		if (timedOut) {
			pthread_mutex_lock(&link->lock);
			sendSeq = link->sioNextSend > link->sioNextRecv ? link->sioNextSend : link->sioNextRecv;
			minSeq = _minQueuedSeq(link);
			pthread_mutex_unlock(&link->lock);
			if (minSeq > sendSeq)
				sendSeq = minSeq;
			if (!sendSeq)
				sendSeq = 1;
			return _resetBoth(link, sendSeq);
		}
		if (!live)
			return mPEER_SIO_WAIT;
		pthread_mutex_lock(&link->lock);
		sendSeq = link->sioNextSend;
		pthread_mutex_unlock(&link->lock);
		if (!_sendSioWord(link, sendSeq, localValue))
			return mPEER_SIO_WAIT;
		pthread_mutex_lock(&link->lock);
		if (link->sioNextSend == sendSeq)
			link->sioNextSend = sendSeq + 1;
		link->sioWaitGen = link->resyncGen;
		pthread_mutex_unlock(&link->lock);
		*seq = sendSeq;
	}

	pthread_mutex_lock(&link->lock);
	waitGen = link->sioWaitGen;
	if (link->resyncGen != waitGen) {
		pthread_mutex_unlock(&link->lock);
		return mPEER_SIO_RESET;
	}
	if (_takeSeqLocked(link, *seq, remoteValue)) {
		if (link->sioNextRecv <= *seq)
			link->sioNextRecv = *seq + 1;
		pthread_mutex_unlock(&link->lock);
		return mPEER_SIO_READY;
	}
	minSeq = _minQueuedSeq(link);
	pthread_mutex_unlock(&link->lock);
	if (minSeq > *seq || timedOut) {
		uint32_t jump = minSeq > *seq ? minSeq : *seq + 1;
		return _resetBoth(link, jump);
	}
	return mPEER_SIO_WAIT;
}

enum mPeerSioResult mPeerLinkSioTake(struct mPeerLink* link, uint32_t* seq, uint16_t* remoteValue) {
	uint32_t minSeq;
	if (!link || !seq || !remoteValue || !atomic_load(&link->connected))
		return mPEER_SIO_OFFLINE;
	if (!atomic_load(&link->localSioActive))
		return mPEER_SIO_WAIT;
	pthread_mutex_lock(&link->lock);
	minSeq = _minQueuedSeq(link);
	if (!minSeq) {
		pthread_mutex_unlock(&link->lock);
		return mPEER_SIO_WAIT;
	}
	if (minSeq > link->sioNextRecv) {
		pthread_mutex_unlock(&link->lock);
		return _resetBoth(link, minSeq);
	}
	if (!_takeSeqLocked(link, minSeq, remoteValue)) {
		pthread_mutex_unlock(&link->lock);
		return mPEER_SIO_WAIT;
	}
	link->sioNextRecv = minSeq + 1;
	*seq = minSeq;
	pthread_mutex_unlock(&link->lock);
	return mPEER_SIO_READY;
}

bool mPeerLinkSioReply(struct mPeerLink* link, uint32_t seq, uint16_t localValue) {
	if (!link || !seq || !atomic_load(&link->connected))
		return false;
	if (!_sendSioWord(link, seq, localValue))
		return false;
	pthread_mutex_lock(&link->lock);
	if (link->sioNextSend <= seq)
		link->sioNextSend = seq + 1;
	pthread_mutex_unlock(&link->lock);
	return true;
}

bool mPeerLinkExchangeU16(struct mPeerLink* link, uint16_t localValue, uint16_t* remoteValue) {
	uint32_t seq = 0;
	/* One non-blocking attempt. The link driver completes the transfer later. */
	return mPeerLinkSioPoll(link, localValue, &seq, _monoNs(), remoteValue) == mPEER_SIO_READY;
}

void mPeerLinkStop(struct mPeerLink* link) {
	if (!link) return;
	atomic_store(&link->running, false);
	atomic_store(&link->connected, false);
	pthread_mutex_lock(&link->lock);
	_clearSioLocked(link);
	link->queueRead = 0;
	link->queueWrite = 0;
	link->queueCount = 0;
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
	if (!link->threadStarted)
		return;
	if (link->socketFd >= 0) shutdown(link->socketFd, SHUT_RDWR);
	if (link->listenFd >= 0) shutdown(link->listenFd, SHUT_RDWR);
	pthread_join(link->thread, NULL);
	link->threadStarted = false;
	link->socketFd = -1;
	link->listenFd = -1;
	free(link->address);
	link->address = NULL;
}

void mPeerLinkUseJavaTransport(struct mPeerLink* link, bool enabled) {
	if (link)
		link->javaTransport = enabled;
}

void mPeerLinkSetConnected(struct mPeerLink* link, bool connected) {
	if (!link)
		return;
	bool wasConnected = atomic_load(&link->connected);
	atomic_store(&link->running, connected);
	atomic_store(&link->connected, connected);
	// Transport connectivity is not the same as an active GBA multiplayer
	// transfer. Wait for each peer's POSITION message to advertise SIO state.
	atomic_store(&link->localSioActive, false);
	atomic_store(&link->remoteSioActive, false);
	pthread_mutex_lock(&link->lock);
	/* Every connect and disconnect is a new SIO session. */
	_clearSioLocked(link);
	if (!connected) {
		// Events belong to one peer session; never replay them after reconnect.
		link->queueRead = 0;
		link->queueWrite = 0;
		link->queueCount = 0;
		link->queueOverflow = false;
	}
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
	if (connected && !wasConnected)
		_queueEvent(link, mPEER_EVENT_CONNECTED, 0, NULL, 0, 0);
	else if (!connected && wasConnected)
		_queueEvent(link, mPEER_EVENT_DISCONNECTED, 0, NULL, 0, 0);
}

void mPeerLinkSetLocalSioActive(struct mPeerLink* link, bool active) {
	if (!link)
		return;
	bool wasActive = atomic_exchange(&link->localSioActive, active);
	if (wasActive == active)
		return;
	/* Leave queued words in place. A map load clears this flag, and wiping
	 * the queue here is what desynchronised the two phones. */
	pthread_mutex_lock(&link->lock);
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
}

void mPeerLinkSetHostRole(struct mPeerLink* link, bool host) {
	if (link)
		atomic_store(&link->host, host);
}

bool mPeerLinkReceiveFrame(struct mPeerLink* link, const uint8_t* frame, size_t size) {
	// Reject callbacks that were already in flight when the previous peer disconnected.
	if (!link || !atomic_load(&link->connected) || !frame || size < sizeof(struct PeerFrameHeader))
		return false;
	struct PeerFrameHeader header;
	memcpy(&header, frame, sizeof(header));
	if (ntohl(header.magic) != PEER_MAGIC || ntohs(header.version) != PEER_VERSION)
		return false;
	uint32_t payloadSize = ntohl(header.length);
	if (payloadSize > MPEER_MAX_PAYLOAD || size != sizeof(header) + payloadSize)
		return false;
	_processPayload(link, ntohs(header.type), frame + sizeof(header), payloadSize);
	return true;
}

void mPeerLinkDestroy(struct mPeerLink* link) {
	if (!link) return;
	mPeerLinkStop(link);
	pthread_cond_destroy(&link->sioCond);
	pthread_mutex_destroy(&link->sendLock);
	pthread_mutex_destroy(&link->lock);
	free(link);
}
