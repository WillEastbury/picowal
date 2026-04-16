#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    (24 * 1024)

#define MEMP_NUM_TCP_PCB            24
#define MEMP_NUM_TCP_PCB_LISTEN     4
#define MEMP_NUM_UDP_PCB            8
#define MEMP_NUM_TCP_SEG            128
#define MEMP_NUM_ARP_QUEUE          10
#define MEMP_NUM_PBUF               64

// Size pbuf pool buffers to match our slot size — one TCP
// segment's worth of WAL data fits in a single pbuf, reducing
// fragmentation and enabling zero-copy reads by Core 1.
#define PBUF_POOL_SIZE              48
#define PBUF_POOL_BUFSIZE           1600

#define TCP_MSS                     1460
#define TCP_WND                     (16 * TCP_MSS)
#define TCP_SND_BUF                 (32 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

// Reduce TIME_WAIT from default 20s to 2s — on a LAN with one
// client, stale packets aren't arriving 20 seconds late.
#define TCP_MSL                     1000   // 1 second (2×MSL = 2s TIME_WAIT)

#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

#define LWIP_CHKSUM_ALGORITHM       3

#endif
