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

#include <inttypes.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include <time.h>

#ifdef PFRING
#include <sys/time.h>
#include <time.h>
#endif

#include "ev_handler.h"
#include "socket_handler.h"
#include "ipfix_handler.h"
#include "logger.h"
#include "netcon.h"

#include "hash.h"
//#include "ipfix.h"
//#include "ipfix_def.h"
//#include "ipfix_def_fokus.h"
#include "stats.h"

// Custom logger
#include "logger.h"
#include "netcon.h"
#include "ev_handler.h"

#include "helper.h"
#include "constants.h"

#include "settings.h"

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------*/
#define RESYNC_PERIOD 1.5 /* seconds */
#define OP_CODE 1 /* identifies a rule packet for openepc*/

/**
 * Event and Signal handling via libev
 */
struct {
    ev_signal sigint_watcher;
    ev_signal sigalrm_watcher;
    ev_signal sigpipe_watcher;
    ev_timer export_timer_pkid;
    ev_timer export_timer_sampling;
    ev_timer export_timer_stats;
    ev_timer export_timer_location;
    ev_timer resync_timer;
    ev_io packet_watchers[MAX_INTERFACES];
} events;

//typedef struct {
//  char cmd;
//  set_cfg_fct_t fct;
//  const char* desc;
//}
//cfg_fct_t;

// register available configuration functions
// to the config function array
cfg_fct_t configuration_fct[] = {
    { '?', &configuration_help, "INFO: -? this help\n"},
    { 'h', &configuration_help, "INFO: -h this help\n"},
    { 'r', &configuration_set_ratio, "INFO: -r capturing ratio in %\n"},
    { 'm', &configuration_set_min_selection, "INFO: -m capturing selection range min (hex|int)\n"},
    { 'M', &configuration_set_max_selection, "INFO: -M capturing selection range max (hex|int)\n"},
    { 'f', &configuration_set_filter, "INFO: -f bpf filter expression\n"},
    { 't', &configuration_set_template, "INFO: -t template (ts|min|lp)\n"},
    { 'I', &configuration_set_export_to_pktid, "INFO: -I pktid export interval (s)\n"},
    { 'J', &configuration_set_export_to_probestats, "INFO: -J porbe stats export interval (s)\n"},
    { 'K', &configuration_set_export_to_ifstats, "INFO: -K interface stats export interval (s)\n"}
};

char cfg_response[256];
#define SET_CFG_RESPONSE(...) snprintf(cfg_response, sizeof(cfg_response), "" __VA_ARGS__);
#define CFG_RESPONSE cfg_response

// -----------------------------------------------------------------------------
// Prototypes
// -----------------------------------------------------------------------------
#ifndef PFRING
void handle_packet(u_char *user_args, const struct pcap_pkthdr *header,
        const u_char * packet);
#else
void packet_pfring_cb(u_char *user_args, const struct pfring_pkthdr *header,
        const u_char *packet);
#endif

// dummy function to prevent segmentation fault

char* unknown_cmd_fct(unsigned long id, char* msg) {
    LOGGER_warn("unknown command received: id=%lu, msg=%s", id, msg);
    return "unknown command received";
}

cfg_fct_t* get_cfg_fct(char cmd) {
    int i = 0;
    int length = sizeof (configuration_fct) / sizeof (cfg_fct_t);

    for (i = 0; i < length; ++i) {
        if (cmd == configuration_fct[i].cmd) {
            return &configuration_fct[i];
        }
    }
    LOGGER_warn("unknown command received: cmd=%c", cmd);
    return NULL;
}

set_cfg_fct_t getFunction(char cmd) {
    cfg_fct_t* f = get_cfg_fct(cmd);
    if (NULL != f) {
        return f->fct;
    } else {
        LOGGER_warn("unknown command received: cmd=%c", cmd);
        return unknown_cmd_fct;
    }
}

/**
 * Call back for SIGINT (Ctrl-C).
 * It breaks all loops and leads to shutdown.
 */
void sigint_cb(EV_P_ ev_signal *w, int revents) {
    LOGGER_info("Signal INT received");
    ev_unloop(EV_A_ EVUNLOOP_ALL);
}

/**
 * SIGPIPE call back, currently not used.
 */
void sigpipe_cb(EV_P_ ev_signal *w, int revents) {
    LOGGER_info("Ignoring SIGPIPE, libipfix should indefinitely try to reconnect to collector.");
}

/**
 * SIGALRM call back, currently not used.
 */
void sigalrm_cb(EV_P_ ev_signal *w, int revents) {
    LOGGER_info("Signal ALRM received");
}

void user_input_cb(EV_P_ ev_io *w, int revents) {
    char buffer[129];
    //int   i = 0;
    //char  c = EOF;

    //while( '\n' != (c = fgetc(stdin)) ) {
    //if( i < sizeof(buffer)-1 ) {
    //buffer[i] = c;
    //}
    //++i;
    //fprintf(stderr, "c= %c\n", c);
    //}
    //buffer[i] = '\0';
    //fprintf(stderr, "%s\n", buffer);
    //exit(0);


    //if( 0 != buffer[0] ) {
    if (NULL != fgets(buffer, sizeof (buffer), stdin)) {
        //fscanf( stdin, "%5c", buffer );
        //LOGGER_info("user input: %s\n", buffer);
        //fprintf(stdout,"user input: %s", buffer);
        if (0 == strncmp(buffer, "exit", 4) ||
                0 == strncmp(buffer, "quit", 4)) {
            exit(0);
        }

        char* b = buffer;
        while (!isalpha(*b) && (*b != '\0')) ++b;
        if ('\0' == *b) return;
        char cmd = *b;
        ++b;

        // remove leading whitespaces
        while (isspace(*b)) ++b;
        //r_trim(b);

        //fprintf(stdout,"user input: [%c: '%s']\n", cmd, b);
        char* rsp_msg = (*getFunction(cmd))(1, b);
        fprintf(stdout, "%s", rsp_msg);

        //char msg[strlen(buffer+1+7)];
        //sprintf( msg, "mid:1 -%s", buffer );
        //runtime_configuration_cb( msg );
    }
}

/**
 * Setups and starts main event loop.
 */
void event_loop(EV_P) {
    LOGGER_info("event_loop()");

    /*=== Setting up event loop ==*/

    /* signals */
    ev_signal_init(&events.sigint_watcher, sigint_cb, SIGINT);
    ev_signal_start(EV_A_ & events.sigint_watcher);
    ev_signal_init(&events.sigalrm_watcher, sigalrm_cb, SIGALRM);
    ev_signal_start(EV_A_ & events.sigalrm_watcher);
    ev_signal_init(&events.sigpipe_watcher, sigpipe_cb, SIGPIPE);
    ev_signal_start(EV_A_ & events.sigpipe_watcher);

    /* resync   */
    ev_timer_init(&events.resync_timer, resync_timer_cb, 0, RESYNC_PERIOD);
    ev_timer_again(EV_A_ & events.resync_timer);

    /* export timers */
    /* export device measurements */
    ev_timer_init(&events.export_timer_pkid, export_timer_pktid_cb //callback
            , 0 // after
            , g_options.export_pktid_interval // repeat
            );
    // trigger first after 'repeat'
    ev_timer_again(EV_A_ & events.export_timer_pkid);

    /* export device sampling stats */
    ev_timer_init(&events.export_timer_sampling, export_timer_sampling_cb // callback
            , 0 // after, not used for ev_timer_again
            , g_options.export_sampling_interval // repeat
            );
    // trigger first after 'repeat'
    ev_timer_again(EV_A_ & events.export_timer_sampling);

    /* export system stats - with at least one export*/
    ev_timer_init(&events.export_timer_stats, export_timer_stats_cb // callback
            , 0 // after
            , g_options.export_stats_interval // repeat
            );
    // trigger first after 'after' then after 'repeat'
    ev_timer_start(EV_A_ & events.export_timer_stats);

    /* export system location - with at least one export*/
    ev_timer_init(&events.export_timer_location, export_timer_location_cb // callback
            , 0 // after
            , g_options.export_location_interval // repeat
            );
    // trigger first after 'after' then after 'repeat'
    ev_timer_start(EV_A_ & events.export_timer_location);

    // listen to standart input
    ev_io event_io;
    ev_io_init(&event_io, user_input_cb, STDIN_FILENO, EV_READ);
    ev_io_start(EV_A_ & event_io);

    /*   packet watchers */
    event_setup_pcapdev(EV_A);

    /* setup network console */
    event_setup_netcon(EV_A);

    /* Enter main event loop; call unloop to exit.
     *
     * Everything is going to be handled within this call
     * accordingly to callbacks defined above.
     * */
    ev_loop(EV_A_ 0);
}

