/*
 * Copyright (C) 2012 Torsten Becker <torsten.becker@gmail.com>.
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER 
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * ssh-bouncer.c, created on 2012-Jan-14.
 */

#include <stdlib.h>


// ----- Config values (who needs config files?) ------------------------------

// Configure the user which this service will run as, default is "nobody":
#ifndef __APPLE__
static const int sb_userid = 65534;
#else
static const int sb_userid = -2;
#endif

// Configure where to change the root directory to:
static const char * sb_chroot = "/var/empty";

struct sb_listen_config_t
{
    unsigned short port;
    const char * version_string;
};

// Define on which ports the daemon should listen and which version it
// will pretend to be running on that port:
static struct sb_listen_config_t sb_listen_config[] = {
    {22,   "SSH-2.0-OpenSSH_4.5p1 FreeBSD-20061110\n"},
    {222,  "SSH-2.0-OpenSSH_5.4p1 Debian-5\n"},
    {2222, "SSH-2.0-OpenSSH_5.3\n"},
};

// The maximum number of clients that will be kept hanging, if more clients
// connect, older ones will be dropped.
static size_t sb_num_clients = 100;



// ----- SSH Bouncer Program --------------------------------------------------

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>

// Linux needs __USE_BSD to get chroot(2) from here...
#define __USE_BSD
#include <unistd.h>


// Prints the error string for the last error and exits the whole program
#define SB_PRINT_ERR_DIE(MSG) { perror("[!] "MSG); exit(1); }

int sb_bound_socket(unsigned short port)
{
    int sock;
    struct sockaddr_in6 server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    
    sock = socket(PF_INET6, SOCK_STREAM, 0);
    if(sock < 0)
        SB_PRINT_ERR_DIE("Creating socked failed")
    
    // Setting IPV6_V6ONLY to false (0) makes this a universal socket
    // which will listen for IPv4 and IPv6 connections.
    int fals = 0;
    if(setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &fals, sizeof(fals)) < 0)
        SB_PRINT_ERR_DIE("setsockopt(IPV6_V6ONLY) failed")
    
    server_addr.sin6_family = AF_INET6;
    server_addr.sin6_addr = in6addr_any;
    server_addr.sin6_port = htons(port);
    
    if(bind(sock, (struct sockaddr*)&server_addr,
            sizeof(struct sockaddr_in6)) < 0)
        SB_PRINT_ERR_DIE("bind() failed")
    
    if(listen(sock, 20) < 0)
        SB_PRINT_ERR_DIE("listen() failed")
    
    return sock;
}

int sb_verbose_accept(int listen_socket)
{
    union {
        struct sockaddr_in sin;
        struct sockaddr_in6 sin6;
    } client_addr; 
    socklen_t client_addr_len = sizeof(client_addr);
    char client_addr_str[INET6_ADDRSTRLEN];
    
    int client = accept(listen_socket, (struct sockaddr *)&client_addr,
                        &client_addr_len);
    if(client < 0)
        SB_PRINT_ERR_DIE("accept() failed")
    
    if(fcntl(client, F_SETFL, O_NONBLOCK) < 0)
        SB_PRINT_ERR_DIE("Setting nonblocking I/O failed")
    
    // TODO: Send a email here or so
    printf("Connection from %s\n",
           inet_ntop(client_addr.sin.sin_family,
                     client_addr.sin.sin_family == AF_INET
                        ? (const void *)&client_addr.sin.sin_addr
                        : (const void *)&client_addr.sin6.sin6_addr,
                     client_addr_str, sizeof(client_addr_str)));
    
    return client;
}


