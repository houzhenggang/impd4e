/**
 * @file
 * @brief parse command line, event handing, control functions.
 */
/* impd4e - a small network probe which allows to monitor and sample datagrams
 * from the network and exports hash-based packet IDs over IPFIX
 * Copyright (c) 2010, Fraunhofer FOKUS (Carsten Schmoll) & TU-Berlin (Christian Henke)
 * This program is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation;
 *  either version 3 of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.

 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <unistd.h>
#include <pcap.h>
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

#include <sys/sysinfo.h> /* TODO review: sysinfo is Linux only */
#include <sys/times.h>

#include "templates.h"
#include "main.h"
#include "hash.h"
#include "mlog.h"
#include "ipfix.h"
#include "ipfix_fields_fokus.h"
#include "stats.h"

// Custom logger
#include "logger.h"
#include "netcon.h"
#include <ev.h> // event loop
/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------*/
#define RESYNC_PERIOD 1.5 /* seconds */

/*----------------------------------------------------------------------------
 Globals
 ----------------------------------------------------------------------------- */

/**
 * Event and Signal handling via libev
 */
struct {
	struct ev_loop *loop;
	ev_signal sigint_watcher;
	ev_signal sigalrm_watcher;
	ev_signal sigpipe_watcher;
	ev_timer export_timer_pkid;
	ev_timer export_timer_sampling;
	ev_timer export_timer_stats;
	ev_timer resync_timer;
	ev_io *packet_watchers;
} events;

options_t g_options;
static char pcap_errbuf[PCAP_ERRBUF_SIZE];
pcap_dev_t *pcap_devices;

/* sync extension, depends on ipfix_collector_t defined in libipfix */
typedef struct collector_node_sync {
	struct collector_node_sync *next;
	int usecount;
	char *chost; /* collector hostname */
	int cport; /* collector port */
	ipfix_proto_t protocol; /* used protocol (e.g. tcp) */
	int fd; /* open socket */
} ipfix_collector_sync_t;

/*----------------------------------------------------------------------------
 Prototypes
 ----------------------------------------------------------------------------- */
/* -- signals --*/
static void sigint_cb (EV_P_ ev_signal *w, int revents);
static void sigalrm_cb (EV_P_ ev_signal *w, int revents);

/* -- capture --*/
static void packet_watcher_cb(EV_P_ ev_io *w, int revents);
static void packet_pcap_cb(u_char *user_args, const struct pcap_pkthdr *header,
		const u_char * packet);
/* -- export -- */
static void export_flush();
static void export_timer_pktid_cb (EV_P_ ev_timer *w, int revents);
static void export_timer_sampling_cb (EV_P_ ev_timer *w, int revents);
static void export_timer_stats_cb (EV_P_ ev_timer *w, int revents);
static void export_data_interface_stats(pcap_dev_t *dev,
		uint64_t observationTimeMilliseconds, u_int32_t size,
		u_int64_t deltaCount);
static void export_data_probe_stats(pcap_dev_t *dev);
static void export_data_sync(pcap_dev_t *dev,
		int64_t observationTimeMilliseconds, u_int32_t messageId,
		u_int32_t messageValue, char * message);

/* -- netcon / resync  -- */
void init_libipfix(pcap_dev_t *pcap_devices, options_t *options);
static void ipfix_reconnect();
static void resync_timer_cb (EV_P_ ev_timer *w, int revents);

/**
 * Print out command usage
 */
void print_help() {
	printf(
			"impd4e - a libpcap based measuring probe which uses hash-based packet\n"
				"         selection and exports packetIDs via IPFIX to a collector.\n\n"
				"USAGE: impd4e -i interface [options] \n"
				"\n");
	printf(
			"options: \n"
			"   -C  <collector IP> \n"
			"   -c  <export packet count>      size of export buffer after which packets\n"
			"                                  are flushed (per device)\n"
			"   -f  <bpf>                      Berkeley Packet Filter expression (e.g. \n"
			"                                  tcp udp icmp)\n"
			"   -F  <hash_function>            hash function to use \"BOB\", \"OAAT\", \n"
			"                                  \"TWMX\", \"HSIEH\"\n"
			"   -h                             print this help \n"
			"   -I  <interval>                 pktid export interval in seconds. Use 0 for \n"
			"                                  disabling pkid export. Ex. -I 1.5  \n"
			"   -i  <interface>                interface(s) to listen on. It can be used \n"
			"                                  multiple times.   \n"
			"   -J  <interval>                 probe stats export interval in seconds. \n"
			"                                  Measurement is done at each elapsed interval. \n"
			"                                  Use -J 0 for disabling this export.\n"
			"                                  Default: 30.0 \n"
			"\n"
			"   -K  <interval>                 interface stats export interval in seconds. \n"
			"                                  Measurement is done at each elapsed interval. \n"
			"                                  Use -K 0 for disabling this export.\n"
			"                                  Default: 10.0 \n"
			"\n"
			"   -M  <maximum selection range>  integer - do not use in conjunction with -r \n"
			"   -m  <minimum selection range>  integer - do not use in conjunction with -r \n"
			"   -o  <observation domain id>    identification of the interface in\n"
			"                                  the IPFIX Header\n"
			"   -P  <collector port> \n"
			"   -p  <hash function>            use different hash_function for packetID\n"
			"                                  generation: \"BOB\", \"OAAT\", \"TWMX\", \"HSIEH\" \n"
			"   -r  <sampling ratio>           in %% (double)\n"
			"   -s  <selection function>       which parts of the header used for hashing\n"
			"                                  either \"IP+TP\", \"IP\", \"REC8\", \"PACKET\" \n"
			"   -t  <template>                 either \"min\" or \"lp\"\n"
			"   -u                             use only one oid from the first interface \n"
			"   -v  verbose-level              can be used multiple times to increase output \n\n");

}