/**
 * Setup network console
 */
void event_setup_netcon(EV_P) {
    char *host = "localhost";
    int port = 5000;

    if (netcon_init(EV_A_ host, port) < 0) {
        LOGGER_error("could not initialize netcon: host: %s, port: %d ", host,
                port);
    }

    // register runtime configuration callback to netcon
    netcon_register(runtime_configuration_cb);
}

/**
 * Here we setup a pcap device in non block mode and configure libev to read
 * a packet as soon it is available.
 */
void event_setup_pcapdev(EV_P) {
    int i;
    device_dev_t * pcap_dev_ptr;
    for (i = 0; i < g_options.number_interfaces; i++) {
        LOGGER_debug("Setting up interface: %s", if_devices[i].device_name);

        pcap_dev_ptr = &if_devices[i];
        // TODO review

#ifndef PFRING
        setNONBlocking(pcap_dev_ptr);
#endif

        int fd = get_file_desc(pcap_dev_ptr);
        LOGGER_debug("File Descriptor: %d", fd);

        /* storing a reference of packet device to
         be passed via watcher on a packet event so
         we know which device to read the packet from */
        // todo: review; where is the memory allocated
        events.packet_watchers[i].data = (device_dev_t *) pcap_dev_ptr;
        ev_io_init(&events.packet_watchers[i], packet_watcher_cb
                , fd
                , EV_READ);
        ev_io_start(EV_A_ & events.packet_watchers[i]);
    }
}

/**
 * Called whenever a new packet is available. Note that packet_pcap_cb is
 * responsible for reading the packet.
 */
void packet_watcher_cb(EV_P_ ev_io *w, int revents) {
    int error_number = 0;

    LOGGER_trace("Enter");
    LOGGER_trace("event: %d", revents);

    // retrieve respective device a new packet was seen
    device_dev_t *pcap_dev_ptr = (device_dev_t *) w->data;

    switch (pcap_dev_ptr->device_type) {
        case TYPE_testtype:
#ifndef PFRING
        case TYPE_PCAP_FILE:
        case TYPE_PCAP:
        case TYPE_SOCKET_INET:
        case TYPE_SOCKET_UNIX:
        {
            error_number = pcap_dev_ptr->dispatch(pcap_dev_ptr->dh
                    , PCAP_DISPATCH_PACKET_COUNT
                    , handle_packet
                    , (u_char*) pcap_dev_ptr);

            if (0 > error_number) {
                LOGGER_error("Error DeviceNo   %s", pcap_dev_ptr->device_name);
                LOGGER_error("Error No.: %d", error_number);
                LOGGER_error("Error No.: %d", errno);
            }
            LOGGER_trace("Packets read: %d", error_number);
        }
            break;
#else
        case TYPE_PFRING:
        {
            LOGGER_trace("pfring");
            error_number = pcap_dev_ptr->dispatch(pcap_dev_ptr->dh
                    , PCAP_DISPATCH_PACKET_COUNT
                    , packet_pfring_cb
                    , (u_char*) pcap_dev_ptr);

            if (0 > error_number) {
                LOGGER_error("Error DeviceNo   %s", pcap_dev_ptr->device_name);
                LOGGER_error("Error No.: %d", error_number);
                LOGGER_error("Error No.: %d", errno);
            }
            LOGGER_trace("Packets read: %d", error_number);
        }
            break;
#endif

        default:
            break;
    }
    LOGGER_trace("Return");
}

#ifdef PFRING

void packet_pfring_cb(u_char *user_args, const struct pfring_pkthdr *header,
        const u_char *packet) {
    device_dev_t* if_device = (device_dev_t*) user_args;
    uint8_t layers[4] = {0};
    uint32_t hash_result = 0;
    uint32_t copiedbytes = 0;
    uint8_t ttl = 0;
    uint64_t timestamp = 0;
    int pktid = 0;

    LOGGER_trace("packet_pfring_cb");

    if_device->sampling_delta_count++;
    if_device->totalpacketcount++;

    layers[L_NET] = header->extended_hdr.parsed_pkt.ip_version;
    layers[L_TRANS] = header->extended_hdr.parsed_pkt.l3_proto;

    // hash was already calculated in-kernel. use it
    hash_result = header->extended_hdr.parsed_pkt.pkt_detail.aggregation.num_pkts;
    /*
     printf("offsets@t0 l(3,4,5): %d, %d, %d\n",
     header->extended_hdr.parsed_pkt.pkt_detail.offset.l3_offset + if_device->offset[L_NET],
     header->extended_hdr.parsed_pkt.pkt_detail.offset.l4_offset + if_device->offset[L_NET],
     header->extended_hdr.parsed_pkt.pkt_detail.offset.payload_offset + if_device->offset[L_NET]);
     */
    //if_device->offset[L_NET]       = header->extended_hdr.parsed_pkt.pkt_detail.offset.l3_offset;
    if_device->offset[L_TRANS] = header->extended_hdr.parsed_pkt.pkt_detail.offset.l4_offset + if_device->offset[L_NET];
    if_device->offset[L_PAYLOAD] = header->extended_hdr.parsed_pkt.pkt_detail.offset.payload_offset + if_device->offset[L_NET];

    //printf("pre getTTL: caplen: %02d, offset_net: %02d, ipv: %d\n",
    //            header->caplen, if_device->offset[L_NET], layers[L_NET]);
    ttl = getTTL(packet, header->caplen, if_device->offset[L_NET],
            layers[L_NET]);

    if_device->export_packet_count++;
    if_device->sampling_size++;

    // bypassing export if disabled by cmd line
    if (g_options.export_pktid_interval <= 0) {
        return;
    }

    // in case we want to use the hashID as packet ID
    if (g_options.hashAsPacketID == 1) {
        pktid = hash_result;
    } else {
        // selection of viable fields of the packet - depend on the selection function choosen
        copiedbytes = g_options.selection_function(packet, header->caplen,
                if_device->outbuffer, if_device->outbufferLength,
                if_device->offset, layers);
        pktid = g_options.pktid_function(if_device->outbuffer, copiedbytes);
    }

    /*
     printf("offsets@t1 l(3,4,5): %d, %d, %d\n",
     if_device->offset[L_NET],
     if_device->offset[L_TRANS],
     if_device->offset[L_PAYLOAD]);
     */

    //printf("pktid: 0d%d\n", pktid);

    timestamp = (uint64_t) header->ts.tv_sec * 1000000ULL
            + (uint64_t) header->ts.tv_usec;

    switch (g_options.templateID) {
        case MINT_ID:
        {
            void* fields[] = {&timestamp, &hash_result, &ttl};
            uint16_t lengths[] = {8, 4, 1};

            if (0 > ipfix_export_array(ipfix(),
                    if_device->ipfixtmpl_min, 3, fields, lengths)) {
                LOGGER_fatal("ipfix_export() failed: %s", strerror(errno));
                exit(1);
            }
            break;
        }

        case TS_ID:
        {
            void* fields[] = {&timestamp, &hash_result};
            uint16_t lengths[] = {8, 4};

            if (0 > ipfix_export_array(ipfix(),
                    if_device->ipfixtmpl_ts, 2, fields, lengths)) {
                LOGGER_fatal("ipfix_export() failed: %s", strerror(errno));
                exit(1);
            }
            break;
        }

        case TS_TTL_PROTO_ID:
        {
            uint16_t length;

            if (layers[L_NET] == N_IP) {
                length = ntohs(*((uint16_t*)
                        (&packet[if_device->offset[L_NET] + 2])));
            } else if (layers[L_NET] == N_IP6) {
                length = ntohs(*((uint16_t*)
                        (&packet[if_device->offset[L_NET] + 4])));
            } else {
                LOGGER_fatal("cannot parse packet length");
                length = 0;
            }

            void* fields[] = {&timestamp,
                &hash_result,
                &ttl,
                &length,
                &layers[L_TRANS],
                &layers[L_NET]};
            uint16_t lengths[6] = {8, 4, 1, 2, 1, 1};

            if (0 > ipfix_export_array(ipfix(),
                    if_device->ipfixtmpl_ts_ttl, 6, fields, lengths)) {
                LOGGER_fatal("ipfix_export() failed: %s", strerror(errno));
                exit(1);
            }
            break;
        }
        default:
            break;
    } // switch (options.templateID)

    // flush ipfix storage if max packetcount is reached
    if (if_device->export_packet_count >= g_options.export_packet_count) {
        if_device->export_packet_count = 0;
        export_flush();
    }
}
#endif

