/* Copyright (c) 2026
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "android-peer-link.h"

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
#define PEER_QUEUE_CAPACITY 32
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
	int socketFd;
	int listenFd;
	bool host;
	char* address;
	uint16_t port;
	bool threadStarted;
	char pin[MPEER_PIN_LENGTH + 1];
	uint32_t sendSequence;
	uint16_t sioQueue[32];
	uint8_t sioQueueRead;
	uint8_t sioQueueWrite;
	uint8_t sioQueueCount;
	bool sioQueueOverflow;
	struct mPeerEvent queue[PEER_QUEUE_CAPACITY];
	size_t queueRead;
	size_t queueWrite;
	size_t queueCount;
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
	}
	pthread_mutex_unlock(&link->lock);
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
	if (link->host) {
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
	if (link->host) {
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
		if (type == mPEER_MSG_SIO_MULTI && size == sizeof(uint16_t)) {
			uint16_t value;
			memcpy(&value, payload, sizeof(value));
			pthread_mutex_lock(&link->lock);
			if (link->sioQueueCount < 32) {
				link->sioQueue[link->sioQueueWrite] = ntohs(value);
				link->sioQueueWrite = (link->sioQueueWrite + 1) % 32;
				++link->sioQueueCount;
			} else {
				link->sioQueueOverflow = true;
			}
			pthread_cond_broadcast(&link->sioCond);
			pthread_mutex_unlock(&link->lock);
		} else {
			_queueEvent(link, mPEER_EVENT_MESSAGE, type, payload, size, 0);
		}
	}
done:
	atomic_store(&link->connected, false);
	if (atomic_load(&link->running))
		_queueEvent(link, mPEER_EVENT_DISCONNECTED, 0, NULL, 0, 0);
	atomic_store(&link->running, false);
	pthread_mutex_lock(&link->lock);
	link->sioQueueRead = 0;
	link->sioQueueWrite = 0;
	link->sioQueueCount = 0;
	link->sioQueueOverflow = false;
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
	link->socketFd = -1;
	link->listenFd = -1;
	return link;
}

bool mPeerLinkHost(struct mPeerLink* link, uint16_t port, const char* pin) {
	if (!link || atomic_load(&link->running) || !_validPin(pin)) return false;
	link->host = true;
	link->port = port ? port : MPEER_DEFAULT_PORT;
	memcpy(link->pin, pin, MPEER_PIN_LENGTH + 1);
	link->sioQueueRead = 0;
	link->sioQueueWrite = 0;
	link->sioQueueCount = 0;
	link->sioQueueOverflow = false;
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
	link->host = false;
	link->port = port ? port : MPEER_DEFAULT_PORT;
	link->address = strdup(address);
	if (!link->address) return false;
	memcpy(link->pin, pin, MPEER_PIN_LENGTH + 1);
	link->sioQueueRead = 0;
	link->sioQueueWrite = 0;
	link->sioQueueCount = 0;
	link->sioQueueOverflow = false;
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
	int fd = link->socketFd;
	bool ok = fd >= 0 && _sendFrame(link, fd, type, data, size);
	pthread_mutex_unlock(&link->sendLock);
	if (!ok && fd >= 0) shutdown(fd, SHUT_RDWR);
	return ok;
}

bool mPeerLinkPoll(struct mPeerLink* link, struct mPeerEvent* event) {
	if (!link || !event) return false;
	pthread_mutex_lock(&link->lock);
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

bool mPeerLinkExchangeU16(struct mPeerLink* link, uint16_t localValue, uint16_t* remoteValue) {
	if (!link || !remoteValue || !atomic_load(&link->connected)) return false;
	uint16_t payload = htons(localValue);
	if (!mPeerLinkSend(link, mPEER_MSG_SIO_MULTI, &payload, sizeof(payload))) return false;

	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 10;
	pthread_mutex_lock(&link->lock);
	int waitResult = 0;
	while (!link->sioQueueCount && !link->sioQueueOverflow && atomic_load(&link->running) && waitResult == 0)
		waitResult = pthread_cond_timedwait(&link->sioCond, &link->lock, &deadline);
	bool received = link->sioQueueCount > 0 && !link->sioQueueOverflow;
	if (received) {
		*remoteValue = link->sioQueue[link->sioQueueRead];
		link->sioQueueRead = (link->sioQueueRead + 1) % 32;
		--link->sioQueueCount;
	}
	pthread_mutex_unlock(&link->lock);
	return received;
}

void mPeerLinkStop(struct mPeerLink* link) {
	if (!link || !link->threadStarted) return;
	atomic_store(&link->running, false);
	pthread_mutex_lock(&link->lock);
	pthread_cond_broadcast(&link->sioCond);
	pthread_mutex_unlock(&link->lock);
	if (link->socketFd >= 0) shutdown(link->socketFd, SHUT_RDWR);
	if (link->listenFd >= 0) shutdown(link->listenFd, SHUT_RDWR);
	pthread_join(link->thread, NULL);
	link->threadStarted = false;
	link->socketFd = -1;
	link->listenFd = -1;
	free(link->address);
	link->address = NULL;
}

void mPeerLinkDestroy(struct mPeerLink* link) {
	if (!link) return;
	mPeerLinkStop(link);
	pthread_cond_destroy(&link->sioCond);
	pthread_mutex_destroy(&link->sendLock);
	pthread_mutex_destroy(&link->lock);
	free(link);
}
