/**
 * $Id$
 *
 * Copyright (C) 2010 Daniel-Constantin Mierla (asipto.com)
 *
 * This file is part of Kamailio, a free SIP server.
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 *
 * This file is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifndef _GEOIP_PV_H_
#define _GEOIP_PV_H_

#include <GeoIP.h>
#include <GeoIPCity.h>

#include "../../pvar.h"

int pv_parse_geoip_name(pv_spec_p sp, str *in);
int pv_get_geoip(struct sip_msg *msg, pv_param_t *param,
		pv_value_t *res);

int geoip_init_pv(char *path);
void geoip_destroy_pv(void);
void geoip_pv_reset(str *pvclass);
int geoip_update_pv(str *tomatch, str *pvclass);

#endif