#ifndef PFRING

inline int set_value(void** field, uint16_t* length, void* value, uint16_t size) {
    *field = value;
    *length = size;
    return 1;
}

static void print_array(const u_char *p, int l) {
    int i = 0;
    for (i = 0; i < l; ++i) {
        if( 0 != i && 0 == i%4 ) {
           if( 0 == i%20 ) 
              fprintf(stderr, "\n");
           else
              fprintf(stderr, "| ");
        }
        fprintf(stderr, "%02x ", p[i]);
        //LOGGER_debug( "%02x ", packet[i]);
    }
    fprintf(stderr, "\n");
}

static void print_ip4(const u_char *p, int l) {
    if (0x40 != (p[0]&0xf0)) {
        print_array(p, l);
        return;
    }
    int i = 0;
    for (i = 0; i < l && i < 12; ++i) {
        fprintf(stderr, "%02x ", p[i]);
    }
    fprintf(stderr, "\b [");
    for (; i < l && i < 16; ++i) {
        fprintf(stderr, "%3d.", p[i]);
    }
    fprintf(stderr, "\b] [");
    for (; i < l && i < 20; ++i) {
        fprintf(stderr, "%3d.", p[i]);
    }
    fprintf(stderr, "\b] ");
    for (; i < l; ++i) {
        fprintf(stderr, "%02x ", p[i]);
    }
    fprintf(stderr, "\n");
}

inline uint8_t get_ttl(packet_t *p, uint32_t offset, netProt_t nettype) {
    switch (nettype) {
        case N_IP:
        {
            return p->ptr[offset + 8];
        }
        case N_IP6:
        {
            return p->ptr[offset + 7];
        }
        default:
        {
            return 0;
        }
    }
}

inline uint16_t get_ip_length(packet_t *p, uint32_t offset, netProt_t nettype) {
    switch (nettype) {
        case N_IP:
        {
            return ntohs(*((uint16_t*) (&p->ptr[offset + 2])));
        }
        case N_IP6:
        {
            return ntohs(*((uint16_t*) (&p->ptr[offset + 4])));
        }
        default:
        {
            LOGGER_fatal("cannot parse packet length");
            return 0;
        }
    }
}

inline uint8_t* get_ipa(packet_t *p, uint32_t offset, netProt_t nettype) {
    static uint32_t unknown_ipa = 0;
    switch (nettype) {
        case N_IP:
        {
            return p->ptr + offset + 12;
        }
        case N_IP6:
        default:
        {
            return (uint8_t*) &unknown_ipa;
        }
    }
}

inline uint16_t get_port(packet_t *p, uint32_t offset, transProt_t transtype) {
    switch (transtype) {
        case T_UDP:
        case T_TCP:
        case T_SCTP:
        {
            return ntohs(*((uint16_t*) (&p->ptr[offset])));
        }
        default:
        {
            return 0;
        }
    }
}

// return the packet protocol beyond the link layer (defined by rfc )
// !! the raw packet is expected (include link layer)
// return 0 if unknown

inline uint16_t get_nettype(packet_t *packet, int linktype) {
    switch (linktype) {
        case DLT_EN10MB: // 14 octets
            // Ethernet
            return ntohs(*((uint16_t*) (&packet->ptr[12])));
            break;
        case DLT_ATM_RFC1483: // 8 octets
            return ntohs(*((uint16_t*) (&packet->ptr[6])));
            break;
        case DLT_LINUX_SLL: // 16 octets
            // TODO: either the first 2 octets or the last 2 octets
            return ntohs(*((uint16_t*) (&packet->ptr[14])));
            break;
        case DLT_RAW:
            break;
        default:
            break;
    }
    return 0;
}

// return the packet protocol beyond the link layer (defined by rfc )
// !! the raw packet is expected (include link layer)
// return 0 if unknown

inline uint16_t get_nettype_pkt(packet_t *packet) {
    // check if at least 20 bytes are available
    if (20 <= packet->len) {
        // currently only IP (v4, v6) is relevant
        switch (packet->ptr[0]&0xf0) {
            case 0x40: return 0x0800;
                break;
            case 0x60: return 0x86DD;
                break;
        }
    }
    return 0;
}

inline uint64_t get_timestamp(struct timeval ts) {
    return (uint64_t) ts.tv_sec * 1000000ULL
            + (uint64_t) ts.tv_usec;
}

inline packet_t decode_array(packet_t* p) {
    packet_t data = {NULL, 0};
    data.len = ntohs(*((uint16_t*) (p->ptr)));
    data.ptr = p->ptr + 2;
    p->ptr += (data.len + 2);
    p->len -= (data.len + 2);
    return data;
}

// decode value of type
// [length][data]
inline packet_t decode_raw(packet_t *p, uint32_t len) {
    packet_t data = {NULL, 0};
    
    data.len = len;
    data.ptr = p->ptr;
    p->len -= len;
    p->ptr += len;
    return data;
}

inline uint32_t decode_uint32(packet_t *p) {
    uint32_t value = ntohl(*((uint32_t*) p->ptr));
    p->len -= 4;
    p->ptr += 4;
    return value;
}

inline uint16_t decode_uint16(packet_t *p) {
    uint16_t value = ntohs(*((uint16_t*) p->ptr));
    p->len -= 2;
    p->ptr += 2;
    return value;
}

inline uint8_t decode_uint8(packet_t *p) {
    uint8_t value = *p->ptr;
    p->len -= 1;
    p->ptr += 1;
    return value;
}