static void ipfix_reconnect() {
	int i;
	LOGGER_info("trying to reconnect ");
	for (i = 0; i < g_options.number_interfaces; i++) {
		ipfix_export_flush(pcap_devices[i].ipfixhandle);
		ipfix_close(pcap_devices[i].ipfixhandle);
	}
	ipfix_cleanup();
	init_libipfix(pcap_devices, &g_options);

}
/**
 * Shutdown impd4e
 */
static void impd4e_shutdown() {
	int i;
	LOGGER_info("Shutting down..");
	for (i = 0; i < g_options.number_interfaces; i++) {
		ipfix_export_flush(pcap_devices[i].ipfixhandle);
		ipfix_close(pcap_devices[i].ipfixhandle);
	}
	ipfix_cleanup();
	free(pcap_devices);

}

/* the signal handler for SIGINT == Ctrl-C --> shutdown program */

/**
 * Call back for SIGINT (Ctrl-C). It breaks all loops
 * and leads to shutdown.
 */
static void sigint_cb (EV_P_ ev_signal *w, int revents) {
	LOGGER_info("Signal INT received");
	ev_unloop (events.loop, EVUNLOOP_ALL);
}
static void sigpipe_cb (EV_P_ ev_signal *w, int revents) {
	LOGGER_info("Ignoring SIGPIPE, libipfix should indefinitely try to reconnect to collector.");

}
/**
 * SIGALRM call back, currently not used.
 */
static void sigalrm_cb (EV_P_ ev_signal *w, int revents) {
	LOGGER_info("Signal ALRM received");

}

/**
 * Set default options
 */
static void options_set_defaults(options_t *options) {
	options->number_interfaces = 0;
	options->bpf = NULL;
	options->templateID = MINT_ID;
	options->collectorPort = 4739;
	strcpy(options->collectorIP, "localhost");
	options->observationDomainID = 0;
	options->hash_function = calcHashValue_BOB;
	options->selection_function = copyFields_U_TCP_and_Net;
	options->sel_range_min = 0x19999999; // (2^32 / 10)
	options->sel_range_max = 0x33333333; // (2^32 / 5)
	options->snapLength = 80;
	options->verbosity = 0;
	options->export_packet_count = 1000;
	options->export_pktid_interval = 3.0; /* seconds */
	options->export_sampling_interval = 10.0; /* seconds */
	options->export_stats_interval = 30.0; /* seconds */

	options->hashAsPacketID = 1;
	options->file = NULL;
	options->use_oid_first_interface = 0;

	//	options->samplingResultExport = false;
	//	options->export_sysinfo = false;
}
/**
 * Parse command line hash function
 */
hashFunction parseFunction(char *arg_string, options_t *options) {
	int k;
	int j = 0;
	struct hashfunction {
		char *hstring;
		hashFunction function;
	} hashfunctions[] = { { HASH_FUNCTION_BOB, calcHashValue_BOB }, {
			HASH_FUNCTION_TWMX, calcHashValue_TWMXRSHash }, {
			HASH_FUNCTION_HSIEH, calcHashValue_Hsieh }, { HASH_FUNCTION_OAAT,
			calcHashValue_OAAT } };

	for (k = 0; k < (sizeof(hashfunctions) / sizeof(struct hashfunction)); k++) {
		if (strncasecmp(arg_string, hashfunctions[k].hstring, strlen(
				hashfunctions[k].hstring)) == 0) {
			j = k;
			LOGGER_info("using %s as hashFunction \n", hashfunctions[k].hstring);

		}
	}
	return hashfunctions[j].function;
}
/**
 * Parse command line selection function
 */
