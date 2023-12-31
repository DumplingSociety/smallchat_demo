/* smallchat.c -- Read clients input, send to all the other connected clients.
 *
 * Copyright (c) 2023, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <sys/select.h>
#include <time.h>
/* ============================ Data structures =================================
 * The minimal stuff we can afford to have. This example must be simple
 * even for people that don't know a lot of C.
 * =========================================================================== */

#define MAX_CLIENTS 1000 // This is actually the higher file descriptor.
#define MAX_NICK_LEN 32
#define SERVER_PORT 7711

/* This structure represents a connected client. There is very little
 * info about it: the socket descriptor and the nick name, if set, otherwise
 * the first byte of the nickname is set to 0 if not set.
 * The client can set its nickname with /nick <nickname> command. */
struct client
{
    int fd;     // Client socket.
    char *nick; // Nickname of the client.
    char *readbuf;   // Dynamic buffer for partial reads.
    size_t buflen;   // Length of the buffer.
    size_t bufused;  // How much of the buffer is used.
};

/* This global structure encasulates the global state of the chat. */
struct chatState
{
    int serversock;                      // Listening server socket.
    int numclients;                      // Number of connected clients right now.
    int maxclient;                       // The greatest 'clients' slot populated.
    struct client *clients[MAX_CLIENTS]; // Clients are set in the corresponding
                                         // slot of their socket descriptor.
};

struct chatState *Chat; // Initialized at startup.

/* ======================== Low level networking stuff ==========================
 * Here you will find basic socket stuff that should be part of
 * a decent standard C library, but you know... there are other
 * crazy goals for the future of C: like to make the whole language an
 * Undefined Behavior.
 * =========================================================================== */

/* Create a TCP socket lisetning to 'port' ready to accept connections. */
int createTCPServer(int port)
{
    int s, yes = 1; // s is the server socket, yes is used to reuse the port.
    struct sockaddr_in sa;
    // create a socket. it uses TCP (SOCK_STREAM ensure that data is not lost or duplicated) and IPv4
    if ((s = socket(AF_INET, SOCK_STREAM, 0)) == -1)
        return -1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)); // Best effort to lose "Address already in use" error message.

    memset(&sa, 0, sizeof(sa)); // Zero the structure.
    sa.sin_family = AF_INET; // IPv4.
    sa.sin_port = htons(port); // Host to network endian conversion.
    sa.sin_addr.s_addr = htonl(INADDR_ANY); // Accept connections from any addr.

    /*bind the socket to the address and port
     assigns the address specified by addr to the socket referred to by the file descriptor sockfd
     s -- the sockfd (socket)
     sa -- the address to bind to
     sizeof(sa) -- the size of the address */
    if (bind(s, (struct sockaddr *)&sa, sizeof(sa)) == -1 || 
        listen(s, 511) == -1)
    {
        close(s);
        return -1;
    }
    return s;
}

/* Set the specified socket in non-blocking mode, with no delay flag. */
int socketSetNonBlockNoDelay(int fd)
{
    int flags, yes = 1;// reuse the port

    /* Set the socket nonblocking.
     * Note that fcntl(2) for F_GETFL and F_SETFL can't be
     * interrupted by a signal. */
    if ((flags = fcntl(fd, F_GETFL)) == -1)
        return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1)
        return -1;

    /* This is best-effort. No need to check for errors; lose the pesky "Address already in use" error message */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    return 0;
}

/* If the listening socket signaled there is a new connection ready to
 * be accepted, we accept(2) it and return -1 on error or the new client
 * socket on success. */
int acceptClient(int server_socket)
{
    int s; // s is the client socket.

    while (1)
    {
        struct sockaddr_in sa;
        socklen_t slen = sizeof(sa); // get the size of the address
        s = accept(server_socket, (struct sockaddr *)&sa, &slen); // accept a new client connection, s is the client socket
        if (s == -1)
        {
            if (errno == EINTR)
                continue; /* Try again. */
            else
                return -1;
        }
        break;
    }
    return s;
}

/* We also define an allocator that always crashes on out of memory: you
 * will discover that in most programs designed to run for a long time, that
 * are not libraries, trying to recover from out of memory is often futile
 * and at the same time makes the whole program terrible. */
