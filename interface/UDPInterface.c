/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "exception/Except.h"
#include "interface/Interface.h"
#include "interface/UDPInterface.h"
#include "interface/UDPInterface_pvt.h"
#include "memory/Allocator.h"
#include "net/InterfaceController.h"
#include "util/Assert.h"
#include "util/Errno.h"
#include "wire/Message.h"
#include "wire/Error.h"

#ifdef WIN32
    #include <winsock.h>
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
#endif

#include <sys/types.h>
#include <event2/event.h>


#define MAX_INTERFACES 256


#define EFFECTIVE_KEY_SIZE \
    ((InterfaceController_KEY_SIZE > sizeof(struct sockaddr_in)) \
        ? sizeof(struct sockaddr_in) : InterfaceController_KEY_SIZE)

static inline void sockaddrForKey(struct sockaddr_in* sockaddr,
                                  uint8_t key[InterfaceController_KEY_SIZE],
                                  struct UDPInterface_pvt* udpif)
{
    if (EFFECTIVE_KEY_SIZE < sizeof(struct sockaddr_in)) {
        Bits_memset(sockaddr, 0, sizeof(struct sockaddr_in));
    }
    Bits_memcpyConst(sockaddr, key, EFFECTIVE_KEY_SIZE);
}

static inline void keyForSockaddr(uint8_t key[InterfaceController_KEY_SIZE],
                                  struct sockaddr_in* sockaddr,
                                  struct UDPInterface_pvt* udpif)
{
    if (EFFECTIVE_KEY_SIZE < InterfaceController_KEY_SIZE) {
        Bits_memset(key, 0, InterfaceController_KEY_SIZE);
    }
    Bits_memcpyConst(key, sockaddr, EFFECTIVE_KEY_SIZE);
}

static uint8_t sendMessage(struct Message* message, struct Interface* iface)
{
    struct UDPInterface_pvt* context = iface->senderContext;
    Assert_true(&context->pub.generic == iface);

    struct sockaddr_in sin;
    sockaddrForKey(&sin, message->bytes, context);
    Bits_memcpyConst(&sin, message->bytes, InterfaceController_KEY_SIZE);
    Message_shift(message, -InterfaceController_KEY_SIZE);

    if (sendto(context->socket,
               message->bytes,
               message->length,
               0,
               (struct sockaddr*) &sin,
               context->addrLen) < 0)
    {
        switch (Errno_get()) {
            case Errno_EMSGSIZE:
                return Error_OVERSIZE_MESSAGE;

            case Errno_ENOBUFS:
            case Errno_EAGAIN:
                return Error_LINK_LIMIT_EXCEEDED;

            default:;
                Log_info(context->logger, "Got error sending to socket [%s]",
                         Errno_getString());
        }
    }
    return 0;
}

/**
 * Release the event used by this module.
 *
 * @param vevent a void pointer cast of the event structure.
 */
static void freeEvent(void* vevent)
{
    event_del((struct event*) vevent);
    event_free((struct event*) vevent);
}

static void handleEvent(evutil_socket_t socket, short eventType, void* vcontext)
{
    struct UDPInterface_pvt* context = (struct UDPInterface_pvt*) vcontext;

    struct Message message = {
        .bytes = context->messageBuff + UDPInterface_PADDING,
        .padding = UDPInterface_PADDING,
        .length = UDPInterface_MAX_PACKET_SIZE
    };

    struct sockaddr_storage addrStore;
    Bits_memset(&addrStore, 0, sizeof(struct sockaddr_storage));
    ev_socklen_t addrLen = sizeof(struct sockaddr_storage);

    // Start writing InterfaceController_KEY_SIZE after the beginning,
    // keyForSockaddr() will write the key there.
    int rc = recvfrom(socket,
                      message.bytes + InterfaceController_KEY_SIZE,
                      message.length - InterfaceController_KEY_SIZE,
                      0,
                      (struct sockaddr*) &addrStore,
                      &addrLen);

    if (addrLen != context->addrLen) {
        return;
    }
    if (rc < 0) {
        return;
    }
    message.length = rc + InterfaceController_KEY_SIZE;

    keyForSockaddr(message.bytes, (struct sockaddr_in*) &addrStore, context);

    context->pub.generic.receiveMessage(&message, &context->pub.generic);
}

