/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2012 Texas Instruments, Inc.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdbool.h>
#include <errno.h>

#include <glib.h>

#include "src/log.h"

#include "lib/uuid.h"
#include "src/shared/util.h"

#include "attrib/gattrib.h"
#include "attrib/att.h"
#include "attrib/gatt.h"

#include "android/dis.h"

#define PNP_ID_SIZE	7

struct bt_dis {
	int			ref_count;
	uint8_t			source;
	uint16_t		vendor;
	uint16_t		product;
	uint16_t		version;
	GAttrib			*attrib;	/* GATT connection */
	struct gatt_primary	*primary;	/* Primary details */
	GSList			*chars;		/* Characteristics */
};

struct characteristic {
	struct gatt_char	attr;	/* Characteristic */
	struct bt_dis		*d;	/* deviceinfo where the char belongs */
};

static void dis_free(struct bt_dis *dis)
{
	if (dis->attrib)
		g_attrib_unref(dis->attrib);

	g_slist_free_full(dis->chars, g_free);

	g_free(dis->primary);
	g_free(dis);
}

struct bt_dis *bt_dis_new(void *primary)
{
	struct bt_dis *dis;

	dis = g_try_new0(struct bt_dis, 1);
	if (!dis)
		return NULL;

	if (primary)
		dis->primary = g_memdup(primary, sizeof(*dis->primary));

	return bt_dis_ref(dis);
}

struct bt_dis *bt_dis_ref(struct bt_dis *dis)
{
	if (!dis)
		return NULL;

	__sync_fetch_and_add(&dis->ref_count, 1);

	return dis;
}

void bt_dis_unref(struct bt_dis *dis)
{
	if (!dis)
		return;

	if (__sync_sub_and_fetch(&dis->ref_count, 1))
		return;

	dis_free(dis);
}

static void read_pnpid_cb(guint8 status, const guint8 *pdu, guint16 len,
							gpointer user_data)
{
	struct characteristic *ch = user_data;
	struct bt_dis *dis = ch->d;
	uint8_t value[PNP_ID_SIZE];
	ssize_t vlen;

	if (status != 0) {
		error("Error reading PNP_ID value: %s", att_ecode2str(status));
		return;
	}

	vlen = dec_read_resp(pdu, len, value, sizeof(value));
	if (vlen < 0) {
		error("Error reading PNP_ID: Protocol error");
		return;
	}

	if (vlen < 7) {
		error("Error reading PNP_ID: Invalid pdu length received");
		return;
	}

	dis->source = value[0];
	dis->vendor = get_le16(&value[1]);
	dis->product = get_le16(&value[3]);
	dis->version = get_le16(&value[5]);
}

static void process_deviceinfo_char(struct characteristic *ch)
{
	if (g_strcmp0(ch->attr.uuid, PNPID_UUID) == 0)
		gatt_read_char(ch->d->attrib, ch->attr.value_handle,
							read_pnpid_cb, ch);
}

static void configure_deviceinfo_cb(uint8_t status, GSList *characteristics,
								void *user_data)
{
	struct bt_dis *d = user_data;
	GSList *l;

	if (status != 0) {
		error("Discover deviceinfo characteristics: %s",
							att_ecode2str(status));
		return;
	}

	for (l = characteristics; l; l = l->next) {
		struct gatt_char *c = l->data;
		struct characteristic *ch;

		ch = g_new0(struct characteristic, 1);
		ch->attr.handle = c->handle;
		ch->attr.properties = c->properties;
		ch->attr.value_handle = c->value_handle;
		memcpy(ch->attr.uuid, c->uuid, MAX_LEN_UUID_STR + 1);
		ch->d = d;

		d->chars = g_slist_append(d->chars, ch);

		process_deviceinfo_char(ch);
	}
}

bool bt_dis_attach(struct bt_dis *dis, void *attrib)
{
	struct gatt_primary *primary = dis->primary;

	if (dis->attrib || !primary)
		return false;

	dis->attrib = g_attrib_ref(attrib);

	gatt_discover_char(dis->attrib, primary->range.start,
						primary->range.end, NULL,
						configure_deviceinfo_cb, dis);

	return true;
}

void bt_dis_detach(struct bt_dis *dis)
{
	if (!dis->attrib)
		return;

	g_attrib_unref(dis->attrib);
	dis->attrib = NULL;
}