inline void apply_offset(packet_t *pkt, uint32_t offset) {
    LOGGER_trace("Offset: %d", offset);
    if (offset < pkt->len) {
        pkt->ptr += offset;
        pkt->len -= offset;
    } else {
        pkt->len = 0;
    }
}

void handle_default_packet(packet_t *packet, packet_info_t *packet_info) {
    LOGGER_warn("packet type: 0x%04X (not supported)", packet_info->nettype);
}

void handle_ip_packet(packet_t *packet, packet_info_t *packet_info) {
    uint32_t hash_id = 0;
    uint32_t pkt_id = 0;

    uint32_t offsets[4] = {0}; // layer offsets for: link, net, transport, payload
    uint8_t layers[4] = {0}; // layer protocol types for: link, net, transport, payload

    LOGGER_trace(" ");

    // reset hash buffer
    packet_info->device->hash_buffer.len = 0;

    // find headers of the IP STACK
    findHeaders(packet->ptr, packet->len, offsets, layers);

    // selection of viable fields of the packet - depend on the selection function choosen
    // locate protocolsections of ip-stack --> findHeaders() in hash.c
    g_options.selection_function(packet,
            &packet_info->device->hash_buffer,
            offsets, layers);

    if (0) print_array(packet_info->device->hash_buffer.ptr, packet_info->device->hash_buffer.len);

    if (0 == packet_info->device->hash_buffer.len) {
        LOGGER_trace("Warning: packet does not contain Selection");
        return;
    }

    // hash the chosen packet data
    hash_id = g_options.hash_function(&packet_info->device->hash_buffer);
    LOGGER_trace("hash id: 0x%08X", hash_id);

    // hash id must be in the chosen selection range to count
    if ((g_options.sel_range_min <= hash_id) &&
            (g_options.sel_range_max >= hash_id)) {
        packet_info->device->export_packet_count++;
        packet_info->device->sampling_size++;

        // bypassing export if disabled by cmd line
        if (g_options.export_pktid_interval <= 0) {
            return;
        }

        // in case we want to use the hashID as packet ID
        if (g_options.hashAsPacketID) {
            pkt_id = hash_id;
        } else {
            pkt_id = g_options.pktid_function(&packet_info->device->hash_buffer);
        }

        uint32_t          t_id = packet_info->device->template_id;
        
        t_id = (-1 == t_id) ? g_options.templateID : t_id;

        ipfix_template_t  *template = get_template( t_id );
        int               size = template->nfields;
        void              *fields[size];
        uint16_t          lengths[size];

        uint8_t ttl = 0;
        uint64_t timestamp = 0;
        uint16_t length = 0; // dummy for TS_TTL_PROTO template id
        uint16_t src_port = 0;
        uint16_t dst_port = 0;
        uint8_t *src_ipa = 0;
        uint8_t *dst_ipa = 0;

        switch (t_id) {
            case TS_ID:
            {
                timestamp = get_timestamp(packet_info->ts);

                int index = 0;
                index += set_value(&fields[index], &lengths[index], &timestamp, 8);
                index += set_value(&fields[index], &lengths[index], &hash_id, 4);
                break;
            }

            case MINT_ID:
            {
                timestamp = get_timestamp(packet_info->ts);
                ttl = get_ttl(packet, offsets[L_NET], layers[L_NET]);

                int index = 0;
                index += set_value(&fields[index], &lengths[index], &timestamp, 8);
                index += set_value(&fields[index], &lengths[index], &hash_id, 4);
                index += set_value(&fields[index], &lengths[index], &ttl, 1);
                break;
            }

            case TS_TTL_PROTO_ID:
            {
                timestamp = get_timestamp(packet_info->ts);
                ttl = get_ttl(packet, offsets[L_NET], layers[L_NET]);
                length = get_ip_length(packet, offsets[L_NET], layers[L_NET]);

                int index = 0;
                index += set_value(&fields[index], &lengths[index], &timestamp, 8);
                index += set_value(&fields[index], &lengths[index], &hash_id, 4);
                index += set_value(&fields[index], &lengths[index], &ttl, 1);
                index += set_value(&fields[index], &lengths[index], &length, 2);
                index += set_value(&fields[index], &lengths[index], &layers[L_TRANS], 1);
                index += set_value(&fields[index], &lengths[index], &layers[L_NET], 1);
                break;
            }

            case TS_TTL_PROTO_IP_ID:
            {
                timestamp = get_timestamp(packet_info->ts);
                ttl = get_ttl(packet, offsets[L_NET], layers[L_NET]);
                length = get_ip_length(packet, offsets[L_NET], layers[L_NET]);
                src_port = get_port(packet, offsets[L_TRANS], layers[L_TRANS]);
                dst_port = get_port(packet, offsets[L_TRANS] + 2, layers[L_TRANS]);
                src_ipa = get_ipa(packet, offsets[L_NET], layers[L_NET]);
                dst_ipa = get_ipa(packet, offsets[L_NET] + 4, layers[L_NET]);

                int index = 0;
                index += set_value(&fields[index], &lengths[index], &timestamp, 8);
                index += set_value(&fields[index], &lengths[index], &hash_id, 4);
                index += set_value(&fields[index], &lengths[index], &ttl, 1);
                index += set_value(&fields[index], &lengths[index], &length, 2);
                index += set_value(&fields[index], &lengths[index], &layers[L_TRANS], 1);
                index += set_value(&fields[index], &lengths[index], &layers[L_NET], 1);
                index += set_value(&fields[index], &lengths[index], src_ipa, 4);
                index += set_value(&fields[index], &lengths[index], &src_port, 2);
                index += set_value(&fields[index], &lengths[index], dst_ipa, 4);
                index += set_value(&fields[index], &lengths[index], &dst_port, 2);
                break;
            }
     
            case TS_ID_EPC_ID:
            {
                timestamp = get_timestamp(packet_info->ts);
                src_ipa = get_ipa(packet, offsets[L_NET], layers[L_NET]);
                src_port = get_port(packet, offsets[L_TRANS], layers[L_TRANS]);
                dst_ipa = get_ipa(packet, offsets[L_NET] + 4, layers[L_NET]);
                dst_port = get_port(packet, offsets[L_TRANS] + 2, layers[L_TRANS]);
                
                decode_raw(packet, packet->len-4);
                rule_id = decode_uint32(packet);
                
                int index = 0;
                index += set_value(&fields[index], &lengths[index], &timestamp, 8);
                index += set_value(&fields[index], &lengths[index], &hash_id, 4);
                index += set_value(&fields[index], &lengths[index], &rule_id, 4);
                index += set_value(&fields[index], &lengths[index], &layers[L_NET], 1);
                index += set_value(&fields[index], &lengths[index], src_ipa, 4);
                index += set_value(&fields[index], &lengths[index], &src_port, 2);
                index += set_value(&fields[index], &lengths[index], dst_ipa, 4);
                index += set_value(&fields[index], &lengths[index], &dst_port, 2);
                break;
            }

            default:
                LOGGER_info("!!!no template specified!!!");
                return;
        } // switch (options.templateID)

        //LOGGER_debug( "%d", size);
        //int i = 0;
        //for( i = 0; i < size; ++i ) {
        //   LOGGER_debug( "%p: %d: %d", fields[i], lengths[i], *( (int*)fields[i]));
        //}

        // send ipfix packet 
        if (0 > ipfix_export_array(ipfix(), template, size, fields, lengths)) {
            LOGGER_fatal("ipfix_export() failed: %s", strerror(errno));
        }

        // flush ipfix storage if max packetcount is reached
        if (packet_info->device->export_packet_count >= g_options.export_packet_count) {
            //todo: export_flush_device( packet_info->device );
            packet_info->device->export_packet_count = 0;
            export_flush();
        }


    } // if (hash in selection range)
}

