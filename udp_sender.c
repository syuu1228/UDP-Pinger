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

#define TIMEOUT 2
#define BUFSIZE 100

static int __poll(int fd)
{
    struct pollfd pollfd = {fd, POLLPRI, 0};
    int ret;

    ret = poll(&pollfd, 1, 100);
    if (ret != 1) {
        if (errno) {
            error(1, errno, "poll");
        }else{
            fprintf(stderr, "poll for tx timestamp timeout.\n");
            return -1;
        }
    }
    return 0;
}

static struct timespec ts_prev;
static void __print_timestamp(const char *name, struct timespec *cur,
                  uint32_t key, int payload_len)
{
    if (!(cur->tv_sec | cur->tv_nsec))
        return;

    fprintf(stderr, "  %s: %lu s %lu us (seq=%u, len=%u)",
            name, cur->tv_sec, cur->tv_nsec / 1000,
            key, payload_len);

    if ((ts_prev.tv_sec | ts_prev.tv_nsec)) {
        int64_t cur_ms, prev_ms;

        cur_ms = (long) cur->tv_sec * 1000 * 1000;
        cur_ms += cur->tv_nsec / 1000;

        prev_ms = (long) ts_prev.tv_sec * 1000 * 1000;
        prev_ms += ts_prev.tv_nsec / 1000;

        fprintf(stderr, "  (%+" PRId64 " us)", cur_ms - prev_ms);
    }

    ts_prev = *cur;
    fprintf(stderr, "\n");
}

static void print_timestamp(struct scm_timestamping *tss, int tstype,
                int tskey, int payload_len)
{
    const char *tsname;

    switch (tstype) {
    case SCM_TSTAMP_SCHED:
        tsname = "  ENQ";
        break;
    case SCM_TSTAMP_SND:
        tsname = "  SND";
        break;
    case SCM_TSTAMP_ACK:
        tsname = "  ACK";
        break;
    default:
        error(1, 0, "unknown timestamp type: %u",
        tstype);
    }
    __print_timestamp(tsname, &tss->ts[0], tskey, payload_len);
}

static void __recv_errmsg_cmsg(struct msghdr *msg, int payload_len)
{
    struct sock_extended_err *serr = NULL;
    struct scm_timestamping *tss = NULL;
    struct cmsghdr *cm;
    int batch = 0;

    for (cm = CMSG_FIRSTHDR(msg);
         cm && cm->cmsg_len;
         cm = CMSG_NXTHDR(msg, cm)) {
        if (cm->cmsg_level == SOL_SOCKET &&
            cm->cmsg_type == SCM_TIMESTAMPING) {
            tss = (void *) CMSG_DATA(cm);
        } else if ((cm->cmsg_level == SOL_IP &&
                cm->cmsg_type == IP_RECVERR) ||
               (cm->cmsg_level == SOL_IPV6 &&
                cm->cmsg_type == IPV6_RECVERR)) {
            serr = (void *) CMSG_DATA(cm);
            if (serr->ee_errno != ENOMSG ||
                serr->ee_origin != SO_EE_ORIGIN_TIMESTAMPING) {
                fprintf(stderr, "unknown ip error %d %d\n",
                        serr->ee_errno,
                        serr->ee_origin);
                serr = NULL;
            }
        } else
            fprintf(stderr, "unknown cmsg %d,%d\n",
                    cm->cmsg_level, cm->cmsg_type);

        if (serr && tss) {
            print_timestamp(tss, serr->ee_info, serr->ee_data,
                    payload_len);
            serr = NULL;
            tss = NULL;
            batch++;
        }
    }

    if (batch > 1)
        fprintf(stderr, "batched %d timestamps\n", batch);
}

static int recv_errmsg(int fd)
{
    static char ctrl[1024 /* overprovision*/];
    static struct msghdr msg;
    struct iovec entry;
    int ret = 0;

    memset(&msg, 0, sizeof(msg));
    memset(&entry, 0, sizeof(entry));
    memset(ctrl, 0, sizeof(ctrl));

    entry.iov_base = NULL;
    entry.iov_len = 0;
    msg.msg_iov = &entry;
    msg.msg_iovlen = 1;
    msg.msg_name = NULL;
    msg.msg_namelen = 0;
    msg.msg_control = ctrl;
    msg.msg_controllen = sizeof(ctrl);

    ret = recvmsg(fd, &msg, MSG_ERRQUEUE);
    if (ret == -1 && errno != EAGAIN)
        error(1, errno, "recvmsg");

    if (ret >= 0) {
        __recv_errmsg_cmsg(&msg, ret);
    }

    return ret == -1;
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
    cfg.tx_type    = HWTSTAMP_TX_ON;
    cfg.rx_filter  = HWTSTAMP_FILTER_NONE;
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
    socklen_t slen=sizeof(remaddr);
    int fd; //my socket
    int port; //port number
    char buf[BUFSIZE]; //receive buffer
    int times; //number of messages send
    char host_to_contact[50];
    struct hostent *hp, *gethostbyname();
    char ip[100];
    struct hostent *he;
    struct in_addr **addr_list;
    struct timeval starttime, endtime;//init the clock
    struct timeval timeout={TIMEOUT,0}; //set timeout
    int i;
// for timestamp
    unsigned int opt;
    char nic[50];
    int err;
    
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
    err = hwts_init(fd, nic);
    if (err == 0) { // with HW support
        printf("Enabling HW TX timestamp\n");
        opt = SOF_TIMESTAMPING_TX_HARDWARE |
            SOF_TIMESTAMPING_RAW_HARDWARE;
    } else { // SW timestamp
        printf("Enabling SW TX timestamp\n");
        opt = SOF_TIMESTAMPING_TX_SOFTWARE |
            SOF_TIMESTAMPING_SOFTWARE;
    }
    opt |= SOF_TIMESTAMPING_OPT_CMSG |
        SOF_TIMESTAMPING_OPT_ID |
        SOF_TIMESTAMPING_OPT_TSONLY;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING,
               (char *) &opt, sizeof(opt)))
        error(1, 0, "setsockopt timestamping");
    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_SELECT_ERR_QUEUE,
               (char *) &opt, sizeof(opt)))
        error(1, 0, "setsockopt timestamping");

   
    /* Send the messages and calculate RTT */
    
    for (i=0; i < times; i++) {
        char send_buf[] = "ping";
        if (sendto(fd, send_buf, strlen(buf), 0, (struct sockaddr *)&remaddr, slen)==-1) {
            perror("sendto");
            printf("\n---------------\n");
            continue;
        }
        gettimeofday(&starttime,0);
        printf("Ping packet send to %s port %d\n", host_to_contact, port);

        /* Receive TX timestamp */
        if (__poll(fd) != 0) {
            return -1;
        }
        while (!recv_errmsg(fd)) {}
        
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
    }
    close(fd);


    return 0;
}