void *chatMalloc(size_t size)
{
    void *ptr = malloc(size);
    if (ptr == NULL)
    {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}

/* Also aborting realloc(). */
void *chatRealloc(void *ptr, size_t size)
{
    ptr = realloc(ptr, size);
    if (ptr == NULL)
    {
        perror("Out of memory");
        exit(1);
    }
    return ptr;
}
/* desc : handle direct message
sender -- the client who sent the DM
target_nick -- the target client's name
message -- the message to be sent*/
void handleDirectMessage(struct client *sender, char *target_nick, char *message) {
    // Check if the target user is found
    for (int j = 0; j <= Chat->maxclient; j++) {
        struct client *target = Chat->clients[j]; // Get the client
        if (target && strcmp(target->nick, target_nick) == 0) { // Check if the client is found and the nick matches
            // Construct the direct message
            char dm[512]; // Make sure this is large enough
            snprintf(dm, sizeof(dm), "DM from %s: %s", sender->nick, message);
            // Send the DM to the target client only
            write(target->fd, dm, strlen(dm));
            return; // DM sent, return early
        }
    }
    // If we reach here, the target user was not found
    char *errmsg = "User not found\n";
    write(sender->fd, errmsg, strlen(errmsg));
}


/* ====================== Small chat core implementation ========================
 * Here the idea is very simple: we accept new connections, read what clients
 * write us and fan-out (that is, send-to-all) the message to everybody
 * with the exception of the sender. And that is, of course, the most
 * simple chat system ever possible.
 * =========================================================================== */

/* Create a new client bound to 'fd'. This is called when a new client
 * connects. As a side effect updates the global Chat state. */
struct client *createClient(int fd) {
    char nick[32]; // Used to create an initial nick for the user.
    int nicklen = snprintf(nick, sizeof(nick), "user:%d", fd);
    struct client *c = chatMalloc(sizeof(*c));
    socketSetNonBlockNoDelay(fd); // Pretend this will not fail.
    c->fd = fd;
    c->nick = chatMalloc(nicklen + 1); // +1 because of the null term.
    c->readbuf = chatMalloc(256);      // Initial buffer size.
    c->buflen = 256;
    c->bufused = 0;
    memcpy(c->nick, nick, nicklen);
    assert(Chat->clients[c->fd] == NULL); // This should be available.
    Chat->clients[c->fd] = c;
    /* We need to update the max client set if needed. */
    if ((c->fd) > (Chat->maxclient))
        Chat->maxclient = c->fd;
    Chat->numclients++;
    return c;
}

/* Free a client, associated resources, and unbind it from the global
 * state in Chat. */
void freeClient(struct client *c)
{
    free(c->nick);
    close(c->fd);
    Chat->clients[c->fd] = NULL;
    Chat->numclients--;
    if (Chat->maxclient == c->fd)
    {
        /* Ooops, this was the max client set. Let's find what is
         * the new highest slot used. */
        int j;
        for (j = Chat->maxclient - 1; j >= 0; j--)
        {
            if (Chat->clients[j] != NULL)
                Chat->maxclient = j;
            break;
        }
        if (j == -1)
            Chat->maxclient = -1; // We no longer have clients.
    }
    free(c);
}

/* Allocate and init the global stuff. */
void initChat(void)
{
    Chat = chatMalloc(sizeof(*Chat));
    memset(Chat, 0, sizeof(*Chat));
    /* No clients at startup, of course. */
    Chat->maxclient = -1;
    Chat->numclients = 0;

    /* Create our listening socket, bound to the given port. This
     * is where our clients will connect. */
    Chat->serversock = createTCPServer(SERVER_PORT);
    if (Chat->serversock == -1)
    {
        perror("Creating listening socket");
        exit(1);
    }
}

/* Send the specified string to all connected clients but the one
 * having as socket descriptor 'excluded'. If you want to send something
 * to every client just set excluded to an impossible socket: -1. */
void sendMsgToAllClientsBut(int excluded, char *s, size_t len)
{
    // get the current time
    time_t rawtime;
    struct tm *timeinfo;
    char time_buffer[20]; // for hold the time string

    time(&rawtime);                                                     //
    timeinfo = localtime(&rawtime);                                     // convert to localtime
    strftime(time_buffer, sizeof(time_buffer), "[%H:%M:%S]", timeinfo); // format the time

    // construct the new message with the timestamp
    char msg_with_time[256];
    snprintf(msg_with_time, sizeof(msg_with_time), "%s %s", time_buffer, s);
    len = strlen(msg_with_time);
    for (int j = 0; j <= Chat->maxclient; j++)
    {
        if (Chat->clients[j] == NULL ||
            Chat->clients[j]->fd == excluded)
            continue;

        /* Important: we don't do ANY BUFFERING. We just use the kernel
         * socket buffers. If the content does not fit, we don't care.
         * This is needed in order to keep this program simple. */
        write(Chat->clients[j]->fd, msg_with_time, len); // send the message to the client
    }
}

/* The main() function implements the main chat logic:
 * 1. Accept new clients connections if any.
 * 2. Check if any client sent us some new message.
 * 3. Send the message to all the other clients. */
int main(void)
{
    initChat();

    while (1)
    {
        fd_set readfds;
        struct timeval tv;
        int retval;

        FD_ZERO(&readfds);
        /* When we want to be notified by select() that there is
         * activity? If the listening socket has pending clients to accept
         * or if any other client wrote anything. */
        FD_SET(Chat->serversock, &readfds);

        for (int j = 0; j <= Chat->maxclient; j++)
        {
            if (Chat->clients[j])
                FD_SET(j, &readfds); // FD_SET is a macro that sets the bit for the file descriptor j in the file descriptor set readfds. this add the client socket to the readfds set
        }

        /* Set a timeout for select(), see later why this may be useful
         * in the future (not now). */
        tv.tv_sec = 1; // 1 sec timeout
        tv.tv_usec = 0;

        /* Select wants as first argument the maximum file descriptor
         * in use plus one. It can be either one of our clients or the
         * server socket itself. */
        int maxfd = Chat->maxclient;
        if (maxfd < Chat->serversock)
            maxfd = Chat->serversock;
        retval = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (retval == -1)
        {
            perror("select() error");
            exit(1);
        }
        else if (retval)
        {

            /* If the listening socket is "readable", it actually means
             * there are new clients connections pending to accept. */
            if (FD_ISSET(Chat->serversock, &readfds)) // This is a macro that returns true if the bit for the file descriptor Chat->serversock is set in the file descriptor set readfds.
            {
                int fd = acceptClient(Chat->serversock);
                struct client *c = createClient(fd);
                /* Send a welcome message. */
                char *welcome_msg =
                    "Welcome to Simple Chat! "
                    "Use /nick <nick> to set your nick.\n";
                write(c->fd, welcome_msg, strlen(welcome_msg));
                printf("Connected client fd=%d\n", fd);
            }

            /* Here for each connected client, check if there are pending
             * data the client sent us. */
            char readbuf[256];
            for (int j = 0; j <= Chat->maxclient; j++)
            {
                if (Chat->clients[j] == NULL)
                    continue;
                if (FD_ISSET(j, &readfds)) // This is a macro that returns true if the bit for the file descriptor j is set in the file descriptor set readfds.
                {
                    /* Here we just hope that ther
                     * message waiting for us. But it is entirely possible
                     * that we read just half a message. In a normal program
                     * that is not designed to be that simple, we should try
                     * to buffer reads until the end-of-the-line is reached. */
                    int nread = read(j, readbuf, sizeof(readbuf) - 1);
                   
                    if (nread <= 0)
                    {
                        /* Error or short read means that the socket
                         * was closed. */
                        printf("Disconnected client fd=%d, nick=%s\n",
                               j, Chat->clients[j]->nick);
                        freeClient(Chat->clients[j]);
                    }
                    else
                    {
                        /* The client sent us a message. We need to
                         * relay this message to all the other clients
                         * in the chat. */
                        struct client *c = Chat->clients[j];
                        readbuf[nread] = 0;

                        /* If the user message starts with "/", we
                         * process it as a client command. So far
                         * only the /nick <newnick> command is implemented. */
                        if (readbuf[0] == '/')
                        {
                            /* Remove any trailing newline. */
                            char *p;
                            p = strchr(readbuf, '\r');
                            if (p)
                                *p = 0;
                            p = strchr(readbuf, '\n');
                            if (p)
                                *p = 0;
                            /* Check for an argument of the command, after
                             * the space. */
                            char *arg = strchr(readbuf, ' ');
                            if (arg)
                            {
                                *arg = 0; /* Terminate command name. */
                                arg++;    /* Argument is 1 byte after the space. */
                            }

                            if (!strcmp(readbuf, "/nick") && arg)
                            {
                                free(c->nick); // Free old nick.
                                int nicklen = strlen(arg); // New nick length.
                                c->nick = chatMalloc(nicklen + 1); // Allocate new nick.
                                memcpy(c->nick, arg, nicklen + 1); // Set new nick.
                            }
                            else if (!strcmp(readbuf, "/list"))
                            {
                                // list each client name 
                                char userlist[256];
                                char listmsg[256];
                                memset(userlist, 0, sizeof(userlist));
                                 for (int i = 0; i <= Chat->maxclient; i++) {
                                    if (Chat->clients[i]) {
                                        strcat(userlist, Chat->clients[i]->nick);
                                        strcat(userlist, "\n");
                                    }
                                }
                                write(c->fd, userlist, strlen(userlist)); // send the list to the client
                                // send the number of connected users to the client 
                                int msglen = snprintf(listmsg, sizeof(listmsg), "Number of connected users: %d\n", Chat->numclients);
                                write(c->fd, listmsg, msglen);
                            }
                            else if (!strcmp(readbuf, "/dm"))
                            {
                                char *target_nick = strtok(arg, " "); // Get the first token after "/dm" as the target nickname
                                char *message = strtok(NULL, ""); // Get the rest of the input as the message

                                // Check if we got a target nickname and a message
                                if (target_nick == NULL || message == NULL) {
                                    printf("Error: The format is /dm <nickname> <message>\n");
                                    continue; // Skip this iteration and wait for a new message
                                }
                                // Call a function to handle DM
                                handleDirectMessage(c, target_nick, message);
                            }
                            else
                            {
                                /* Unsupported command. Send an error. */
                                char *errmsg = "Unsupported command\n";
                                write(c->fd, errmsg, strlen(errmsg));
                            }
                        }
                        else
                        {
                            /* Create a message to send everybody (and show
                             * on the server console) in the form:
                             *   nick> some message. */
                            char msg[256];
                            int msglen = snprintf(msg, sizeof(msg),
                                                  "%s> %s", c->nick, readbuf);
                            printf("%s", msg);

                            /* Send it to all the other clients. */
                            sendMsgToAllClientsBut(j, msg, msglen);
                        }
                    }
                }
            }
        }
        else
        {
            /* Timeout occurred. We don't do anything right now, but in
             * general this section can be used to wakeup periodically
             * even if there is no clients activity. */
        }
    }
    return 0;
}
