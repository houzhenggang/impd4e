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

#ifndef TEMPLATES_H_
#define TEMPLATES_H_

#include "ipfix.h"
#include "ipfix_def.h"
#include "ipfix_def_fokus.h"



/* help macros */
#define IPFIX_MAKE_TEMPLATE(handle,template,fields) \
	ipfix_make_template(handle, &(template), fields, sizeof(fields) / sizeof(export_fields_t) )

/*
 * for an introduction to the IPFIX protocol see:
 * RFC5101 - http://tools.ietf.org/html/rfc5101
 *
 * for an overview of available IPFIX fields and
 * their meaning see
 * RFC5102 - http://tools.ietf.org/html/rfc5102
 * (or in more himan readable form:
 * http://www.iana.org/assignments/ipfix/ipfix.xhtml)
 */


/*
 * when invoked with "-t ts" the following fields are exported
 * in each IPFIX data record:
 */
export_fields_t export_fields_ts[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMICROSECONDS, 8},
    { 0, IPFIX_FT_DIGESTHASHVALUE, 4},
};

/*
 * when invoked with "-t min" the following fields are exported
 * in each IPFIX data record:
 * TODO: use a commen function name like 'ts_ttl' instead of 'min'
 */
export_fields_t export_fields_min[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMICROSECONDS, 8},
    { 0, IPFIX_FT_DIGESTHASHVALUE, 4},
    { 0, IPFIX_FT_IPTTL, 1},
};

/*
 * when invoked with "-t lp" the following fields are exported
 * in each IPFIX data record:
 */
export_fields_t export_fields_ts_ttl_proto[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMICROSECONDS, 8},
    { 0, IPFIX_FT_DIGESTHASHVALUE, 4},
    { 0, IPFIX_FT_IPTTL, 1},
    { 0, IPFIX_FT_TOTALLENGTHIPV4, 2},
    { 0, IPFIX_FT_PROTOCOLIDENTIFIER, 1},
    { 0, IPFIX_FT_IPVERSION, 1},
};

/*
 * when invoked with "-t ls" the following fields are exported
 * in each IPFIX data record:
 */
export_fields_t export_fields_ts_ttl_proto_ip[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMICROSECONDS, 8},
    { 0, IPFIX_FT_DIGESTHASHVALUE, 4},
    { 0, IPFIX_FT_IPTTL, 1},
    { 0, IPFIX_FT_TOTALLENGTHIPV4, 2},
    { 0, IPFIX_FT_PROTOCOLIDENTIFIER, 1},
    { 0, IPFIX_FT_IPVERSION, 1},
    { 0, IPFIX_FT_SOURCEIPV4ADDRESS, 4},
    { 0, IPFIX_FT_SOURCETRANSPORTPORT, 2},
    { 0, IPFIX_FT_DESTINATIONIPV4ADDRESS, 4},
    { 0, IPFIX_FT_DESTINATIONTRANSPORTPORT, 2},
};

export_fields_t export_fields_interface_stats[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMILLISECONDS, 8},
    { 0, IPFIX_FT_SAMPLINGSIZE, 4},
    { 0, IPFIX_FT_PACKETDELTACOUNT, 8},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PCAPSTAT_RECV, 4}, /* PFIX_CODING_UINT, "pcap_recv",  "number of packets received by pcap"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PCAPSTAT_DROP, 4}, /* PFIX_CODING_UINT, "pcap_drop",  "number of packets dropped by pcap"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_INTERFACE_NAME, 65535},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_INTERFACE_DESCRIPTION, 65535},
};

export_fields_t export_fields_probe_stats[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMILLISECONDS, 8},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_SYSTEM_CPU_IDLE, 4}, /* IPFIX_CODING_FLOAT, "sys_cpu_idle",  "PT system CPU idle %"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_SYSTEM_MEM_FREE, 8}, /* IPFIX_CODING_UINT,  "sys_mem_free",  "PT system free memory in kilobytes"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROCESS_CPU_USER, 4}, /* IPFIX_CODING_FLOAT, "proc_cpu_user", "PT percentage of CPU used in user level (application) "  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROCESS_CPU_SYS, 4}, /* PFIX_CODING_FLOAT,  "proc_cpu_sys",  "PT percentage of CPU used in system level (kernel) "  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROCESS_MEM_VZS, 8}, /* PFIX_CODING_UINT,   "proc_mem_vzs",  "PT the process virtual memory used in kilobytes"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROCESS_MEM_RSS, 8}, /* PFIX_CODING_UINT,   "proc_mem_rss",  "PT the process resident set size in kilobytes"  }, */
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_SYSTEM_MEM_TOTAL, 8}, /* IPFIX_CODING_UINT,  "sys_mem_total",  "PT system total memory in kilobytes"  }, */
};

export_fields_t export_fields_location[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMILLISECONDS, 8},
    { 0, IPFIX_FT_SOURCEIPV4ADDRESS, 4},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_GEO_LATITUDE, 65535},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_GEO_LONGITUDE, 65535},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROBE_NAME, 65535},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_PROBE_LOCATION_NAME, 65535},
};

export_fields_t export_fields_sync[] = {
    { 0, IPFIX_FT_OBSERVATIONTIMEMILLISECONDS, 8},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_MESSAGE_ID, 4},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_MESSAGE_VALUE, 4},
    { IPFIX_ENO_FOKUS, IPFIX_FT_PT_MESSAGE, 65535},
};

#endif