void parseSelFunction(char *arg_string, options_t *options) {
	int k;
	struct selfunction {
		char *hstring;
		selectionFunction selfunction;
	} selfunctions[] = { { HASH_INPUT_REC8, copyFields_Rec }, { HASH_INPUT_IP,
			copyFields_Only_Net },
			{ HASH_INPUT_IPTP, copyFields_U_TCP_and_Net }, { HASH_INPUT_PACKET,
					copyFields_Packet } };

	for (k = 0; k < (sizeof(selfunctions) / sizeof(struct selfunction)); k++) {
		if (strncasecmp(arg_string, selfunctions[k].hstring, strlen(
				selfunctions[k].hstring)) == 0) {
			options->selection_function = selfunctions[k].selfunction;
		}
	}
}
/**
 * Parse command line template
 */
void parseTemplate(char *arg_string, options_t *options) {
	int k;
	struct templateDef {
		char *hstring;
		int templateID;
	} templates[] = { { MIN_NAME, MINT_ID }, { TS_TTL_RROTO_NAME,
			TS_TTL_PROTO_ID }, };

	for (k = 0; k < (sizeof(templates) / sizeof(struct templateDef)); k++) {
		if (strncasecmp(arg_string, templates[k].hstring, strlen(
				templates[k].hstring)) == 0) {
			options->templateID = templates[k].templateID;
		}
	}
}
/**
 * Set sampling ratio, returns -1 in case of failure.
 */
int sampling_set_ratio(options_t *options, double sampling_ratio) {
	LOGGER_debug("sampling ratio: %lf", sampling_ratio);
	/*
	 * for the sampling ratio we do not like values at the edge, therefore we use values beginning at the 10% slice.
	 */
	options->sel_range_min = 0x19999999;
	options->sel_range_max = (double) UINT32_MAX / 100 * sampling_ratio;

	if (UINT32_MAX - options->sel_range_max > options->sel_range_min) {
		options->sel_range_min = 0x19999999;
		options->sel_range_max += options->sel_range_min;
	} else {
		/* more than 90% therefore use also values from first 10% slice */
		options->sel_range_min = UINT32_MAX - options->sel_range_max;
		options->sel_range_max = UINT32_MAX;
	}
	return 0;
}
/**
 * Process command line arguments
 */
void parse_cmdline(options_t *options, int argc, char **argv) {

	int c;
	char par[] = "vJ:K:i:I:o:r:t:f:m:M:hs:F:c:P:C:R:nSu";
	char *endptr;
	errno = 0;
	double sampling_ratio;

	options->number_interfaces = 0;

	while ((c = getopt(argc, argv, par)) != -1) {
		switch (c) {
		case 'C':
			/* collector port */
			strcpy(options->collectorIP, optarg);
			break;
		case 'c': /* export count */
			options->export_packet_count = atoi(optarg);
			break;
		case 'f':
			options->bpf = strdup(optarg);
			break;
		case 'h':
			print_help();
			exit(0);
			break;
		case 'i':
			if (options->number_interfaces == MAX_INTERFACES) {
				mlogf(ALWAYS, "specify at most %d interfaces with -i",
						MAX_INTERFACES);
				exit(1);
			}
			options->if_names[options->number_interfaces++] = strdup(optarg);
			break;
		case 'I':
			options->export_pktid_interval = atof(optarg);
			break;
		case 'J':
			options->export_stats_interval = atof(optarg);
			break;
		case 'K':
			options->export_sampling_interval = atof(optarg);
			break;

		case 'o':
			options->observationDomainID = atoi(optarg);
			break;
		case 't':
			parseTemplate(optarg, options);
			break;
		case 'm':
			options->sel_range_min = strtoll(optarg, &endptr, 0);
			if ((*endptr != '\0') || (errno == ERANGE
					&& (options->sel_range_min == LONG_MAX
							|| options->sel_range_min == LONG_MIN)) || (errno
					!= 0 && options->sel_range_min == 0)) {
				mlogf(ALWAYS,
						"error parsing selection_miminum_range - needs to be (uint32_t) \n");
				exit(1);
			}
			break;
		case 'M':
			options->sel_range_max = strtoll(optarg, NULL, 0);
			if ((*endptr != '\0') || (errno == ERANGE
					&& (options->sel_range_max == LONG_MAX
							|| options->sel_range_max == LONG_MIN)) || (errno
					!= 0 && options->sel_range_max == 0)) {
				mlogf(ALWAYS,
						"error parsing selection_maximum_range - needs to be (uint32_t) \n");
				exit(1);
			}
			break;
		case 's':
			parseSelFunction(optarg, options);
			break;
		case 'F':
			options->hash_function = parseFunction(optarg, options);
			break;
		case 'p':
			options->pktid_function = parseFunction(optarg, options);
			options->hashAsPacketID = 0;
			break;
		case 'P':
			if ((options->collectorPort = atoi(optarg)) < 0) {
				mlogf(ALWAYS, "Invalid -p argument!\n");
				exit(1);
			}
			break;
		case 'v':
			mlog_set_vlevel(options->verbosity++);
			break;
		case 'l':
			options->snapLength = atoi(optarg);
			break;
		case 'r':
			sscanf(optarg, "%lf", &sampling_ratio);
			sampling_set_ratio(options, sampling_ratio);
			break;
		case 'R':
			options->file = strdup(optarg);
			options->number_interfaces++;
			break;
		case 'u':
			options->use_oid_first_interface=1;
			break;
		case 'n':
			// TODO parse enable export sampling
			break;
		case 'S':
			// TODO
			//			options->export_sysinfo = true;
			break;
		default:
			printf("unknown parameter: %d \n", c);
			break;
		}

	}

}
/**
 * Helper for printing out IPv4 address
 */
