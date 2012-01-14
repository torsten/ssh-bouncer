/* Originally written by Torsten Becker <torsten.becker@gmail.com> in 2012 */

#include <arpa/inet.h>
#include <assert.h>
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


/* "nobody" */
#ifndef __APPLE__
static const int sb_userid = 65534;
#else
static const int sb_userid = -2;
#endif

static const char * sb_chroot = "/var/empty";

struct sb_listen_config_t
{
    unsigned short port;
    const char * version_string;
};

static struct sb_listen_config_t sb_listen_config[] = {
    {22,   "SSH-2.0-OpenSSH_4.5p1 FreeBSD-20061110\n"},
    {222,  "SSH-2.0-OpenSSH_5.4p1 Debian-5\n"},
    {2222, "SSH-2.0-OpenSSH_5.3\n"},
};

#define SB_PERR_AND_EXIT(MSG) { perror(MSG); exit(1); }

enum {
    sb_ipv4 = 1,
    sb_ipv6 = 0
};

int sb_bound_socket(unsigned short port, int ip_version)
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
            SB_PERR_AND_EXIT("Creating IPv4 socked failed")
        
        server_addr.sin.sin_family = AF_INET;
        server_addr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin.sin_port = htons(port);
        
        if(bind(sock, (struct sockaddr*)&server_addr,
                sizeof(struct sockaddr_in)) < 0)
            SB_PERR_AND_EXIT("Binding IPv4 socket failed")
        
        if(listen(sock, 20) < 0)
            SB_PERR_AND_EXIT("Listening on IPv4 socket failed")
    }
    else {
        sock = socket(PF_INET6, SOCK_STREAM, 0);
        if(sock < 0)
            SB_PERR_AND_EXIT("Creating IPv6 socked failed")
        
        server_addr.sin6.sin6_family = AF_INET6;
        server_addr.sin6.sin6_addr = in6addr_any;
        server_addr.sin6.sin6_port = htons(port);
        
        if(bind(sock, (struct sockaddr*)&server_addr,
                sizeof(struct sockaddr_in6)) < 0)
            SB_PERR_AND_EXIT("Binding IPv6 socket failed")
        
        if(listen(sock, 20) < 0)
            SB_PERR_AND_EXIT("Listening on IPv6 socket failed")
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
        SB_PERR_AND_EXIT("select failed")
    
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
    
    int listen_sockets[num_configs*2];
    
    for(size_t i = 0; i < num_configs; ++i) {
        listen_sockets[i*2] = sb_bound_socket(sb_listen_config[i].port,
                                              sb_ipv4);
        listen_sockets[i*2 + 1] = sb_bound_socket(sb_listen_config[i].port,
                                                  sb_ipv6);
    }
    
    if(chroot(sb_chroot) < 0)
        SB_PERR_AND_EXIT("chroot failed")
    
    if(setgid(sb_userid) < 0)
        SB_PERR_AND_EXIT("Setting group id failed")
    
    if(setuid(sb_userid) < 0)
        SB_PERR_AND_EXIT("Setting user id failed")
    
    printf("[+] Listening to max %zu clients on %zu sockets.\n",
           max_num_clients, num_sockets);
    
    // switch(fork()) {
    //     case -1:
    //         SB_PERR_AND_EXIT("fork failed")
    //     case 0:
    //         break;
    //     default:
    //         return 0;
    // }
    
    fd_set fds;
    struct timeval tv;
    int ready_fds;
    int max_fd = 0;
    
    int connected_clients[max_num_clients];
    size_t num_connected_clients = 0;
    
    for(;;) {
        FD_ZERO(&fds);
        for(size_t i = 0; i < num_sockets; ++i) {
            FD_SET(listen_sockets[i], &fds);
            if(listen_sockets[i] > max_fd)
                max_fd = listen_sockets[i];
        }
        // walk over connected_clients and add those...
        ++max_fd;
        
        tv.tv_sec = 10;
        tv.tv_usec = 0;
        ready_fds = select(max_fd, &fds, NULL, NULL, &tv);
        
        if(ready_fds == -1)
            SB_PERR_AND_EXIT("select failed")
        
        else if(ready_fds) {
            for(size_t i = 0; i < num_sockets; ++i) {
                if(FD_ISSET(listen_sockets[i], &fds)) {
                    int client = sb_verbose_accept(listen_sockets[i]);
                    const char * version = sb_listen_config[i/2].version_string;
                    
                    if(write(client, version, strlen(version)) < 0) {
                        perror("Error writing to client");
                        close(client);
                    }
                    else {
                        assert(num_connected_clients < 20);
                        connected_clients[num_connected_clients++] = client;
                    }
                }
            }
            // walk over connected_clients and close those
        }
    }
    
    return 0;
}
