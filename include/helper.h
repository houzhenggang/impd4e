/*
 * impd4e - a small network probe which allows to monitor and sample datagrams
 * from the network based on hash-based packet selection.
 *
 * Copyright (c) 2011
 *
 * Fraunhofer FOKUS
 * www.fokus.fraunhofer.de
 *
 * in cooperation with
 *
 * Technical University Berlin
 * www.av.tu-berlin.de
 *
 * authors:
 * Ramon Masek <ramon.masek@fokus.fraunhofer.de>
 * Christian Henke <c.henke@tu-berlin.de>
 * Carsten Schmoll <carsten.schmoll@fokus.fraunhofer.de>
 *
 * For questions/comments contact packettracking@fokus.fraunhofer.de
 *
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation;
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HELPER_H_
#define HELPER_H_

#include <stdint.h>
#include <limits.h>
#include <errno.h>
#include <sys/types.h>

#include "constants.h"
//#include "settings.h"

#ifdef PFRING
/*
 * These are the types that are the same on all platforms, and that
 * have been defined by <net/bpf.h> for ages.
 */
 // TODO: get rid of these defines
 //       include pcap/bpf.h instead
#define DLT_NULL    0   /* BSD loopback encapsulation */
#define DLT_EN10MB  1   /* Ethernet (10Mb) */
#define DLT_EN3MB   2   /* Experimental Ethernet (3Mb) */
#define DLT_AX25    3   /* Amateur Radio AX.25 */
#define DLT_PRONET  4   /* Proteon ProNET Token Ring */
#define DLT_CHAOS   5   /* Chaos */
#define DLT_IEEE802 6   /* 802.5 Token Ring */
#define DLT_ARCNET  7   /* ARCNET, with BSD-style header */
#define DLT_SLIP    8   /* Serial Line IP */
#define DLT_PPP     9   /* Point-to-point Protocol */
#define DLT_FDDI    10  /* FDDI */
#endif

// return binary ip address in network byte order
uint32_t getIPv4AddressFromDevice(char* dev_name);

char* l_trim( char* s );
void  r_trim( char* s );

char* htoa(uint32_t ipaddr);
char* ntoa(uint32_t ipaddr);

#ifndef PFRING
void setNONBlocking( device_dev_t* pDevice );
#endif

int get_file_desc( device_dev_t* pDevice );

#ifdef PFRING
#ifdef PFRING_STATS
void print_stats( device_dev_t* dev );
#endif
int pfring_dispatch(pfring* pd, int max_packets, void(*packet_handler)(u_char*, const struct pfring_pkthdr*, const u_char*), u_char* user_args);
int pfring_dispatch_wrapper(dh_t dh, int cnt,    void(*packet_handler)(u_char*, const struct pfring_pkthdr*, const u_char*), u_char* user_args);
int setPFRingFilter(device_dev_t* pfring_device);
int8_t setPFRingFilterPolicy(device_dev_t* pfring_device);
#endif

#ifndef PFRING
int  set_all_filter(const char* bpf);
int  set_filter(device_dev_t* pd, const char* bpf);
void setFilter(device_dev_t* pcap_device);
#endif

void print_byte_array_hex( uint8_t* p, int length );

#endif /* HELPER_H_ */