void handle_open_epc_packet(packet_t *packet, packet_info_t *packet_info) {
    LOGGER_trace("Enter");

    uint32_t t_id = packet_info->device->template_id;
    t_id = (-1 == t_id) ? g_options.templateID : t_id;

    ipfix_template_t  *template = get_template( t_id );
    int size = template->nfields;
    void* fields[size];
    uint16_t lengths[size];
    int i;
    uint32_t dummy = 0;    
    uint8_t rule_flag     = 0;
    uint32_t rule_id      = 0;
    uint64_t timestamp    = 0;
    uint8_t src_ai_fam    = 0;
    uint8_t dst_ai_fam    = 0;
    uint8_t src_prefix    = 0;
    uint8_t dst_prefix    = 0;
    uint16_t src_port     = 0;
    uint16_t dst_port     = 0;
    uint16_t sdf_counter  = 0;
    uint32_t qci          = 0;
    uint32_t max_dl       = 0;
    uint32_t max_ul       = 0;
    uint32_t gua_dl       = 0;
    uint32_t gua_ul       = 0;
    uint32_t apn_dl       = 0;
    uint32_t apn_ul       = 0;
    packet_t apn          = {NULL, 0};
    packet_t rule_name    = {NULL, 0};
    packet_t imsi         = {NULL, 0};
    packet_t flow_desc    = {NULL, 0};
    packet_t src_ipa      = {NULL, 0};
    packet_t dst_ipa      = {NULL, 0};
    packet_t decode       = *packet;

    switch (t_id) {
        case TS_OPEN_EPC_ID:
        {
            //if (*((uint8_t*)packet) == OP_CODE) {
                rule_flag = decode_uint8(&decode);
                rule_id   = decode_uint32(&decode);
                imsi      = decode_array(&decode);
                apn       = decode_array(&decode);
                rule_name = decode_array(&decode);
                
                qci     = decode_uint32(&decode);
                max_dl  = decode_uint32(&decode);
                max_ul  = decode_uint32(&decode);
                gua_dl  = decode_uint32(&decode);
                gua_ul  = decode_uint32(&decode);
                apn_dl  = decode_uint32(&decode);
                apn_ul  = decode_uint32(&decode);
                
                /* TODO: Zeit richtig setzen da im Moment Microseconds zuerck
                 *       geliefert werden, wir aber Milliseconds fuer unser
                 *       Template brauchen
                 */
                //timestamp = get_timestamp(packet_info->ts); 
                timestamp = time(NULL);

                int index = 0;
                index += set_value(&fields[index],
                        &lengths[index], &timestamp, 8);
                index += set_value(&fields[index],
                        &lengths[index], &rule_flag, 1);
                index += set_value(&fields[index],
                        &lengths[index], &rule_id, 4);
                index += set_value(&fields[index],
                        &lengths[index], apn.ptr, apn.len);
                index += set_value(&fields[index],
                        &lengths[index], rule_name.ptr, rule_name.len);
                index += set_value(&fields[index],
                        &lengths[index], imsi.ptr, imsi.len);
                index += set_value(&fields[index], &lengths[index], &qci, 4);
                index += set_value(&fields[index], &lengths[index], &max_dl, 4);
                index += set_value(&fields[index], &lengths[index], &max_ul, 4);
                index += set_value(&fields[index], &lengths[index], &gua_dl, 4);
                index += set_value(&fields[index], &lengths[index], &gua_ul, 4);
                index += set_value(&fields[index], &lengths[index], &apn_dl, 4);
                index += set_value(&fields[index], &lengths[index], &apn_ul, 4);
                sdf_counter = decode_uint16(&decode);
                
                for (i = 0; i < sdf_counter; i++) {
                    int int_idx = index;

                    flow_desc  = decode_array(&decode);

                    src_ai_fam = decode_uint8(&decode);
                    src_prefix = decode_uint8(&decode);
                    src_port   = decode_uint16(&decode);

                    if (src_ai_fam == AF_INET) {
                        src_ipa = decode_raw(&decode, 4);
                    } else {
                        src_ipa.ptr = (uint8_t*)&dummy;
                        src_ipa.len = 4;
                    }

                    dst_ai_fam = decode_uint8(&decode);
                    dst_prefix = decode_uint8(&decode);
                    dst_port = decode_uint16(&decode);

                    if(dst_ai_fam == AF_INET) {
                        dst_ipa = decode_raw(&decode, 4);   
                    } else {
                        dst_ipa.ptr = (uint8_t*)&dummy;
                        dst_ipa.len = 4;
                    }
                    
                    int_idx += set_value(&fields[int_idx], &lengths[int_idx], src_ipa.ptr, src_ipa.len);
                    int_idx += set_value(&fields[int_idx], &lengths[int_idx], &src_port, 2);
                    int_idx += set_value(&fields[int_idx], &lengths[int_idx], dst_ipa.ptr, dst_ipa.len);
                    int_idx += set_value(&fields[int_idx], &lengths[int_idx], &dst_port, 2);

                    if (0 > ipfix_export_array(ipfix(), template, size, fields, lengths)) {
                        LOGGER_fatal("ipfix_export() failed: %s", strerror(errno));
                    }    
                }
                break;
            //}
        }
        
        default:
            LOGGER_info("!!!no template specified!!!");
            return;
    } // switch (options.templateID)

    export_flush();

    LOGGER_trace("Return");
    return;
}

void handle_packet(u_char *user_args, const struct pcap_pkthdr *header, const u_char * packet) {
    packet_t pkt = {(uint8_t*) packet, header->caplen};
    packet_info_t info = {header->ts, header->len, (device_dev_t*) user_args};

    LOGGER_trace("Enter");

    info.device->sampling_delta_count++;
    info.device->totalpacketcount++;

    // debug output
    if (0) print_array(pkt.ptr, pkt.len);

    if ((info.device->device_type == TYPE_SOCKET_UNIX ||
            info.device->device_type == TYPE_SOCKET_INET)
            && info.device->template_id == TS_OPEN_EPC_ID) {
        handle_open_epc_packet(&pkt, &info);
    } else {
        switch (info.device->device_type) {
            case TYPE_PCAP:
            case TYPE_PCAP_FILE:
                // get packet type from link layer header
                info.nettype = get_nettype(&pkt, info.device->link_type);
                break;

            case TYPE_SOCKET_UNIX:
            case TYPE_SOCKET_INET:
                info.nettype = get_nettype_pkt(&pkt);
                break;
            case TYPE_FILE:
            case TYPE_UNKNOWN:
            default:
                break;
        }
        LOGGER_trace("nettype: 0x%04X", info.nettype);

        // apply net offset - skip link layer header for further processing
        apply_offset(&pkt, info.device->pkt_offset);

        // apply user offset
        apply_offset(&pkt, g_options.offset);

        // debug output
        if (0) print_array(pkt.ptr, pkt.len);

        if (0x0800 == info.nettype || // IPv4
                0x86DD == info.nettype) // IPv6
        {
            if (0) print_ip4(pkt.ptr, pkt.len);
            handle_ip_packet(&pkt, &info);
            //LOGGER_trace( "drop" );
        } else {
            handle_default_packet(&pkt, &info);
        }
    }
    LOGGER_trace("Return");
}


