//
//  udp_sender.c
//  
//
//  Created by XueFei Yang on 2015-02-11.
//
//

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>

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
#include <sys/ioctl.h>
#include <error.h>

#define TIMEOUT 2
#define BUFSIZE 100

static void printpacket(struct msghdr *msg, int res,
            char *data,
            int sock, int recvmsg_flags)
{
    struct sockaddr_in *from_addr = (struct sockaddr_in *)msg->msg_name;
    struct cmsghdr *cmsg;
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
                printf("type %d", cmsg->cmsg_type);
                break;
            }
            break;
        case IPPROTO_IP:
            printf("IPPROTO_IP ");
            switch (cmsg->cmsg_type) {
            case IP_RECVERR: {
                struct sock_extended_err *err =
                    (struct sock_extended_err *)CMSG_DATA(cmsg);
                printf("IP_RECVERR ee_errno '%s' ee_origin %d => %s",
                    strerror(err->ee_errno),
                    err->ee_origin,
#ifdef SO_EE_ORIGIN_TIMESTAMPING
                    err->ee_origin == SO_EE_ORIGIN_TIMESTAMPING ?
                    "bounced packet" : "unexpected origin"
#else
                    "probably SO_EE_ORIGIN_TIMESTAMPING"
#endif
                    );
                break;
            }
            case IP_PKTINFO: {
                struct in_pktinfo *pktinfo =
                    (struct in_pktinfo *)CMSG_DATA(cmsg);
                printf("IP_PKTINFO interface index %u",
                    pktinfo->ipi_ifindex);
                break;
            }
            default:
                printf("type %d", cmsg->cmsg_type);
                break;
            }
            break;
        default:
            printf("level %d type %d",
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
        printpacket(&msg, res, data,
                sock, recvmsg_flags);
    }
}