char *htoa(uint32_t ipaddr) {
	static char addrstr[16];
	ipaddr = htonl(ipaddr);
	uint8_t *p = (uint8_t*) &ipaddr;
	sprintf(addrstr, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
	return addrstr;
}

void determineLinkType(pcap_dev_t *pcap_device) {

	pcap_device->link_type = pcap_datalink(pcap_device->pcap_handle);
	switch (pcap_device->link_type) {
	case DLT_EN10MB:
		pcap_device->offset[L_NET] = 14;
		LOGGER_info("dltype: DLT_EN10M");
		break;
	case DLT_ATM_RFC1483:
		pcap_device->offset[L_NET] = 8;
		LOGGER_info("dltype: DLT_ATM_RFC1483");
		break;
	case DLT_LINUX_SLL:
		pcap_device->offset[L_NET] = 16;
		LOGGER_info("dltype: DLT_LINUX_SLL");
		break;
	case DLT_RAW:
		pcap_device->offset[L_NET] = 0;
		LOGGER_info("dltype: DLT_RAW");
		break;
	default:
		mlogf(ALWAYS, "Link Type (%d) not supported - default to DLT_RAW",
				pcap_device->link_type);
		pcap_device->offset[L_NET] = 0;
		break;
	}

}

void setFilter(pcap_dev_t *pcap_device) {
	/* apply filter */
	struct bpf_program fp;

	if (g_options.bpf) {
		if (pcap_compile(pcap_device->pcap_handle, &fp, g_options.bpf, 0, 0)
				== -1) {
			mlogf(ALWAYS, "Couldn't parse filter %s: %s\n", g_options.bpf,
					pcap_geterr(pcap_device->pcap_handle));
		}
		if (pcap_setfilter(pcap_device->pcap_handle, &fp) == -1) {
			mlogf(ALWAYS, "Couldn't install filter %s: %s\n", g_options.bpf,
					pcap_geterr(pcap_device->pcap_handle));
		}
	}

}
/**
 * Open packet capture devices
 */
void open_pcap(pcap_dev_t *pcap_devices, options_t *options) {

	if (options->file != NULL) {

		/* in case of file input */

		pcap_devices[0].pcap_handle = pcap_open_offline(options->file,
				pcap_errbuf);
		if (pcap_devices[0].pcap_handle == NULL) {
			fprintf(stderr, "%s \n", pcap_errbuf);
		}
		determineLinkType(&pcap_devices[0]);
		setFilter(&pcap_devices[0]);

	} else {

		int i;
		int fd;
		struct ifreq ifr;
		fd = socket(AF_INET, SOCK_DGRAM, 0);
		if (fd == -1) {
			perror("cannot create socket: ");
			exit(1);
		}

		/* I want to get an IPv4 IP address */
		ifr.ifr_addr.sa_family = AF_INET;

		for (i = 0; i < (options->number_interfaces); i++) {
			pcap_devices[i].pcap_handle = pcap_open_live(options->if_names[i],
					options->snapLength, 1, 1000, pcap_errbuf);
			pcap_devices[i].ifname = options->if_names[i];
			if (pcap_devices[i].pcap_handle == NULL) {
				fprintf(stderr, "%s \n", pcap_errbuf);
				exit(1);
			}
			// if (pcap_lookupnet(options->if_names[i],
			//		&(pcap_devices[i].IPv4address), &(pcap_devices[i].mask), errbuf)
			//		< 0) {
			//	printf("could not determine netmask and Ip-Adrdess of device %s \n",
			//			options->if_names[i]);
			// }

			/* get IP address attached to device */

			strncpy(ifr.ifr_name, options->if_names[i], IFNAMSIZ - 1);
			ioctl(fd, SIOCGIFADDR, &ifr);

			pcap_devices[i].IPv4address = ntohl(
					((struct sockaddr_in *) &ifr.ifr_addr)->sin_addr.s_addr);

			/* display result */
			LOGGER_info("Device %s has IP %s", options->if_names[i], htoa(
					pcap_devices[i].IPv4address));

			determineLinkType(&pcap_devices[i]);
			setFilter(&pcap_devices[i]);

		}
		close(fd);
	}
}

/**
 * Handle packets comming from libpcap and perform hash based selection.
 *
 */
static void packet_pcap_cb(u_char *user_args, const struct pcap_pkthdr *header,
		const u_char * packet) {
	pcap_dev_t *pcap_device = (pcap_dev_t*) user_args;
	//	int16_t headerOffset[4];
	uint8_t layers[4];
	uint32_t hash_result;
	uint32_t copiedbytes;
	uint8_t ttl;
	uint64_t timestamp;

	LOGGER_trace("handle packet");

	pcap_device->sampling_delta_count++;

	if (findHeaders(packet, header->caplen, pcap_device->offset, layers, &ttl) == 1) {

		//	LOGGER_debug("addr: %d",pcap_device->options==NULL );
		//	return;
		copiedbytes = pcap_device->options->selection_function(packet,
				header->caplen, pcap_device->outbuffer,
				pcap_device->outbufferLength, pcap_device->offset, layers);

		hash_result = pcap_device->options->hash_function(
				pcap_device->outbuffer, copiedbytes);

		// is packet selected?

		if ((pcap_device->options->sel_range_min < hash_result)
				&& (pcap_device->options->sel_range_max > hash_result)) {
			pcap_device->sampling_size++;

			// bypassing export if disabled by cmd line
			if (g_options.export_pktid_interval <= 0) {
				return;
			}

			int pktid = 0;
			if (g_options.hashAsPacketID == 1) { // in case we want to use the hashID as packet ID
				pktid = hash_result;
			} else {
				pktid = g_options.pktid_function(pcap_device->outbuffer,
						copiedbytes);
			}

			switch (pcap_device->options->templateID) {
			case MINT_ID: {
				timestamp = (uint64_t) header->ts.tv_sec * 1000000ULL
						+ (uint64_t) header->ts.tv_usec;
				void *fields[] = { &timestamp, &hash_result, &ttl };
				uint16_t lengths[] = { 8, 4, 1 };
				if (ipfix_export_array(pcap_device->ipfixhandle,
						pcap_device->ipfixtmpl_min, 3, fields, lengths) < 0) {
					fprintf(stderr, "ipfix_export() failed: %s\n", strerror(
							errno));
					exit(1);
				}
				break;
			}
			case TS_TTL_PROTO_ID: {
				uint16_t length;
				timestamp = (unsigned long long) header->ts.tv_sec * 1000000ULL
						+ header->ts.tv_usec;
				if (layers[L_NET] == N_IP) {
					length = ntohs(
							*((uint16_t*) (&packet[pcap_device->offset[L_NET]
									+ 2])));
				} else if (layers[L_NET] == N_IP6) {
					length = ntohs(
							*((uint16_t*) (&packet[pcap_device->offset[L_NET]
									+ 4])));
				} else {
					mlogf(ALWAYS, "cannot parse packet length \n");
					length = 0;
				}

				void *fields[] = { &timestamp, &hash_result, &ttl, &length,
						&layers[L_TRANS], &layers[L_NET] };
				uint16_t lengths[6] = { 8, 4, 1, 2, 1, 1 };

				if (ipfix_export_array(pcap_device->ipfixhandle,
						pcap_device->ipfixtmpl_ts_ttl, 6, fields, lengths)
						< 0) {
					fprintf(stderr, "ipfix_export() failed: %s\n", strerror(
							errno));
					exit(1);
				}
				break;
			}
			default:
				break;
			}

			if (++pcap_device->export_packet_count
					>= pcap_device->options->export_packet_count) {
				pcap_device->export_packet_count = 0;
				export_flush();
			}
		}
	}
}

/**
 * Called whenever a new packet is available. Note that packet_pcap_cb is
 * responsible for reading the packet.
 */
static void packet_watcher_cb(EV_P_ ev_io *w, int revents) {
	LOGGER_trace("packet");
	// retrieve respective device a new packet was seen
	pcap_dev_t *pcap_dev_ptr = (pcap_dev_t *) w->data;

	// dispatch packet
	if( pcap_dispatch(pcap_dev_ptr->pcap_handle,
					PCAP_DISPATCH_PACKET_COUNT ,
					packet_pcap_cb,
					(u_char*) pcap_dev_ptr)< 0 ) {
		LOGGER_error( "Error DeviceNo  %s: %s\n",pcap_dev_ptr->ifname,
				pcap_geterr( pcap_dev_ptr->pcap_handle) );
	}

}
/**
 * Here we setup a pcap device in non block mode and configure libev to read
 * a packet as soon it is available.
 */
static void event_setup_pcapdev(struct ev_loop *loop) {
	int i;
	pcap_dev_t * pcap_dev_ptr;
	for (i = 0; i < g_options.number_interfaces; i++) {
		LOGGER_debug("Setting up interface: %s", g_options.if_names[i]);

		pcap_dev_ptr = &pcap_devices[i];
		// TODO review
		pcap_dev_ptr->options = &g_options;

		if (pcap_setnonblock((*pcap_dev_ptr).pcap_handle, 1, pcap_errbuf) < 0) {
			LOGGER_error( "pcap_setnonblock: %s: %s", g_options.if_names[i],
					pcap_errbuf);
		}
		/* storing a reference of packet device to
		 be passed via watcher on a packet event so
		 we know which device to read the packet from */
		events.packet_watchers[i].data = (pcap_dev_t *) pcap_dev_ptr;
		ev_io_init(&events.packet_watchers[i], packet_watcher_cb, pcap_fileno(
				(*pcap_dev_ptr).pcap_handle), EV_READ);
		ev_io_start(loop, &events.packet_watchers[i]);
	}
}
/**
 * returns: 1 consumed, 0 otherwise
 */
static int netcom_cmd_set_ratio(char *msg) {
	double sampling_ratio;
	unsigned long messageId = 0; // session id
	int i, matches;
	matches = sscanf(msg, "mid: %lu -r %lf ", &messageId, &sampling_ratio);
	if (matches == 2) {
		LOGGER_debug("id: %lu", messageId);
		/* currently sampling ratio is equal for all devices */
		for (i = 0; i < g_options.number_interfaces; i++) {
			if (sampling_set_ratio(pcap_devices[i].options, sampling_ratio)
					== -1) {
				LOGGER_error("error setting sampling ration: %f",
						sampling_ratio);
			} else {
				char response[255];
				snprintf(response, 255, "INFO: new sampling ratio: %.3f",
						sampling_ratio);
				LOGGER_debug("==> %s", response);
				export_data_sync(&pcap_devices[i], ev_now(events.loop) * 1000,
						messageId, 0, response);
			}
		}
		return NETCON_CMD_MATCHED;
	}
	//	if( messageId > 0 ){
	//		char response[255];
	//		snprintf(response,255,"ERROR: invalid command: %s",msg);
	//		LOGGER_debug("==> %s",response);
	//		/* FIXME review: interface devices and options are still confuse*/
	//		for (i = 0; i < options.number_interfaces; i++) {
	//			export_data_sync(&pcap_devices[i],
	//					ev_now(events.loop)*1000,
	//					messageId,
	//					0,
	//					response);
	//		}
	//	}
	return NETCON_CMD_UNKNOWN;
}
/**
 * Setup network console
 */
static void event_setup_netcon(struct ev_loop *loop) {
	char *host = "localhost";
	int port = 5000;

	if (netcon_init(loop, host, port) < 0) {
		LOGGER_error("could not initialize netcon: host: %s, port: %d ", host,
				port);
	}
	netcon_register(netcom_cmd_set_ratio);
}

/**
 * Setups and starts main event loop.
 */
static void event_loop() {
	//	struct ev_loop *loop = ev_default_loop (EVLOOP_ONESHOT);
	struct ev_loop *loop = ev_default_loop(0);
	if (!loop) {
		LOGGER_fatal("Could not initialize loop!");
		exit(EXIT_FAILURE);
	}
	LOGGER_info("event_loop()");
	/*=== Setting up event loop ==*/
	/* signals */
	ev_signal_init(&events.sigint_watcher, sigint_cb, SIGINT);
	ev_signal_start(loop, &events.sigint_watcher);
	ev_signal_init(&events.sigalrm_watcher, sigalrm_cb, SIGALRM);
	ev_signal_start(loop, &events.sigalrm_watcher);

	ev_signal_init(&events.sigpipe_watcher, sigpipe_cb, SIGPIPE);
	ev_signal_start(loop, &events.sigpipe_watcher);

	/* resync  */
	ev_init(&events.resync_timer, resync_timer_cb);
	events.resync_timer.repeat = RESYNC_PERIOD;
	ev_timer_again(loop, &events.resync_timer);

	/* export timers */
	ev_init (&events.export_timer_pkid, export_timer_pktid_cb );
	if(g_options.export_pktid_interval > 0 ){
		events.export_timer_pkid.repeat  = g_options.export_pktid_interval;
		ev_timer_again (loop, &events.export_timer_pkid);
	}
	ev_init (&events.export_timer_sampling, export_timer_sampling_cb );
	if(g_options.export_sampling_interval > 0){
		events.export_timer_sampling.repeat  = g_options.export_sampling_interval;
		ev_timer_again (loop, &events.export_timer_sampling);
	}
	ev_init (&events.export_timer_stats, export_timer_stats_cb );
	if( g_options.export_stats_interval > 0 ){
		events.export_timer_stats.repeat  = g_options.export_stats_interval;
		ev_timer_again (loop, &events.export_timer_stats);
	}

	/*  packet watchers */
	event_setup_pcapdev(loop);
	/* setup network console 
	 */
	event_setup_netcon(loop);

	/* Enter main event loop; call unloop to exit.
	 *
	 * Everything is going to be handled within this call
	 * accordingly to callbacks defined above.
	 * */
	events.loop = loop;
	ev_loop(loop, 0);
}

/*-----------------------------------------------------------------------------
 Export
 -----------------------------------------------------------------------------*/
static void export_data_interface_stats(pcap_dev_t *dev,
		uint64_t observationTimeMilliseconds, u_int32_t size,
		u_int64_t deltaCount) {
	static uint16_t lengths[] = { 8, 4, 8, 4, 4, 0, 0 };
	static char interfaceName[255];
	static char interfaceDescription[255];
	struct pcap_stat pcapStat;
	struct in_addr addr;
	void *fields[] = { &observationTimeMilliseconds,
			&size, &deltaCount,
			&pcapStat.ps_recv, &pcapStat.ps_drop, interfaceName, interfaceDescription };
	snprintf(interfaceName,255, "%s",dev->ifname );
	addr.s_addr = htonl(dev->IPv4address);
	snprintf(interfaceDescription,255,"%s",inet_ntoa(addr));
	lengths[5]=strlen(interfaceName);
	lengths[6]=strlen(interfaceDescription);

	/* Get pcap statistics in case of live capture */
	if (g_options.file == NULL) {
		if (pcap_stats(dev->pcap_handle, &pcapStat) < 0) {
			LOGGER_error("Error DeviceNo  %s: %s\n", dev->ifname, pcap_geterr(
					dev->pcap_handle));
		}
	} else {
		pcapStat.ps_drop = 0;
		pcapStat.ps_recv = 0;
	}

	LOGGER_trace("sampling: (%d, %lu)", size, (long unsigned) deltaCount);
	if (ipfix_export_array(dev->ipfixhandle, dev->ipfixtmpl_interface_stats, 7,
			fields, lengths) < 0) {
		LOGGER_error("ipfix export failed: %s", strerror(errno));
	} else {
		dev->sampling_size = 0;
		dev->sampling_delta_count = 0;
	}
}
static void export_data_sync(pcap_dev_t *dev,
		int64_t observationTimeMilliseconds, u_int32_t messageId,
		u_int32_t messageValue, char * message) {
	static uint16_t lengths[] = { 8, 4, 4, 0 };
	lengths[3] = strlen(message);
	void *fields[] = { &observationTimeMilliseconds, &messageId, &messageValue,
			message };
	LOGGER_debug("export data sync");
	if (ipfix_export_array(dev->ipfixhandle, dev->ipfixtmpl_sync, 4,
			fields, lengths) < 0) {
		LOGGER_error("ipfix export failed: %s", strerror(errno));
		return;
	}
	if (ipfix_export_flush(dev->ipfixhandle) < 0) {
		LOGGER_error("Could not export IPFIX (flush) ");
	}

}
static void export_data_probe_stats(pcap_dev_t *dev) {
	static uint16_t lengths[] = { 8, 4, 8, 4, 4, 8, 8 };
	struct probe_stat probeStat;


	void *fields[] = { &probeStat.observationTimeMilliseconds,
			&probeStat.systemCpuIdle, &probeStat.systemMemFree,
			&probeStat.processCpuUser, &probeStat.processCpuSys,
			&probeStat.processMemVzs, &probeStat.processMemRss };

	probeStat.observationTimeMilliseconds = (uint64_t) ev_now(events.loop)
			* 1000;
	get_probe_stats(&probeStat);

	if (ipfix_export_array(dev->ipfixhandle, dev->ipfixtmpl_probe_stats, 7,
			fields, lengths) < 0) {
		LOGGER_error("ipfix export failed: %s", strerror(errno));
		return;
	}

}

/**
 * This causes libipfix to send cached messages to
 * the registered collectors.
 */
static void export_flush() {
	int i;
	LOGGER_trace("export_flush");
	for (i = 0; i < g_options.number_interfaces; i++) {
		if( ipfix_export_flush(pcap_devices[i].ipfixhandle) < 0 ){
			LOGGER_error("Could not export IPFIX, device: %d", i);
			//			ipfix_reconnect();
			break;
		}
	}
}
/**
 * Periodically called each export time interval.
 *
 */
static void export_timer_pktid_cb (EV_P_ ev_timer *w, int revents) {
	LOGGER_trace("export timer tick");
	export_flush();
}
/**
 * Peridically called each export/sampling time interval
 */
static void export_timer_sampling_cb (EV_P_ ev_timer *w, int revents) {
	int i;
	uint64_t observationTimeMilliseconds;
	LOGGER_trace("export timer sampling call back");
	observationTimeMilliseconds = (uint64_t)ev_now(events.loop) * 1000;
	for (i = 0; i < g_options.number_interfaces ; i++) {
		pcap_dev_t *dev = &pcap_devices[i];
		export_data_interface_stats(dev, observationTimeMilliseconds, dev->sampling_size, dev->sampling_delta_count );
	}
	export_flush();
}
/**
 * Periodically checks ipfix export fd and reconnects it
 * to netcon
 */
static void resync_timer_cb (EV_P_ ev_timer *w, int revents) {
	int i;
	ipfix_collector_sync_t *col;
	for (i = 0; i < (g_options.number_interfaces); i++) {
		col = (ipfix_collector_sync_t*) pcap_devices[i].ipfixhandle->collectors;
		netcon_resync( col->fd );

	}
}

static void export_timer_stats_cb (EV_P_ ev_timer *w, int revents) {
	/* using ipfix handle from first interface */
	export_data_probe_stats(&pcap_devices[0] );
	export_flush();
}

void init_libipfix(pcap_dev_t *pcap_devices, options_t *options) {
	int i;
	if (ipfix_init() < 0) {
		mlogf(ALWAYS, "cannot init ipfix module: %s\n", strerror(errno));

	}
	if (ipfix_add_vendor_information_elements(ipfix_ft_fokus) < 0) {
		fprintf(stderr, "cannot add FOKUS IEs: %s\n", strerror(errno));
		exit(1);
	}
	//	pcap_devices[i].ipfixhandle->collectors

	// printf("in open_ipfix\n");
	for (i = 0; i < (options->number_interfaces); i++) {
		// printf("in loop: %i\n", i);

		pcap_devices[i].export_packet_count = 0;

		/* use observationDomainID if explicitely given via cmd line, else use interface IPv4address as oid */
		uint32_t
		odid =
				(options->observationDomainID != 0) ? options->observationDomainID
						: pcap_devices[i].IPv4address;
		if( options->use_oid_first_interface ){
			odid = pcap_devices[0].IPv4address;
		}
		if (ipfix_open(&(pcap_devices[i].ipfixhandle), odid, IPFIX_VERSION) < 0) {
			mlogf(ALWAYS, "ipfix_open() failed: %s\n", strerror(errno));

		}
		if (IPFIX_MAKE_TEMPLATE(pcap_devices[i].ipfixhandle,
				pcap_devices[i].ipfixtmpl_min, export_fields_min) < 0) {
			LOGGER_fatal("template initialization failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (IPFIX_MAKE_TEMPLATE(pcap_devices[i].ipfixhandle,
				pcap_devices[i].ipfixtmpl_ts_ttl,
				export_fields_ts_ttl_proto) < 0) {
			LOGGER_fatal("template initialization failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (IPFIX_MAKE_TEMPLATE(pcap_devices[i].ipfixhandle,
				pcap_devices[i].ipfixtmpl_interface_stats, export_fields_interface_stats)
				< 0) {
			LOGGER_fatal("template initialization failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (IPFIX_MAKE_TEMPLATE(pcap_devices[i].ipfixhandle,
				pcap_devices[i].ipfixtmpl_probe_stats, export_fields_probe_stats) < 0) {
			LOGGER_fatal("template initialization failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}
		if (IPFIX_MAKE_TEMPLATE(pcap_devices[i].ipfixhandle,
				pcap_devices[i].ipfixtmpl_sync, export_fields_sync) < 0) {
			LOGGER_fatal("template initialization failed: %s", strerror(errno));
			exit(EXIT_FAILURE);
		}

		if (ipfix_add_collector(pcap_devices[i].ipfixhandle,
				options->collectorIP, options->collectorPort, IPFIX_PROTO_TCP)
				< 0) {
			LOGGER_error("ipfix_add_collector(%s,%d) failed: %s\n",
					options->collectorIP, options->collectorPort, strerror(
							errno));
		}

	}
}

/*-----------------------------------------------------------------------------
 MAIN
 -----------------------------------------------------------------------------*/
int main(int argc, char *argv[]) {
	int i;
	// initializing custom logger
	logger_init(LOGGER_LEVEL_WARN);

	// set defaults options
	options_set_defaults(&g_options);
	// parse commandline
	parse_cmdline(&g_options, argc, argv);

	logger_setlevel(g_options.verbosity);

	// --

	// allocate memory for pcap handles

	if (g_options.number_interfaces != 0) {
		pcap_devices = calloc((int) g_options.number_interfaces,
				sizeof(pcap_dev_t));
		events.packet_watchers = calloc((int) g_options.number_interfaces, sizeof(ev_io) );
		for (i = 0; i < g_options.number_interfaces; i++) {
			pcap_devices[i].outbuffer = calloc(g_options.snapLength,
					sizeof(uint8_t));
			pcap_devices[i].sampling_delta_count = 0;
			pcap_devices[i].sampling_size = 0;
		}

		// open pcap interfaces with filter
		open_pcap(pcap_devices, &g_options);

		// setup ipfix_exporter for each device
		init_libipfix(pcap_devices, &g_options);

		/* ---- main event loop  ---- */
		event_loop();

		/* -- normal shutdown --  */
		impd4e_shutdown();
		LOGGER_info("bye.");
	} else {
		print_help();
		exit(-1);
	}
	exit(0);

}