// formaly known as handle_packet()
//void packet_pcap_cb(u_char *user_args, const struct pcap_pkthdr *header, const u_char * packet) {
//   device_dev_t* if_device = (device_dev_t*) user_args;
//   uint8_t  layers[4] = { 0 };
//   uint32_t hash_result;
//   int packet_len = header->caplen;
//   
//   LOGGER_trace("handle packet");
//   
//   if_device->sampling_delta_count++;
//   if_device->totalpacketcount++;
//
//   // debug output
//   if (0) print_array( packet, packet_len );
//   if (0) print_array( packet+if_device->offset[L_NET], packet_len-if_device->offset[L_NET] );
//
//   // selection of viable fields of the packet - depend on the selection function choosen
//   // locate protocolsections of ip-stack --> findHeaders() in hash.c
//   g_options.selection_function(packet, packet_len,
//         &if_device->hash_buffer,
//         if_device->offset, layers);
//
//   // !!!! no ip-stack found !!!!
//   if (0 == if_device->hash_buffer.len) {
//      LOGGER_trace( "Warning: packet does not contain Selection");
//      // todo: ?alternative selection function
//      // todo: ?for the whole configuration
//      // todo: ????drop????
//      return;
//   }
//   //   else {
//   //      LOGGER_warn( "Warnig: packet contain Selection (%d)", copiedbytes);
//   //   }
//
//   // hash the chosen packet data
//   hash_result = g_options.hash_function(&if_device->hash_buffer);
//   //LOGGER_trace( "hash result: 0x%04X", hash_result );
//
//   // hash result must be in the chosen selection range to count
//   if ((g_options.sel_range_min <= hash_result)
//         && (g_options.sel_range_max >= hash_result))
//   {
//      uint8_t  ttl;
//      uint64_t timestamp;
//
//      if_device->export_packet_count++;
//      if_device->sampling_size++;
//
//      // bypassing export if disabled by cmd line
//      if (g_options.export_pktid_interval <= 0) {
//         return;
//      }
//
//      int pktid = 0;
//      // in case we want to use the hashID as packet ID
//      if (g_options.hashAsPacketID == 1) {
//         pktid = hash_result;
//      } else {
//         pktid = g_options.pktid_function(&if_device->hash_buffer);
//      }
//
//      ttl       = get_ttl(packet, packet_len, if_device->offset[L_NET], layers[L_NET]);
//      timestamp = get_timestamp(header->ts);
//
//      ipfix_template_t* template = if_device->ipfixtmpl_min;
//      switch (g_options.templateID) {
//      case MINT_ID:
//         template = if_device->ipfixtmpl_min;
//         break;
//      case TS_ID:
//         template = if_device->ipfixtmpl_ts;
//         break;
//      case TS_TTL_PROTO_ID:
//         template = if_device->ipfixtmpl_ts_ttl;
//         break;
//      case TS_TTL_PROTO_IP_ID:
//         template = if_device->ipfixtmpl_ts_ttl_ip;
//         break;
//      default:
//         LOGGER_info( "!!!no template specified!!!" );
//         return;
//         break;
//      }
//      int               size     = template->nfields;
//      void*             fields[size];
//      uint16_t          lengths[size];
//
//      uint16_t length; // dummy for TS_TTL_PROTO template id
//
////      set_hash( );
////      set_timestamp();
////      set_ip_ttl();
////      set_ip_version();
////      set_ip_length();
////      set_ip_id();
//      
//      switch (g_options.templateID) {
//      case TS_ID: {
//         int index = 0;
//         index += set_value( &fields[index], &lengths[index], &timestamp, 8);
//         index += set_value( &fields[index], &lengths[index], &hash_result, 4);
//         break;
//      }
//
//      case MINT_ID: {
//         int index = 0;
//         index += set_value( &fields[index], &lengths[index], &timestamp, 8);
//         index += set_value( &fields[index], &lengths[index], &hash_result, 4);
//         index += set_value( &fields[index], &lengths[index], &ttl, 1);
//         break;
//      }
//
//      case TS_TTL_PROTO_ID: {
//         if (layers[L_NET] == N_IP) {
//            length = ntohs(*((uint16_t*) (&packet[if_device->offset[L_NET] + 2])));
//         } else if (layers[L_NET] == N_IP6) {
//            length = ntohs(*((uint16_t*) (&packet[if_device->offset[L_NET] + 4])));
//         } else {
//            LOGGER_fatal( "cannot parse packet length" );
//            length = 0;
//         }
//
//         int index = 0;
//         index += set_value( &fields[index], &lengths[index], &timestamp, 8);
//         index += set_value( &fields[index], &lengths[index], &hash_result, 4);
//         index += set_value( &fields[index], &lengths[index], &ttl, 1);
//         index += set_value( &fields[index], &lengths[index], &length, 2);
//         index += set_value( &fields[index], &lengths[index], &layers[L_TRANS], 1);
//         index += set_value( &fields[index], &lengths[index], &layers[L_NET], 1);
//         break;
//      }
//
//      case TS_TTL_PROTO_IP_ID: {          
//          if (layers[L_NET] == N_IP) {
//              length = ntohs(*((uint16_t*) (&packet[if_device->offset[L_NET] + 2])));  
//          } else if (layers[L_NET] == N_IP6) {
//              length = ntohs(*((uint16_t*) (&packet[if_device->offset[L_NET] + 4])));
//          } else {
//              LOGGER_fatal( "cannot parse packet length" );
//              length = 0;
//          }
//          
//          int index = 0;
//          index += set_value( &fields[index], &lengths[index], &timestamp, 8);
//          index += set_value( &fields[index], &lengths[index], &hash_result, 4);
//          index += set_value( &fields[index], &lengths[index], &ttl, 1);
//          index += set_value( &fields[index], &lengths[index], &length, 2);
//          index += set_value( &fields[index], &lengths[index], &layers[L_TRANS], 1);
//          index += set_value( &fields[index], &lengths[index], &layers[L_NET], 1);
//          // this needs to be IPv4 and UDP or TCP or SCTP (not yet supported)
//          // TODO: switch template members
//          uint32_t ipa = 0;
//          if( N_IP == layers[L_NET] ) {
//             index += set_value( &fields[index], &lengths[index], 
//                   (uint32_t*) &packet[if_device->offset[L_NET] + 12], 4);
//          }
//          else {
//             index += set_value( &fields[index], &lengths[index], &ipa, 4);
//          }
//          uint16_t port = 0;
//          switch( layers[L_TRANS] ) {
//             case T_TCP:
//             case T_UDP:
//             //case T_SCTP:
//                port = ntohs(*((uint16_t*) &packet[if_device->offset[L_TRANS]]));
//                break;
//             default:
//                port = 0;
//          }
//          index += set_value( &fields[index], &lengths[index], &port, 2);
//          if( N_IP == layers[L_NET] ) {
//             index += set_value( &fields[index], &lengths[index], 
//                   (uint32_t*) &packet[if_device->offset[L_NET] + 16], 4);
//          }
//          else {
//             index += set_value( &fields[index], &lengths[index], &ipa, 4);
//          }
//          switch( layers[L_TRANS] ) {
//             case T_TCP:
//             case T_UDP:
//             //case T_SCTP:
//                port = ntohs(*((uint16_t*) &packet[if_device->offset[L_TRANS] + 2]));
//                break;
//             default:
//                port = 0;
//          }
//          index += set_value( &fields[index], &lengths[index], &port, 2);
//          break;
//      }
//
//      default:
//         LOGGER_info( "!!!no template specified!!!" );
//         return;
//      } // switch (options.templateID)
//
//      //LOGGER_debug( "%d", size);
//      //int i = 0;
//      //for( i = 0; i < size; ++i ) {
//      //   LOGGER_debug( "%p: %d: %d", fields[i], lengths[i], *( (int*)fields[i]));
//      //}
//
//      // send ipfix packet 
//      if (0 > ipfix_export_array(ipfix(), template, size, fields, lengths)) {
//         LOGGER_fatal( "ipfix_export() failed: %s", strerror(errno));
//         exit(1);
//      }
//
//      // flush ipfix storage if max packetcount is reached
//      if (if_device->export_packet_count >= g_options.export_packet_count) {
//         //todo: export_flush_device( if_device );
//         if_device->export_packet_count = 0;
//         export_flush();
//      }
//
//   } // if((options.sel_range_min < hash_result) && (options.sel_range_max > hash_result))
////   else {
////      LOGGER_info( "INFO: drop packet; hash not in selection range");
////   }
//}
#endif