int main(int argc, char **argv)
{
    const size_t num_configs = sizeof(sb_listen_config) /
                               sizeof(struct sb_listen_config_t);
    const size_t max_num_clients = FD_SETSIZE - num_configs;
    
    if(sb_num_clients > max_num_clients) {
        fprintf(stderr, "[!] Configured number of clients (%zu) is larger "
                        "than maximum available number of clients (%zu).\n",
                sb_num_clients, max_num_clients);
        return 1;
    }
    
    int listen_sockets[num_configs];
    for(size_t i = 0; i < num_configs; ++i)
        listen_sockets[i] = sb_bound_socket(sb_listen_config[i].port);
    
    if(chroot(sb_chroot) < 0)
        SB_PRINT_ERR_DIE("chroot() failed")
    
    if(setgid(sb_userid) < 0)
        SB_PRINT_ERR_DIE("Setting group id failed")
    
    if(setuid(sb_userid) < 0)
        SB_PRINT_ERR_DIE("Setting user id failed")
    
    printf("[+] Listening on %zu sockets for maximum %zu clients.\n",
           num_configs, sb_num_clients);
    
    pid_t daemon_pid = fork();
    switch(daemon_pid) {
        case -1:
            SB_PRINT_ERR_DIE("fork() failed")
        case 0:
            break;
        default:
            printf("[+] Daemon is PID %d\n", daemon_pid);
            return 0;
    }
    
    fd_set all_sockets;
    struct timeval tv;
    int ready_sockets;
    int max_fd = 0;
    
    // Keep all clients in this stupid simple ring buffer, -1 means
    // it's a unused slot, else it is a valid socket fd.
    int client_ring[sb_num_clients];
    for(size_t i = 0; i < sb_num_clients; ++i)
        client_ring[i] = -1;
    
    size_t next_free_client = 0;
    
    char nirvana[2048];
    
    
    for(;;) {
        FD_ZERO(&all_sockets);
        for(size_t i = 0; i < num_configs; ++i) {
            FD_SET(listen_sockets[i], &all_sockets);
            if(listen_sockets[i] > max_fd)
                max_fd = listen_sockets[i];
        }
        for(size_t i = 0; i < sb_num_clients; ++i) {
            int client = client_ring[i];
            
            if(client != -1) {
                FD_SET(client, &all_sockets);
                if(client > max_fd)
                    max_fd = client;
            }
        }
        ++max_fd;
        
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ready_sockets = select(max_fd, &all_sockets, NULL, NULL, &tv);
        
        if(ready_sockets == -1)
            SB_PRINT_ERR_DIE("select() failed")
        
        else if(ready_sockets) {
            // Walk over all listen sockets and accept new clients
            for(size_t i = 0; i < num_configs; ++i) {
                if(FD_ISSET(listen_sockets[i], &all_sockets)) {
                    int client = sb_verbose_accept(listen_sockets[i]);
                    const char * version = sb_listen_config[i/2].version_string;
                    
                    // Try to write the fake version string...
                    if(write(client, version, strlen(version)) < 0) {
                        perror("[!] Writing version to client failed");
                        close(client);
                    }
                    // ... if writing succeeds, put client into the ring buffer
                    // of all connected clients.
                    else {
                        client_ring[next_free_client] = client;
                        
                        next_free_client = (next_free_client + 1) %
                                           sb_num_clients;
                        
                        if(client_ring[next_free_client] != -1) {
                            close(client_ring[next_free_client]);
                            client_ring[next_free_client] = -1;
                        }
                    }
                }
            }
            // Walk over all connected clients and see if there is any new
            // data or if the clients is disconnected
            for(size_t i = 0; i < sb_num_clients; ++i) {
                int client = client_ring[i];
                
                if(client != -1) {
                    if(FD_ISSET(client, &all_sockets)) {
                        ssize_t did_read;
                        
                        // Read data into nirvana...
                        for(;;) {
                            did_read = read(client, nirvana, sizeof(nirvana));
                            // A error occured, hmm...
                            if(did_read < 0) {
                                // .. AH, no new data available
                                if(errno == EAGAIN) {
                                    // printf("EAGAIN\n");
                                    break;
                                }
                                perror("[!] Read error");
                                break;
                            }
                            // Reading 0 means, the client disconnected
                            else if(did_read == 0) {
                                // printf("NOTHING: close\n");
                                close(client);
                                client_ring[i] = -1;
                                break;
                            }
                            // Reading less than buffer size means, there was
                            // no more data available...
                            else if(did_read < sizeof(nirvana)) {
                                // printf("READ %zu\n", did_read);
                                break;
                            }
                            // Reading exact data means, there will be more
                            // or EAGAIN, this case is the loop-default...
                            // else {
                            //     printf("READ EXACT\n");
                            // }
                        
                        } // end-less read loop
                    } // if fd is in set
                } // client != -1
            } // for all connected clients
        } // if socket is ready
    } // infinite loop
    
    return 0;
}
