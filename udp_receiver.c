//
//  udp_receiver.c
//  
//
//  Created by XueFei Yang on 2015-02-11.
//
//

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>

// headers for timestamp
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <poll.h>
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <linux/sockios.h>
#include <net/if.h>

#define BUFSIZE 100

static void printpacket(struct msghdr *msg, int res, int recvmsg_flags)
{
    struct sockaddr_in *from_addr = (struct sockaddr_in *)msg->msg_name;
    struct cmsghdr *cmsg;
    struct timeval tv;
    struct timespec ts;
    struct timeval now;

    gettimeofday(&now, 0);

    printf("%ld.%06ld: received %s data, %d bytes from %s, %zu bytes control messages\n",
           (long)now.tv_sec, (long)now.tv_usec,
           (recvmsg_flags & MSG_ERRQUEUE) ? "error" : "regular",
           res,
           inet_ntoa(from_addr->sin_addr),
           msg->msg_controllen);
    for (cmsg = CMSG_FIRSTHDR(msg);
         cmsg;
         cmsg = CMSG_NXTHDR(msg, cmsg)) {
        printf("   cmsg len %zu: ", cmsg->cmsg_len);
        switch (cmsg->cmsg_level) {
        case SOL_SOCKET:
            printf("SOL_SOCKET ");
            switch (cmsg->cmsg_type) {
            case SO_TIMESTAMP: {
                struct timeval *stamp =
                    (struct timeval *)CMSG_DATA(cmsg);
                printf("SO_TIMESTAMP %ld.%06ld",
                       (long)stamp->tv_sec,
                       (long)stamp->tv_usec);
                break;
            }
            case SO_TIMESTAMPNS: {
                struct timespec *stamp =
                    (struct timespec *)CMSG_DATA(cmsg);
                printf("SO_TIMESTAMPNS %ld.%09ld",
                       (long)stamp->tv_sec,
                       (long)stamp->tv_nsec);
                break;
            }
            case SO_TIMESTAMPING: {
                struct timespec *stamp =
                    (struct timespec *)CMSG_DATA(cmsg);
                printf("SO_TIMESTAMPING ");
                printf("SW %ld.%09ld ",
                       (long)stamp->tv_sec,
                       (long)stamp->tv_nsec);
                stamp++;
                /* skip deprecated HW transformed */
                stamp++;
                printf("HW raw %ld.%09ld",
                       (long)stamp->tv_sec,
                       (long)stamp->tv_nsec);
                break;
            }
            default:
                printf("unknown type %d", cmsg->cmsg_type);
                break;
            }
            break;
        default:
            printf("unknown level %d type %d",
                cmsg->cmsg_level,
                cmsg->cmsg_type);
            break;
        }
        printf("\n");
    }
}

static void recvpacket(int sock, int recvmsg_flags)
{
    char data[256];
    struct msghdr msg;
    struct iovec entry;
    struct sockaddr_in from_addr;
    struct {
        struct cmsghdr cm;
        char control[512];
    } control;
    int res;

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &entry;
    msg.msg_iovlen = 1;
    entry.iov_base = data;
    entry.iov_len = sizeof(data);
    msg.msg_name = (caddr_t)&from_addr;
    msg.msg_namelen = sizeof(from_addr);
    msg.msg_control = &control;
    msg.msg_controllen = sizeof(control);

    res = recvmsg(sock, &msg, recvmsg_flags|MSG_DONTWAIT);
    if (res < 0) {
        printf("%s %s: %s\n",
               "recvmsg",
               (recvmsg_flags & MSG_ERRQUEUE) ? "error" : "regular",
               strerror(errno));
    } else {
        printpacket(&msg, res, recvmsg_flags);
    }
}