int UDPInterface_beginConnection(const char* address,
                                 uint8_t cryptoKey[32],
                                 String* password,
                                 struct UDPInterface* udp)
{
    struct UDPInterface_pvt* udpif = (struct UDPInterface_pvt*) udp;
    struct sockaddr_storage addr;
    ev_socklen_t addrLen = sizeof(struct sockaddr_storage);
    Bits_memset(&addr, 0, addrLen);
    if (evutil_parse_sockaddr_port(address, (struct sockaddr*) &addr, (int*) &addrLen)) {
        return UDPInterface_beginConnection_BAD_ADDRESS;
    }
    if (addrLen != udpif->addrLen) {
        return UDPInterface_beginConnection_ADDRESS_MISMATCH;
    }

    uint8_t key[InterfaceController_KEY_SIZE];
    keyForSockaddr(key, (struct sockaddr_in*) &addr, udpif);
    int ret = udpif->ic->insertEndpoint(key, cryptoKey, password, &udpif->pub.generic, udpif->ic);
    switch(ret) {
        case 0:
            return 0;

        case InterfaceController_registerInterface_BAD_KEY:
            return UDPInterface_beginConnection_BAD_KEY;

        case InterfaceController_registerInterface_OUT_OF_SPACE:
            return UDPInterface_beginConnection_OUT_OF_SPACE;

        default:
            return UDPInterface_beginConnection_UNKNOWN_ERROR;
    }
}

struct UDPInterface* UDPInterface_new(struct event_base* base,
                                      const char* bindAddr,
                                      struct Allocator* allocator,
                                      struct Except* exHandler,
                                      struct Log* logger,
                                      struct InterfaceController* ic)
{
    struct UDPInterface_pvt* context = Allocator_malloc(allocator, sizeof(struct UDPInterface_pvt));
    Bits_memcpyConst(context, (&(struct UDPInterface_pvt) {
        .pub = {
            .generic = {
                .sendMessage = sendMessage,
                .senderContext = context,
                .allocator = allocator
            },
        },
        .logger = logger,
        .ic = ic
    }), sizeof(struct UDPInterface_pvt));

    int addrFam;
    if (bindAddr != NULL) {
        context->addrLen = sizeof(struct sockaddr_storage);
        if (0 != evutil_parse_sockaddr_port(bindAddr,
                                            (struct sockaddr*) &context->addr,
                                            (int*) &context->addrLen))
        {
            Except_raise(exHandler, UDPInterface_new_PARSE_ADDRESS_FAILED,
                         "failed to parse address");
        }
        addrFam = context->addr.ss_family;

        // This is because the key size is only 8 bytes.
        // Expanding the key size just for IPv6 doesn't make a lot of sense
        // when ethernet, 802.11 and ipv4 are ok with a shorter key size
        if (context->addr.ss_family != AF_INET || context->addrLen != sizeof(struct sockaddr_in)) {
            Except_raise(exHandler, UDPInterface_new_PROTOCOL_NOT_SUPPORTED,
                         "only IPv4 is supported");
        }

    } else {
        addrFam = AF_INET;
        context->addrLen = sizeof(struct sockaddr);
    }

    context->socket = socket(addrFam, SOCK_DGRAM, 0);
    if (context->socket == -1) {
        Except_raise(exHandler, UDPInterface_new_BIND_FAILED, "call to socket() failed.");
    }

    if (bindAddr != NULL) {
        if (bind(context->socket, (struct sockaddr*) &context->addr, context->addrLen)) {
            Except_raise(exHandler, UDPInterface_new_BIND_FAILED, "call to bind() failed.");
        }
    }

    if (getsockname(context->socket, (struct sockaddr*) &context->addr, &context->addrLen)) {
        enum Errno err = Errno_get();
        EVUTIL_CLOSESOCKET(context->socket);
        Except_raise(exHandler, -1, "Failed to get socket name [%s]", Errno_strerror(err));
    }

    evutil_make_socket_nonblocking(context->socket);

    context->incomingMessageEvent =
        event_new(base, context->socket, EV_READ | EV_PERSIST, handleEvent, context);

    if (!context->incomingMessageEvent || event_add(context->incomingMessageEvent, NULL)) {
        Except_raise(exHandler, UDPInterface_new_FAILED_CREATING_EVENT,
                     "failed to create UDPInterface event");
    }

    allocator->onFree(freeEvent, context->incomingMessageEvent, allocator);

    ic->registerInterface(&context->pub.generic, ic);

    return &context->pub;
}