/**
 * initial cb function;
 * selection of runtime configuration commands
 * command: "mid: <id> -<cmd> <value>
 * @param cmd string
 *
 * returns: 1 consumed, 0 otherwise
 */
int runtime_configuration_cb(char* conf_msg) {
    unsigned long mID = 0; // session id
    int matches;

    LOGGER_debug("configuration message received: '%s'", conf_msg);
    // check prefix: "mid: <id>"
    matches = sscanf(conf_msg, "mid: %lu ", &mID);
    if (1 == matches) {
        LOGGER_debug("Message ID: %lu", mID);

        // fetch command from string starting with hyphen '-'
        char cmd = '?';
        int length = strlen(conf_msg);

        int i = 0;
        for (i = 0; i < length; ++i, ++conf_msg) {
            if ('-' == *conf_msg) {
                // get command
                ++conf_msg;
                cmd = *conf_msg;
                ++conf_msg;

                // remove leading whitespaces
                while (isspace(*conf_msg))
                    ++conf_msg;

                // execute command
                LOGGER_debug("configuration command '%c': %s", cmd, conf_msg);

                char* rsp_msg = (*getFunction(cmd))(mID, conf_msg);

                int i;
                for (i = 0; i < g_options.number_interfaces; i++) {
                    LOGGER_debug("==> %s", rsp_msg);
                    export_data_sync(&if_devices[i]
                            , ev_now(EV_DEFAULT) * 1000
                            , mID
                            , 0
                            , rsp_msg);
                }
                return NETCON_CMD_MATCHED;
            }
        }
    }

    return NETCON_CMD_UNKNOWN;
}

/**
 * send available command
 * command: h,?
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_help(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);
    static char* response = NULL;

    cfg_fct_t* cfg_f = get_cfg_fct(*msg);
    if (NULL != cfg_f) {
        return (char*) cfg_f->desc;
    }
    else {
        if (NULL == response) {
            int i;
            int size = 1;
            int length = sizeof (configuration_fct) / sizeof (cfg_fct_t);

            for (i = 0; i < length; ++i) {
                size += strlen(configuration_fct[i].desc);
            }
            response = (char*) malloc(size + 1);

            char* tmp = response;
            for (i = 0; i < length; ++i) {
                strcpy(tmp, configuration_fct[i].desc);
                tmp += strlen(configuration_fct[i].desc);
            }
        }
        return response;
    }
}

/**
 * command: t <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_template(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    uint32_t t_id = parse_template(msg);
    if (-1 == t_id) {
        LOGGER_warn("unknown template: %s", msg);
        SET_CFG_RESPONSE("INFO: unknown template: %s", msg);
    }
    else {
        // TODO: handling for different devices
        int i = 0;
        for (i = 0; i < getOptions()->number_interfaces; ++i) {
            // reset all device specific templates
            if_devices[i].template_id = -1;
        }
        getOptions()->templateID = t_id;
        SET_CFG_RESPONSE("INFO: new template set: %s", msg);
    }
    return CFG_RESPONSE;
}

/**
 * command: f <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_filter(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    if (-1 == set_all_filter(msg)) {
        LOGGER_error("error setting filter: %s", msg);
        SET_CFG_RESPONSE("INFO: error setting filter: %s", msg);
    }
    else {
        SET_CFG_RESPONSE("INFO: new filter expression set: %s", msg);
    }
    return CFG_RESPONSE;
}

/**
 * command: J <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_export_to_probestats(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    int new_timeout = strtol(msg, NULL, 0);
    if (0 <= new_timeout) {
        events.export_timer_stats.repeat = new_timeout;
        ev_timer_again(EV_DEFAULT, &events.export_timer_stats);

        SET_CFG_RESPONSE("INFO: new probestats export timeout set: %s", msg);
    } else {
        SET_CFG_RESPONSE("INFO: probestats export timeout NOT changed");
    }
    return CFG_RESPONSE;
}

/**
 * command: K <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_export_to_ifstats(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    int new_timeout = strtol(msg, NULL, 0);
    if (0 <= new_timeout) {
        events.export_timer_sampling.repeat = new_timeout;
        ev_timer_again(EV_DEFAULT, &events.export_timer_sampling);

        SET_CFG_RESPONSE("INFO: new ifstats export timeout set: %s", msg);
    } else {
        SET_CFG_RESPONSE("INFO: ifstats export timeout NOT changed");
    }
    return CFG_RESPONSE;
}

/**
 * command: I <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_export_to_pktid(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    int new_timeout = strtol(msg, NULL, 0);
    if (0 <= new_timeout) {
        events.export_timer_pkid.repeat = new_timeout;
        ev_timer_again(EV_DEFAULT, &events.export_timer_pkid);

        SET_CFG_RESPONSE("INFO: new packet export timeout set: %s", msg);
    } else {
        SET_CFG_RESPONSE("INFO: packet export timeout NOT changed");
    }
    return CFG_RESPONSE;
}

/**
 * command: m <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_min_selection(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    uint32_t value = set_sampling_lowerbound(&g_options, msg);
    SET_CFG_RESPONSE("INFO: minimum selection range set: %d", value);

    return CFG_RESPONSE;
}

/**
 * command: M <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_max_selection(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    uint32_t value = set_sampling_upperbound(&g_options, msg);
    SET_CFG_RESPONSE("INFO: maximum selection range set: %d", value);

    return CFG_RESPONSE;
}

/**
 * command: r <value>
 * returns: 1 consumed, 0 otherwise
 */
char* configuration_set_ratio(unsigned long mid, char *msg) {
    LOGGER_debug("Message ID: %lu", mid);

    /* currently sampling ratio is equal for all devices */
    if (-1 == set_sampling_ratio(&g_options, msg)) {
        LOGGER_error("error setting sampling ration: %s", msg);
        SET_CFG_RESPONSE("INFO: error setting sampling ration: %s", msg);
    }
    else {
        SET_CFG_RESPONSE("INFO: new sampling ratio set: %s", msg);
    }
    return CFG_RESPONSE;
}

/*-----------------------------------------------------------------------------
  Export
  -----------------------------------------------------------------------------*/
