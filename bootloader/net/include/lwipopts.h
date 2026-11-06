/* lwIP configuration for the bootloader's network window.
 * Bare metal, polled (NO_SYS), IPv4 only: ARP, ICMP (ping), UDP, DHCP, TFTP. No TCP. */
#ifndef LWIPOPTS_H
#define LWIPOPTS_H

#define NO_SYS                      1
#define SYS_LIGHTWEIGHT_PROT        0
#define LWIP_TIMERS                 1
#define LWIP_NETCONN                0
#define LWIP_SOCKET                 0

#define LWIP_IPV4                   1
#define LWIP_IPV6                   0
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    0
#define LWIP_UDP                    1
#define LWIP_TCP                    0
#define LWIP_DHCP                   1
#define LWIP_AUTOIP                 0
#define LWIP_ACD                    0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_DNS                    0
#define LWIP_IGMP                   0
#define IP_REASSEMBLY               0
#define IP_FRAG                     0

#define LWIP_SINGLE_NETIF           1
#define LWIP_NETIF_HOSTNAME         1
#define LWIP_NETIF_STATUS_CALLBACK  0
#define LWIP_NETIF_LINK_CALLBACK    0

/* Memory: everything lives in the SDRAM region set aside for the window (link_net.ld). */
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (32 * 1024)
#define MEMP_NUM_PBUF               16
#define MEMP_NUM_UDP_PCB            4
/* lwIP sizes its timer pool for its own users only; the TFTP server needs one more. */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 2)
#define PBUF_POOL_SIZE              24
#define PBUF_POOL_BUFSIZE           1536
#define ETH_PAD_SIZE                0

#define LWIP_STATS                  0
#define LWIP_DEBUG                  0
#define LWIP_PROVIDE_ERRNO          1

/* TFTP server */
#define TFTP_MAX_FILENAME_LEN       32
#define TFTP_TIMEOUT_MSECS          2000
#define TFTP_MAX_RETRIES            5

unsigned int netboot_rand(void);
#define LWIP_RAND()                 netboot_rand()

#endif
