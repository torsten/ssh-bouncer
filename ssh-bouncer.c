/* Originally written by Torsten Becker <torsten.becker@gmail.com> in 2012 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>


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

// Prints the error string for the last error and exits the whole program
#define SB_PRINT_ERR_DIE(MSG) { perror("[!] "MSG); exit(1); }

enum sb_ip_version {
    sb_ipv4 = 1,
    sb_ipv6 = 0,
};

int sb_bound_socket(unsigned short port, enum sb_ip_version ip_version)
{
    int sock;
    union {
        struct sockaddr_in sin;
        struct sockaddr_in6 sin6;
    } server_addr; 
    memset(&server_addr, 0, sizeof(server_addr));
    
    if(ip_version == sb_ipv4) {
        sock = socket(PF_INET, SOCK_STREAM, 0);
        if(sock < 0)
            SB_PRINT_ERR_DIE("Creating IPv4 socked failed")
        
        server_addr.sin.sin_family = AF_INET;
        server_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin.sin_port = htons(port);
        
        if(bind(sock, (struct sockaddr*)&server_addr,
                sizeof(struct sockaddr_in)) < 0)
            SB_PRINT_ERR_DIE("Binding IPv4 socket failed")
        
        if(listen(sock, 20) < 0)
            SB_PRINT_ERR_DIE("Listening on IPv4 socket failed")
    }
    else {
        sock = socket(PF_INET6, SOCK_STREAM, 0);
        if(sock < 0)
            SB_PRINT_ERR_DIE("Creating IPv6 socked failed")
        
        server_addr.sin6.sin6_family = AF_INET6;
        server_addr.sin6.sin6_addr = in6addr_any;
        server_addr.sin6.sin6_port = htons(port);
        
        if(bind(sock, (struct sockaddr*)&server_addr,
                sizeof(struct sockaddr_in6)) < 0)
            SB_PRINT_ERR_DIE("Binding IPv6 socket failed")
        
        if(listen(sock, 20) < 0)
            SB_PRINT_ERR_DIE("Listening on IPv6 socket failed")
    }
    
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
    const size_t num_sockets = num_configs * 2;
    const size_t max_num_clients = FD_SETSIZE - num_sockets;
    
    if(sb_num_clients > max_num_clients) {
        fprintf(stderr, "[!] Configured number of clients (%zu) is larger "
                        "than maximum available number of clients (%zu).\n",
                sb_num_clients, max_num_clients);
        return 1;
    }
    
    int listen_sockets[num_configs*2];
    for(size_t i = 0; i < num_configs; ++i) {
        listen_sockets[i*2] = sb_bound_socket(sb_listen_config[i].port,
                                              sb_ipv4);
        listen_sockets[i*2 + 1] = sb_bound_socket(sb_listen_config[i].port,
                                                  sb_ipv6);
    }
    
    if(chroot(sb_chroot) < 0)
        SB_PRINT_ERR_DIE("chroot() failed")
    
    if(setgid(sb_userid) < 0)
        SB_PRINT_ERR_DIE("Setting group id failed")
    
    if(setuid(sb_userid) < 0)
        SB_PRINT_ERR_DIE("Setting user id failed")
    
    printf("[+] Listening on %zu sockets for max. %zu clients.\n",
           num_sockets, max_num_clients);
    
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
    
    for(;;) {
        FD_ZERO(&all_sockets);
        for(size_t i = 0; i < num_sockets; ++i) {
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
            for(size_t i = 0; i < num_sockets; ++i) {
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
                char buf[2048];
                
                if(client != -1) {
                    if(FD_ISSET(client, &all_sockets)) {
                        ssize_t did_read;
                        
                        // Read data into nirvana...
                        for(;;) {
                            did_read = read(client, buf, sizeof(buf));
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
                            // Reading less than buf size means, there was
                            // no more data available...
                            else if(did_read < sizeof(buf)) {
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