void export_data_interface_stats(device_dev_t *dev,
        uint64_t observationTimeMilliseconds, u_int32_t size,
        u_int64_t deltaCount) {
    static uint16_t lengths[] = {8, 4, 8, 4, 4, 0, 0};
    static char interfaceDescription[16];
#ifndef PFRING
    struct pcap_stat pcapStat;
    void* fields[] = {&observationTimeMilliseconds, &size, &deltaCount,
        &pcapStat.ps_recv, &pcapStat.ps_drop, dev->device_name,
        interfaceDescription};
#else
    pfring_stat pfringStat;
    void* fields[] = {&observationTimeMilliseconds, &size, &deltaCount
        , &pfringStat.recv
        , &pfringStat.drop
        , dev->device_name
        , interfaceDescription};
#endif

    snprintf(interfaceDescription, sizeof (interfaceDescription), "%s",
            ntoa(dev->IPv4address));
    lengths[5] = strlen(dev->device_name);
    lengths[6] = strlen(interfaceDescription);

#ifndef PFRING
    /* Get pcap statistics in case of live capture */
    if (TYPE_PCAP == dev->device_type) {
        if (pcap_stats(dev->device_handle.pcap, &pcapStat) < 0) {
            LOGGER_error("Error DeviceNo   %s: %s", dev->device_name,
                    pcap_geterr(dev->device_handle.pcap));
        }
    } else {
        pcapStat.ps_drop = 0;
        pcapStat.ps_recv = 0;
    }
#else
    if (TYPE_PFRING == dev->device_type) {
        if (pfring_stats(dev->device_handle.pfring, &pfringStat) < 0) {
            LOGGER_error("Error DeviceNo   %s: Failed to get statistics",
                    dev->device_name);
        }
    } else {
        pfringStat.drop = 0;
        pfringStat.recv = 0;
    }
#endif

    LOGGER_trace("sampling: (%d, %lu)", size, (long unsigned) deltaCount);
    if (ipfix_export_array(ipfix(), get_template(INTF_STATS_ID), 7,
            fields, lengths) < 0) {
        LOGGER_error("ipfix export failed: %s", strerror(errno));
    } else {
        dev->sampling_size = 0;
        dev->sampling_delta_count = 0;
    }
}

void export_data_sync(device_dev_t *dev, int64_t observationTimeMilliseconds,
        u_int32_t messageId, u_int32_t messageValue, char * message) {
    static uint16_t lengths[] = {8, 4, 4, 0};
    lengths[3] = strlen(message);
    void *fields[] = {&observationTimeMilliseconds, &messageId, &messageValue,
        message};
    LOGGER_debug("export data sync");
    if (ipfix_export_array(ipfix(), get_template(SYNC_ID), 4, fields,
            lengths) < 0) {
        LOGGER_error("ipfix export failed: %s", strerror(errno));
        return;
    }
    if (ipfix_export_flush(ipfix()) < 0) {
        LOGGER_error("Could not export IPFIX (flush) ");
    }

}

void export_data_probe_stats(int64_t observationTimeMilliseconds) {
    static uint16_t lengths[] = {8, 4, 8, 4, 4, 8, 8, 8};
    struct probe_stat probeStat;

    void *fields[] = { &probeStat.observationTimeMilliseconds
                     , &probeStat.systemCpuIdle
                     , &probeStat.systemMemFree
                     , &probeStat.processCpuUser
                     , &probeStat.processCpuSys
                     , &probeStat.processMemVzs
                     , &probeStat.processMemRss
                     , &probeStat.systemMemTotal
                     };

    ipfix_template_t* t = get_template(PROBE_STATS_ID);

    probeStat.observationTimeMilliseconds = observationTimeMilliseconds;
    get_probe_stats(&probeStat);

    if (ipfix_export_array(ipfix(), t, t->nfields, fields, lengths) < 0) {
        LOGGER_error("ipfix export failed: %s", strerror(errno));
        return;
    }
    if (ipfix_export_flush(ipfix()) < 0) {
        LOGGER_error("Could not export IPFIX (flush) ");
    }
}

void export_data_location(int64_t observationTimeMilliseconds) {
    static uint16_t lengths[] = {8, 4, 0, 0, 0, 0};
    lengths[2] = strlen(getOptions()->s_latitude);
    lengths[3] = strlen(getOptions()->s_longitude);
    lengths[4] = strlen(getOptions()->s_probe_name);
    lengths[5] = strlen(getOptions()->s_location_name);
    void *fields[] = {&observationTimeMilliseconds, &getOptions()->ipAddress,
        getOptions()->s_latitude, getOptions()->s_longitude,
        getOptions()->s_probe_name, getOptions()->s_location_name};

    LOGGER_debug("export data location");
    //LOGGER_fatal("%s; %s",getOptions()->s_latitude, getOptions()->s_longitude );
    if (ipfix_export_array(ipfix(), get_template(LOCATION_ID),
            sizeof (lengths) / sizeof (lengths[0]), fields, lengths) < 0) {
        LOGGER_error("ipfix export failed: %s", strerror(errno));
        return;
    }
    if (ipfix_export_flush(ipfix()) < 0) {
        LOGGER_error("Could not export IPFIX (flush) ");
    }
}

/**
 * This causes libipfix to send cached messages to
 * the registered collectors.
 *
 * flushes each device
 */
void export_flush() {
    int i;
    LOGGER_trace("export_flush");
    for (i = 0; i < g_options.number_interfaces; i++) {
        if (ipfix_export_flush(ipfix()) < 0) {
            LOGGER_error("Could not export IPFIX, device: %d", i);
            //         ipfix_reconnect();
            break;
        }
    }
}

void export_flush_all() {
    int i;
    LOGGER_trace("export_flush_all");
    for (i = 0; i < g_options.number_interfaces; i++) {
        export_flush_device(&if_devices[i]);
    }
}

void export_flush_device(device_dev_t* device) {
    LOGGER_trace("export_flush_device");
    if (0 != device) {
        device->export_packet_count = 0;
        if (ipfix_export_flush(ipfix()) < 0) {
            LOGGER_error("Could not export IPFIX: %s", device->device_name);
            //         ipfix_reconnect();
        }
    }
}

/**
 * Periodically called each export time interval.
 */
void export_timer_pktid_cb(EV_P_ ev_timer *w, int revents) {
    LOGGER_trace("export timer tick");
    export_flush();
}

/**
 * Peridically called each export/sampling time interval
 */
void export_timer_sampling_cb(EV_P_ ev_timer *w, int revents) {
    int i;
    uint64_t observationTimeMilliseconds;
    LOGGER_trace("export timer sampling call back");
    observationTimeMilliseconds = (uint64_t) ev_now(EV_A) * 1000;
    for (i = 0; i < g_options.number_interfaces; i++) {
        device_dev_t *dev = &if_devices[i];
        export_data_interface_stats(dev, observationTimeMilliseconds, dev->sampling_size, dev->sampling_delta_count);
#ifdef PFRING
#ifdef PFRING_STATS
        print_stats(dev);
#endif
#endif
    }
    export_flush();
}

void export_timer_stats_cb(EV_P_ ev_timer *w, int revents) {
    LOGGER_trace("export timer probe stats call back");
    export_data_probe_stats( (uint64_t) ev_now(EV_A) * 1000 );
}

/**
 * Peridically called
 */
void export_timer_location_cb(EV_P_ ev_timer *w, int revents) {
    LOGGER_trace("export timer location call back");
    export_data_location( (uint64_t) ev_now(EV_A) * 1000 );
}

/**
 * Periodically checks ipfix export fd and reconnects it
 * to netcon
 */
void resync_timer_cb(EV_P_ ev_timer *w, int revents) {
   ipfix_collector_sync_t *col;

   col = (ipfix_collector_sync_t*) (ipfix()->collectors);
   LOGGER_debug("collector_fd: %d", col->fd);
   netcon_resync(EV_A_ col->fd);
}