static int hwts_init(int fd, const char *device)
{
    struct ifreq ifreq;
    struct hwtstamp_config cfg, req;
    int err;

    memset(&ifreq, 0, sizeof(ifreq));
    memset(&cfg, 0, sizeof(cfg));

    strncpy(ifreq.ifr_name, device, sizeof(ifreq.ifr_name) - 1);

    ifreq.ifr_data = (void *) &cfg;
    cfg.tx_type    = HWTSTAMP_TX_OFF;
    cfg.rx_filter  = HWTSTAMP_FILTER_ALL; // XXX: don't want timestamp all
    req = cfg;
    err = ioctl(fd, SIOCSHWTSTAMP, &ifreq);
    if (err < 0) {
        perror("ioctl(SIOCSHWTSTAMP)");
        return err;
    }

    if (memcmp(&cfg, &req, sizeof(cfg))) {

        printf("driver changed our HWTSTAMP options\n");
        printf("tx_type   %d not %d\n", cfg.tx_type, req.tx_type);
        printf("rx_filter %d not %d\n", cfg.rx_filter, req.rx_filter);

        if (cfg.tx_type != req.tx_type) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv){
    struct sockaddr_in myaddr;	//my address
    struct sockaddr_in remaddr;	//remote address
    socklen_t addrlen = sizeof(remaddr);
    int fd; //my socket
    int port; //port number
    char buf[BUFSIZE]; //receive buffer
 // for timestamp
    unsigned int opt;
    char nic[50];
    int err;

   
    /* Get port number */
    if(argc==3){
        port = atoi(argv[1]);
        strncpy(nic, argv[4], sizeof(nic));
    }
    else{
        printf("Usage: %s <port_number> <nic>\n", argv[0]);
        exit(1);
    }
    
    
    /* Create a UDP socket */
    
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create socket\n");
        exit(1);
    }

    /* Enable RX timestamp */
    err = hwts_init(fd, nic);
    if (err == 0) { // with HW support
        printf("Enabling HW RX timestamp\n");
        opt = SOF_TIMESTAMPING_RX_HARDWARE |
            SOF_TIMESTAMPING_RAW_HARDWARE;
    } else { // SW timestamp
        printf("Enabling SW RX timestamp\n");
        opt = SOF_TIMESTAMPING_RX_SOFTWARE |
            SOF_TIMESTAMPING_SOFTWARE;
    }
    opt |= SOF_TIMESTAMPING_OPT_CMSG |
        SOF_TIMESTAMPING_OPT_ID |
        SOF_TIMESTAMPING_OPT_TSONLY;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING,
               (char *) &opt, sizeof(opt)))
        error(1, 0, "setsockopt timestamping");
    opt = 1;
    
    memset((char *)&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(port);
    
    if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
        perror("bind failed");
        exit(1);
    }

    /* Receive data and send back to the sender */
    for (;;) {
        struct msghdr msg;
        struct iovec entry;
        struct {
            struct cmsghdr cm;
            char control[512];
        } control;

        printf("\nListening on port %d ...\n", port);
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &entry;
        msg.msg_iovlen = 1;
        entry.iov_base = buf;
        entry.iov_len = BUFSIZE;
        msg.msg_name = (caddr_t)&remaddr;
        msg.msg_namelen = sizeof(remaddr);
        msg.msg_control = &control;
        msg.msg_controllen = sizeof(control);

        int recvlen = recvmsg(fd, &msg, 0);
        if (recvlen >= 0) {
            buf[recvlen] = 0;
            printf("\nPing Message Received\n");
            printpacket(&msg, recvlen, 0);
        }
        else{
            printf("\nMessage Receive Failed\n");
            printf("\n---------------\n");
            continue;
        }
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &entry;
        msg.msg_iovlen = 1;
        entry.iov_base = NULL;
        entry.iov_len = 0;
        msg.msg_name = (caddr_t)&remaddr;
        msg.msg_namelen = sizeof(remaddr);
        msg.msg_control = &control;
        msg.msg_controllen = sizeof(control);

        recvlen = recvmsg(fd, &msg, MSG_ERRQUEUE);
        if (recvlen >= 0) {
            printpacket(&msg, recvlen, MSG_ERRQUEUE);
        }
        if (sendto(fd, buf, strlen(buf), 0, (struct sockaddr *)&remaddr, addrlen) < 0){
            perror("\nMessage Send Failed\n");
        }
        else{
            printf("\nPong Message Send\n");
        }
        printf("\n---------------\n");
    }
    
    return 0;
}
