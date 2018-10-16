/*
 * Tegra host1x Syncpoints
 *
 * Copyright (c) 2010-2015, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "dev.h"

struct host1x_syncpt *host1x_syncpt_get(struct host1x *host, u32 id)
{
	return NULL;
}

u32 host1x_syncpt_id(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read_min(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read_max(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_read(struct host1x_syncpt *sp)
{
	return 0;
}

int host1x_syncpt_incr(struct host1x_syncpt *sp)
{
	return 0;
}

u32 host1x_syncpt_incr_max(struct host1x_syncpt *sp, u32 incrs)
{
	return 0;
}

int host1x_syncpt_wait(struct host1x_syncpt *sp, u32 thresh, long timeout,
		       u32 *value)
{
	return 0;
}

struct host1x_syncpt *host1x_syncpt_request(struct host1x_client *client,
					    unsigned long flags)
{
	return NULL;
}

void host1x_syncpt_free(struct host1x_syncpt *sp)
{
}

struct host1x_syncpt_base *host1x_syncpt_get_base(struct host1x_syncpt *sp)
{
	return NULL;
}

u32 host1x_syncpt_base_id(struct host1x_syncpt_base *base)
{
	return 0;
}
