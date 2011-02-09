/*
 * constants.h
 *
 *  Created on: 26.08.2010
 *      Author: Ramon Masek
 */

#ifndef CONSTANTS_H_
#define CONSTANTS_H_

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#include <pcap.h>
#include <ipfix.h>

#include "hash.h" // todo:


#ifndef MAX_INTERFACES
#define MAX_INTERFACES 10
#endif

#define PCAP_DISPATCH_PACKET_COUNT 10 /*!< max number of packets to be processed on each dispatch */

#define BUFFER_SIZE 1024

typedef uint32_t (*hashFunction)(uint8_t*,uint16_t);
typedef uint16_t (*selectionFunction) (const uint8_t *, uint16_t , uint8_t *, uint16_t, int16_t *, uint8_t*);

typedef void (*device_handler)(u_char*, void* , const u_char* );


typedef struct {
	uint8_t*       ptr;
	uint32_t       len;
	const uint32_t max_len;
}
buffer_t;

typedef struct options
{	char     basedir[100];
	uint8_t  number_interfaces;
	uint32_t templateID;
	char     collectorIP[256];
	int16_t  collectorPort;
	char*    bpf; // berkley packet filter
	uint32_t          observationDomainID;
	hashFunction      hash_function;
	hashFunction      pktid_function;
	selectionFunction selection_function;
	uint32_t sel_range_min;
	uint32_t sel_range_max;
	uint16_t snapLength;
	uint8_t  verbosity;
	uint32_t export_packet_count;
	uint32_t export_interval;
	double sampling_ratio;
	bool   samplingResultExport;
	bool   resourceConsumptionExport;
	double export_pktid_interval;
	double export_sampling_interval;
	double export_stats_interval;
	int hashAsPacketID;
	int use_oid_first_interface;
} options_t;

typedef union device {
	pcap_t* pcap;
	char*   pcap_file;
	int     socket;
} device_t;

typedef enum {
	  TYPE_UNKNOWN
	, TYPE_PCAP
	, TYPE_PCAP_FILE
	, TYPE_SOCKET_UNIX
	, TYPE_SOCKET_INET
	, TYPE_FILE
	, TYPE_testtype
} device_type_t;

typedef struct device_desc {
	device_type_t	  type;
	char*    		  name;	// network adapter; file-name; socket-name; depends on device type
	device_t          handle;
} device_desc_t;

typedef struct ipfix_conf {
	ipfix_t*          handle;
	ipfix_template_t* template;
	ipfix_template_t* sampling_template;
} ipfix_conf_t;

typedef struct device_dev {
	device_type_t     device_type;
	char*             device_name;	// network adapter; file-name; socket-name; depends on device type
	device_t          device_handle;
	bpf_u_int32       IPv4address;
	bpf_u_int32       mask;
	int               link_type;
	ipfix_t*          ipfixhandle;
//	ipfix_template_t* ipfixtemplate;
	ipfix_template_t *ipfixtmpl_min;
	ipfix_template_t *ipfixtmpl_ts;
	ipfix_template_t *ipfixtmpl_ts_ttl;
	ipfix_template_t *ipfixtmpl_interface_stats;
	ipfix_template_t *ipfixtmpl_probe_stats;
	ipfix_template_t *ipfixtmpl_sync;
	ipfix_template_t* sampling_export_template;
	int16_t           offset[4];
	uint8_t*          outbuffer;
	uint16_t          outbufferLength;
	uint32_t          export_packet_count;
	uint64_t          totalpacketcount;
	struct timeval    last_export_time;
	uint32_t sampling_size;
	uint64_t sampling_delta_count;
} device_dev_t;

typedef struct packet_data {
	const uint8_t*  packet;
	uint32_t        length;
	uint32_t        capture_length;
	struct timeval  timestamp;
} packet_data_t;

typedef struct export_data {
	buffer_t    buffer;
	// layer offsets (IP Stack)
	uint64_t    timestamp;
	int16_t     layer_offsets[4];
	netProt_t   net;
	transProt_t transport;
	uint8_t     ttl;
} export_data_t;

// hash functions for parsing


#define HASH_FUNCTION_BOB   "BOB"
#define HASH_FUNCTION_OAAT  "OAAT"
#define HASH_FUNCTION_TWMX  "TWMX"
#define HASH_FUNCTION_HSIEH "HSIEH"

//hash input selection functions for parsing
#define HASH_INPUT_REC8    "REC8"
#define HASH_INPUT_IP      "IP"
#define HASH_INPUT_IPTP    "IP+TP"
#define HASH_INPUT_PACKET  "PACKET"
#define HASH_INPUT_RAW     "RAW"
#define HASH_INPUT_LINK    "LINK"
#define HASH_INPUT_NET     "NET"
#define HASH_INPUT_TRANS   "TRANS"
#define HASH_INPUT_PAYLOAD "PAYLOAD"
#define HASH_INPUT_SELECT  "SELECT"

// template definition

#define MINT_ID         	0
#define TS_TTL_PROTO_ID 	1
#define TS_ID           	2

#define MIN_NAME  			"min"
#define TS_TTL_RROTO_NAME 	"lp"
#define TS_NAME			 	"ts"

typedef enum hash_function {
	FUNCTION_BOB		= 0x001,
	FUNCTION_TWMX		= 0x002,
	FUNCTION_OAAT		= 0x003,
	FUNCTION_SBOX		= 0x004,
} hash_function_t;



typedef enum hash_input_selection {
	INPUT_8_RECOMMENDED_BYTES = 0x001,
	INPUT_IP_HEADER           = 0x002,
	INPUT_IP_TRANSPORT_HEADER = 0x003,
	INPUT_WHOLE_PACKET        = 0x004,
} hash_input_selection_t;


// log level
enum {
	  ALWAYS=0
	, ERROR=1
	, CRITICAL=1
	, WARNING=2
	, INFO
	, DEBUG
	, ALL
};
//#define ALWAYS   0
//#define ERROR    1
//#define CRITICAL 1
//#define WARNING  2
//#define INFO     3
//#define DEBUG    4
//#define ALL      5

extern options_t     g_options;
extern device_dev_t  if_devices[];
extern char pcap_errbuf[PCAP_ERRBUF_SIZE];
extern char errbuf[PCAP_ERRBUF_SIZE];



#endif /* CONSTANTS_H_ */