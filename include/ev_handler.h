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

#ifndef EVENTHANDLER_H_
#define EVENTHANDLER_H_

#include <ev.h>

// -----------------------------------------------------------------------------
// Type definitions
// -----------------------------------------------------------------------------

typedef void (*timer_cb_t)(EV_P_ ev_timer *w, int revents);
typedef void (*io_cb_t)(EV_P_ ev_io *w, int revents);
typedef void (*watcher_cb_t)(EV_P_ ev_watcher *w, int revents);

/* -- event loop -- */
void event_loop( EV_P );
void event_loop_init( EV_P );
void event_loop_start( EV_P );
ev_watcher* event_register_io(EV_P_ watcher_cb_t cb, int fd);
ev_watcher* event_register_io_r(EV_P_ watcher_cb_t cb, int fd);
ev_watcher* event_register_io_w(EV_P_ watcher_cb_t cb, int fd);
ev_watcher* event_register_timer(EV_P_ watcher_cb_t cb, double timeout);
ev_watcher* event_register_timer_w(EV_P_ watcher_cb_t cb, double timeout);

void event_deregister_timer( EV_P_ ev_timer *w );
void event_deregister_io( EV_P_ ev_io *w );


#endif /* EVENTHANDLER_H_ */