int main(int argc, char **argv){
    struct sockaddr_in myaddr;    //my address
    struct sockaddr_in remaddr;    //remote address
    socklen_t slen=sizeof(remaddr);
    int fd; //my socket
    int port; //port number
    char buf[BUFSIZE]; //receive buffer
    int times; //number of messages send
    char host_to_contact[50];
    struct hostent *gethostbyname();
    char ip[100];
    struct hostent *he;
    struct in_addr **addr_list;
    struct timeval starttime, endtime;//init the clock
    struct timeval timeout={TIMEOUT,0}; //set timeout
    int i;
// for timestamp
    unsigned int opt;
    char nic[50];
    int hw = 0;
    struct ifreq device;
    struct ifreq hwtstamp;
    struct hwtstamp_config hwconfig, hwconfig_requested;
    int enabled = 1;

    /* Get host name, port number, loop times */
    if(argc==5){
        strncpy(host_to_contact, argv[1], sizeof(host_to_contact));
        port=atoi(argv[2]);
        times = atoi(argv[3]);
        strncpy(nic, argv[4], sizeof(nic));
    }
    else{
        printf("Usage: %s <host name> <port number> <number of messages> <nic>\n", argv[0]);
        exit(1);
    }
    
    /* Create a UDP socket */
    
    if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("cannot create socket\n");
        exit(1);
    }
    
    /* Get IP address from Hostname */
    
    he = gethostbyname(host_to_contact);
    
    addr_list = (struct in_addr **) he->h_addr_list;
    
    for(i = 0; addr_list[i] != NULL; i++)
    {
        strcpy(ip , inet_ntoa(*addr_list[i]) );
    }
    
    memset((char *)&myaddr, 0, sizeof(myaddr));
    myaddr.sin_family = AF_INET;
    myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    myaddr.sin_port = htons(0);
    
    if (bind(fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) < 0) {
        perror("bind failed");
        return 0;
    }
    
    memset((char *) &remaddr, 0, sizeof(remaddr));
    remaddr.sin_family = AF_INET;
    remaddr.sin_port = htons(port);
    if (inet_aton(ip, &remaddr.sin_addr)==0) {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }
    
    /* Set receive UDP message timeout */
    
    setsockopt(fd,SOL_SOCKET,SO_RCVTIMEO,(char*)&timeout,sizeof(struct timeval));

    /* Enable TX timestamp */
    memset(&device, 0, sizeof(device));
    strncpy(device.ifr_name, nic, sizeof(device.ifr_name));
    if (ioctl(fd, SIOCGIFADDR, &device) < 0) {
        perror("getting interface IP address");
        exit(1);
    }

    memset(&hwtstamp, 0, sizeof(hwtstamp));
    strncpy(hwtstamp.ifr_name, nic, sizeof(hwtstamp.ifr_name));
    hwtstamp.ifr_data = (void *)&hwconfig;
    memset(&hwconfig, 0, sizeof(hwconfig));
    hwconfig.tx_type =HWTSTAMP_TX_ON;
    hwconfig.rx_filter =HWTSTAMP_FILTER_NONE;
    hwconfig_requested = hwconfig;
    if (ioctl(fd, SIOCSHWTSTAMP, &hwtstamp) < 0) {
        if ((errno == EINVAL || errno == ENOTSUP) &&
            hwconfig_requested.tx_type == HWTSTAMP_TX_OFF &&
            hwconfig_requested.rx_filter == HWTSTAMP_FILTER_NONE) {
            printf("SIOCSHWTSTAMP: disabling hardware time stamping not possible\n");
        } else {
            perror("SIOCSHWTSTAMP");
        }
    } else {
        hw = 1;
    }
    printf("SIOCSHWTSTAMP: tx_type %d requested, got %d; rx_filter %d requested, got %d\n",
           hwconfig_requested.tx_type, hwconfig.tx_type,
           hwconfig_requested.rx_filter, hwconfig.rx_filter);

    if (hw) {
        printf("Enabling HW TX timestamp\n");
        opt = SOF_TIMESTAMPING_TX_HARDWARE |
            SOF_TIMESTAMPING_RAW_HARDWARE;
    } else { // SW timestamp
        printf("Enabling SW TX timestamp\n");
        opt = SOF_TIMESTAMPING_TX_SOFTWARE |
            SOF_TIMESTAMPING_SOFTWARE;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING,
               (char *) &opt, sizeof(opt)))
        error(1, 0, "setsockopt timestamping");
    if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE,
               (char *) &opt, sizeof(opt)))
        error(1, 0, "setsockopt timestamping");
    /* request IP_PKTINFO for debugging purposes */
    if (setsockopt(fd, SOL_IP, IP_PKTINFO,
               &enabled, sizeof(enabled)) < 0)
        error(1, 0, "setsockopt IP_PKTINFO");

   
    /* Send the messages and calculate RTT */
    
    for (i=0; i < times; i++) {
        char send_buf[] = "ping";
        fd_set errorfs;
        struct timeval tv;
        int res;
        if (sendto(fd, send_buf, strlen(buf), 0, (struct sockaddr *)&remaddr, slen)==-1) {
            perror("sendto");
            printf("\n---------------\n");
            continue;
        }
        gettimeofday(&starttime,0);
        printf("Ping packet send to %s port %d\n", host_to_contact, port);

        /* Receive TX timestamp */
        FD_ZERO(&errorfs);
        FD_SET(fd, &errorfs);
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        res = select(fd + 1, 0, 0, &errorfs, &tv);
        if (res > 0 && FD_ISSET(fd, &errorfs)) {
            recvpacket(fd, MSG_ERRQUEUE);
        } else {
            printf("Timeout to read from errqueue\n");
        }

        /* Waiting message come back */
        int recvlen = recvfrom(fd, buf, BUFSIZE, 0, (struct sockaddr *)&remaddr, &slen);
        if (recvlen >= 0) {
            buf[recvlen] = 0;
            printf("\nPong Message Received\n");
            gettimeofday(&endtime,0);
            double timeuse = 1000000*(endtime.tv_sec - starttime.tv_sec) + endtime.tv_usec - starttime.tv_usec;
            timeuse /=1000;
            printf("RTT: %.3f Seconds\n", timeuse);
        }
        else{
            printf("\nMessage Receive Timeout or Error\n");
            printf("\n---------------\n");
            continue;
        }
        sleep(1);
    }
    close(fd);


    return 0;
}
