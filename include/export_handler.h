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

#ifndef _EXPORT_HANDLER_H_
#define _EXPORT_HANDLER_H_

#include <inttypes.h>
#include <ev.h>

#include "constants.h"

extern ev_timer* export_timer_pkid;
extern ev_timer* export_timer_sampling;
extern ev_timer* export_timer_stats;
extern ev_timer* export_timer_location;

void export_handler_init(EV_P);

void export_data_sync(device_dev_t *dev
      , int64_t observationTimeMilliseconds
      , u_int32_t messageId
      , u_int32_t messageValue
      , char * message);

#endif // _EXPORT_HANDLER_H_

