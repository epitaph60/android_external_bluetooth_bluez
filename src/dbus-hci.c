/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2006-2007  Nokia Corporation
 *  Copyright (C) 2004-2008  Marcel Holtmann <marcel@holtmann.org>
 *
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

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>
#include <bluetooth/hci_lib.h>
#include <bluetooth/sdp.h>

#include <glib.h>
#include <dbus/dbus.h>
#include <gdbus.h>

#include "hcid.h"
#include "textfile.h"
#include "manager.h"
#include "adapter.h"
#include "device.h"
#include "error.h"
#include "glib-helper.h"
#include "dbus-common.h"
#include "agent.h"
#include "dbus-hci.h"

static DBusConnection *connection = NULL;

void bonding_request_free(struct bonding_request_info *bonding)
{
	struct btd_device *device;
	char address[18];
	struct agent *agent;

	if (!bonding)
		return;

	if (bonding->msg)
		dbus_message_unref(bonding->msg);

	if (bonding->conn)
		dbus_connection_unref(bonding->conn);

	if (bonding->io)
		g_io_channel_unref(bonding->io);

	ba2str(&bonding->bdaddr, address);

	device = adapter_find_device(bonding->adapter, address);
	agent = device_get_agent(device);

	if (device && agent) {
		agent_destroy(agent, FALSE);
		device_set_agent(device, NULL);
	}

	g_free(bonding);
}

int found_device_cmp(const struct remote_dev_info *d1,
			const struct remote_dev_info *d2)
{
	int ret;

	if (bacmp(&d2->bdaddr, BDADDR_ANY)) {
		ret = bacmp(&d1->bdaddr, &d2->bdaddr);
		if (ret)
			return ret;
	}

	if (d2->name_status != NAME_ANY) {
		ret = (d1->name_status - d2->name_status);
		if (ret)
			return ret;
	}

	return 0;
}

int dev_rssi_cmp(struct remote_dev_info *d1, struct remote_dev_info *d2)
{
	int rssi1, rssi2;

	rssi1 = d1->rssi < 0 ? -d1->rssi : d1->rssi;
	rssi2 = d2->rssi < 0 ? -d2->rssi : d2->rssi;

	return rssi1 - rssi2;
}

int found_device_add(GSList **list, bdaddr_t *bdaddr, int8_t rssi,
			name_status_t name_status)
{
	struct remote_dev_info *dev, match;
	GSList *l;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);
	match.name_status = NAME_ANY;

	/* ignore repeated entries */
	l = g_slist_find_custom(*list, &match, (GCompareFunc) found_device_cmp);
	if (l) {
		/* device found, update the attributes */
		dev = l->data;

		if (rssi != 0)
			dev->rssi = rssi;

		 /* Get remote name can be received while inquiring.
		  * Keep in mind that multiple inquiry result events can
		  * be received from the same remote device.
		  */
		if (name_status != NAME_NOT_REQUIRED)
			dev->name_status = name_status;

		*list = g_slist_sort(*list, (GCompareFunc) dev_rssi_cmp);

		return -EALREADY;
	}

	dev = g_new0(struct remote_dev_info, 1);

	bacpy(&dev->bdaddr, bdaddr);
	dev->rssi = rssi;
	dev->name_status = name_status;

	*list = g_slist_insert_sorted(*list, dev, (GCompareFunc) dev_rssi_cmp);

	return 0;
}

static int found_device_remove(GSList **list, bdaddr_t *bdaddr)
{
	struct remote_dev_info *dev, match;
	GSList *l;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, bdaddr);

	l = g_slist_find_custom(*list, &match, (GCompareFunc) found_device_cmp);
	if (!l)
		return -1;

	dev = l->data;
	*list = g_slist_remove(*list, dev);
	g_free(dev);

	return 0;
}

int active_conn_find_by_bdaddr(const void *data, const void *user_data)
{
	const struct active_conn_info *con = data;
	const bdaddr_t *bdaddr = user_data;

	return bacmp(&con->bdaddr, bdaddr);
}

static int active_conn_find_by_handle(const void *data, const void *user_data)
{
	const struct active_conn_info *dev = data;
	const uint16_t *handle = user_data;

	if (dev->handle == *handle)
		return 0;

	return -1;
}

static int active_conn_append(GSList **list, bdaddr_t *bdaddr,
				uint16_t handle)
{
	struct active_conn_info *dev;

	dev = g_new0(struct active_conn_info, 1);

	bacpy(&dev->bdaddr, bdaddr);
	dev->handle = handle;

	*list = g_slist_append(*list, dev);
	return 0;
}

DBusMessage *new_authentication_return(DBusMessage *msg, uint8_t status)
{
	switch (status) {
	case 0x00: /* success */
		return dbus_message_new_method_return(msg);

	case 0x04: /* page timeout */
	case 0x08: /* connection timeout */
	case 0x10: /* connection accept timeout */
	case 0x22: /* LMP response timeout */
	case 0x28: /* instant passed - is this a timeout? */
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationTimeout",
					"Authentication Timeout");
	case 0x17: /* too frequent pairing attempts */
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".RepeatedAttempts",
					"Repeated Attempts");

	case 0x06:
	case 0x18: /* pairing not allowed (e.g. gw rejected attempt) */
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationRejected",
					"Authentication Rejected");

	case 0x07: /* memory capacity */
	case 0x09: /* connection limit */
	case 0x0a: /* synchronous connection limit */
	case 0x0d: /* limited resources */
	case 0x13: /* user ended the connection */
	case 0x14: /* terminated due to low resources */
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationCanceled",
					"Authentication Canceled");

	case 0x05: /* authentication failure */
	case 0x0E: /* rejected due to security reasons - is this auth failure? */
	case 0x25: /* encryption mode not acceptable - is this auth failure? */
	case 0x26: /* link key cannot be changed - is this auth failure? */
	case 0x29: /* pairing with unit key unsupported - is this auth failure? */
	case 0x2f: /* insufficient security - is this auth failure? */
	default:
		return dbus_message_new_error(msg,
					ERROR_INTERFACE ".AuthenticationFailed",
					"Authentication Failed");
	}
}

static void adapter_mode_changed(struct adapter *adapter, uint8_t scan_mode)
{
	const char *mode;
	const gchar *path = adapter_get_path(adapter);

	adapter_set_scan_mode(adapter, scan_mode);

	switch (scan_mode) {
	case SCAN_DISABLED:
		mode = "off";
		adapter->mode = MODE_OFF;
		break;
	case SCAN_PAGE:
		mode = "connectable";
		adapter->mode = MODE_CONNECTABLE;
		break;
	case (SCAN_PAGE | SCAN_INQUIRY):

		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter, adapter->discov_timeout * 1000);

		if (adapter->mode == MODE_LIMITED) {
			mode = "limited";
		} else {
			adapter->mode = MODE_DISCOVERABLE;
			mode = "discoverable";
		}
		break;
	case SCAN_INQUIRY:
		/* Address the scenario where another app changed the scan mode */
		if (adapter->discov_timeout != 0)
			adapter_set_discov_timeout(adapter, adapter->discov_timeout * 1000);

		/* ignore, this event should not be sent*/
	default:
		/* ignore, reserved */
		return;
	}

	dbus_connection_emit_property_changed(connection, path,
					ADAPTER_INTERFACE, "Mode",
					DBUS_TYPE_STRING, &mode);
}

/*****************************************************************
 *
 *  Section reserved to HCI commands confirmation handling and low
 *  level events(eg: device attached/dettached.
 *
 *****************************************************************/

static void pincode_cb(struct agent *agent, DBusError *err, const char *pincode,
			struct btd_device *device)
{
	struct adapter *adapter = device_get_adapter(device);
	pin_code_reply_cp pr;
	bdaddr_t sba, dba;
	size_t len;
	int dev;
	struct pending_auth_info *auth;
	const gchar *destination = device_get_address(device);
	uint16_t dev_id = adapter_get_dev_id(adapter);
	const gchar *source = adapter_get_address(adapter);

	/* No need to reply anything if the authentication already failed */
	if (adapter->bonding && adapter->bonding->hci_status)
		return;

	dev = hci_open_dev(dev_id);
	if (dev < 0) {
		error("hci_open_dev(%d): %s (%d)", dev_id,
				strerror(errno), errno);
		return;
	}

	str2ba(source, &sba);
	str2ba(destination, &dba);

	auth = adapter_find_auth_request(adapter, &dba);

	if (err) {
		hci_send_cmd(dev, OGF_LINK_CTL,
				OCF_PIN_CODE_NEG_REPLY, 6, &dba);
		goto done;
	}

	len = strlen(pincode);

	set_pin_length(&sba, len);

	memset(&pr, 0, sizeof(pr));
	bacpy(&pr.bdaddr, &dba);
	memcpy(pr.pin_code, pincode, len);
	pr.pin_len = len;
	hci_send_cmd(dev, OGF_LINK_CTL, OCF_PIN_CODE_REPLY, PIN_CODE_REPLY_CP_SIZE, &pr);

done:
	if (auth) {
		auth->replied = TRUE;
		auth->agent = NULL;
	}
	hci_close_dev(dev);
}

int hcid_dbus_request_pin(int dev, bdaddr_t *sba, struct hci_conn_info *ci)
{
	char addr[18];
	struct adapter *adapter;
	struct btd_device *device;
	struct agent *agent;
	int ret;

	adapter = manager_find_adapter(sba);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	ba2str(&ci->bdaddr, addr);

	device = adapter_find_device(adapter, addr);

	if (device)
		agent = device_get_agent(device);

	if (!agent)
		agent = adapter->agent;

	if (!agent)
		return -EPERM;

	if (!device) {
		device = adapter_create_device(connection, adapter, addr);
		if (!device)
			return -ENODEV;
	}

	ret = agent_request_pincode(agent, device,
					(agent_pincode_cb) pincode_cb,
					device);
	if (ret == 0) {
		struct pending_auth_info *auth;
		auth = adapter_new_auth_request(adapter, &ci->bdaddr,
						AUTH_TYPE_PINCODE);
		auth->agent = agent;
	}

	return ret;
}

static void confirm_cb(struct agent *agent, DBusError *err, void *user_data)
{
	struct btd_device *device = user_data;
	struct adapter *adapter = device_get_adapter(device);
	user_confirm_reply_cp cp;
	int dd;
	struct pending_auth_info *auth;
	const gchar *destination = device_get_address(device);
	uint16_t dev_id = adapter_get_dev_id(adapter);

	/* No need to reply anything if the authentication already failed */
	if (adapter->bonding && adapter->bonding->hci_status)
		return;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("Unable to open hci%d", dev_id);
		return;
	}

	memset(&cp, 0, sizeof(cp));
	str2ba(destination, &cp.bdaddr);

	auth = adapter_find_auth_request(adapter, &cp.bdaddr);

	if (err)
		hci_send_cmd(dd, OGF_LINK_CTL, OCF_USER_CONFIRM_NEG_REPLY,
					USER_CONFIRM_REPLY_CP_SIZE, &cp);
	else
		hci_send_cmd(dd, OGF_LINK_CTL, OCF_USER_CONFIRM_REPLY,
					USER_CONFIRM_REPLY_CP_SIZE, &cp);

	if (auth) {
		auth->replied = TRUE;
		auth->agent = FALSE;
	}

	hci_close_dev(dd);
}

static void passkey_cb(struct agent *agent, DBusError *err, uint32_t passkey,
			void *user_data)
{
	struct btd_device *device = user_data;
	struct adapter *adapter = device_get_adapter(device);
	user_passkey_reply_cp cp;
	bdaddr_t dba;
	int dd;
	struct pending_auth_info *auth;
	const gchar *destination = device_get_address(device);
	uint16_t dev_id = adapter_get_dev_id(adapter);

	/* No need to reply anything if the authentication already failed */
	if (adapter->bonding && adapter->bonding->hci_status)
		return;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("Unable to open hci%d", dev_id);
		return;
	}

	str2ba(destination, &dba);

	memset(&cp, 0, sizeof(cp));
	bacpy(&cp.bdaddr, &dba);
	cp.passkey = passkey;

	auth = adapter_find_auth_request(adapter, &dba);

	if (err)
		hci_send_cmd(dd, OGF_LINK_CTL,
				OCF_USER_PASSKEY_NEG_REPLY, 6, &dba);
	else
		hci_send_cmd(dd, OGF_LINK_CTL, OCF_USER_PASSKEY_REPLY,
					USER_PASSKEY_REPLY_CP_SIZE, &cp);

	if (auth) {
		auth->replied = TRUE;
		auth->agent = NULL;
	}

	hci_close_dev(dd);
}

static int get_auth_requirements(bdaddr_t *local, bdaddr_t *remote,
							uint8_t *auth)
{
	struct hci_auth_info_req req;
	char addr[18];
	int err, dd, dev_id;

	ba2str(local, addr);

	dev_id = hci_devid(addr);
	if (dev_id < 0)
		return dev_id;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return dd;

	memset(&req, 0, sizeof(req));
	bacpy(&req.bdaddr, remote);

	err = ioctl(dd, HCIGETAUTHINFO, (unsigned long) &req);
	if (err < 0) {
		debug("HCIGETAUTHINFO failed: %s (%d)",
					strerror(errno), errno);
		hci_close_dev(dd);
		return err;
	}

	hci_close_dev(dd);

	if (auth)
		*auth = req.type;

	return 0;
}

int hcid_dbus_user_confirm(bdaddr_t *sba, bdaddr_t *dba, uint32_t passkey)
{
	struct adapter *adapter;
	struct btd_device *device;
	struct agent *agent;
	char addr[18];
	uint8_t type;
	struct pending_auth_info *auth;
	uint16_t dev_id;

	adapter = manager_find_adapter(sba);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	dev_id = adapter_get_dev_id(adapter);

	if (get_auth_requirements(sba, dba, &type) < 0) {
		int dd;

		dd = hci_open_dev(dev_id);
		if (dd < 0) {
			error("Unable to open hci%d", dev_id);
			return -1;
		}

		hci_send_cmd(dd, OGF_LINK_CTL,
					OCF_USER_CONFIRM_NEG_REPLY, 6, dba);

		hci_close_dev(dd);

		return 0;
	}

	ba2str(dba, addr);

	device = adapter_get_device(connection, adapter, addr);
	if (!device) {
		error("Device creation failed");
		return -1;
	}

	/* If no MITM protection required, auto-accept */
	if (!(device_get_auth(device) & 0x01) && !(type & 0x01)) {
		int dd;

		dd = hci_open_dev(dev_id);
		if (dd < 0) {
			error("Unable to open hci%d", dev_id);
			return -1;
		}

		hci_send_cmd(dd, OGF_LINK_CTL,
					OCF_USER_CONFIRM_REPLY, 6, dba);

		hci_close_dev(dd);

		return 0;
	}

	agent = device_get_agent(device);

	if (!agent)
		agent = adapter->agent;

	if (!agent) {
		error("No agent available for user confirm request");
		return -1;
	}

	if (agent_request_confirmation(agent, device, passkey,
						confirm_cb, device) < 0) {
		error("Requesting passkey failed");
		return -1;
	}

	auth = adapter_new_auth_request(adapter, dba, AUTH_TYPE_CONFIRM);
	auth->agent = agent;

	return 0;
}

int hcid_dbus_user_passkey(bdaddr_t *sba, bdaddr_t *dba)
{
	struct adapter *adapter;
	struct btd_device *device;
	struct agent *agent;
	char addr[18];
	struct pending_auth_info *auth;

	adapter = manager_find_adapter(sba);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	ba2str(dba, addr);

	device = adapter_get_device(connection, adapter, addr);

	if (device)
		agent = device_get_agent(device);

	if (!agent)
		agent = adapter->agent;

	if (!agent) {
		error("No agent available for user confirm request");
		return -1;
	}

	if (agent_request_passkey(agent, device, passkey_cb, device) < 0) {
		error("Requesting passkey failed");
		return -1;
	}

	auth = adapter_new_auth_request(adapter, dba, AUTH_TYPE_PASSKEY);
	auth->agent = agent;

	return 0;
}

int hcid_dbus_user_notify(bdaddr_t *sba, bdaddr_t *dba, uint32_t passkey)
{
	struct adapter *adapter;
	struct btd_device *device;
	struct agent *agent;
	char addr[18];
	struct pending_auth_info *auth;

	adapter = manager_find_adapter(sba);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	ba2str(dba, addr);

	device = adapter_get_device(connection, adapter, addr);
	if (device)
		agent = device_get_agent(device);

	if (!agent)
		agent = adapter->agent;

	if (!agent) {
		error("No agent available for user confirm request");
		return -1;
	}

	if (agent_display_passkey(agent, device, passkey) < 0) {
		error("Displaying passkey failed");
		return -1;
	}

	auth = adapter_new_auth_request(adapter, dba, AUTH_TYPE_NOTIFY);
	auth->agent = agent;

	return 0;
}

void hcid_dbus_bonding_process_complete(bdaddr_t *local, bdaddr_t *peer,
					uint8_t status)
{
	struct adapter *adapter;
	char peer_addr[18];
	const char *paddr = peer_addr;
	DBusMessage *reply;
	struct btd_device *device;
	struct bonding_request_info *bonding;
	gboolean paired = TRUE;
	struct pending_auth_info *auth;
	const gchar *dev_path;
	const gchar *path;

	debug("hcid_dbus_bonding_process_complete: status=%02x", status);

	ba2str(peer, peer_addr);

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	if (status) {
		if (adapter->bonding)
			adapter->bonding->hci_status = status;
	}

	auth = adapter_find_auth_request(adapter, peer);
	if (!auth) {
		debug("hcid_dbus_bonding_process_complete: no pending auth request");
		goto proceed;
	}

	if (auth->agent)
		agent_cancel(auth->agent);

	adapter_remove_auth_request(adapter, peer);

	if (status)
		goto proceed;

	device = adapter_get_device(connection, adapter, paddr);
	if (device) {
		debug("hcid_dbus_bonding_process_complete: removing temporary flag");

		device_set_temporary(device, FALSE);
		dev_path = device_get_path(device);
		path = adapter_get_path(adapter);

		g_dbus_emit_signal(connection, path,
				ADAPTER_INTERFACE, "DeviceCreated",
				DBUS_TYPE_OBJECT_PATH, &dev_path,
				DBUS_TYPE_INVALID);

		dbus_connection_emit_property_changed(connection, dev_path,
					DEVICE_INTERFACE, "Paired",
					DBUS_TYPE_BOOLEAN, &paired);
	}

proceed:
	bonding = adapter->bonding;
	if (!bonding || bacmp(&bonding->bdaddr, peer))
		return; /* skip: no bonding req pending */

	if (bonding->cancel) {
		/* reply authentication canceled */
		reply = new_authentication_return(bonding->msg,
				HCI_OE_USER_ENDED_CONNECTION);
		g_dbus_send_message(connection, reply);
		goto cleanup;
	}

	/* reply authentication success or an error */
	if (dbus_message_is_method_call(bonding->msg, ADAPTER_INTERFACE,
					"CreateBonding")) {
		reply = new_authentication_return(bonding->msg, status);
		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	} else if ((device = adapter_find_device(adapter, paddr))) {
		if (status) {
			reply = new_authentication_return(bonding->msg, status);
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
		} else {
			device_set_temporary(device, FALSE);
			device_browse(device, bonding->conn,
					bonding->msg, NULL);
		}
	}

cleanup:
	g_dbus_remove_watch(connection, adapter->bonding->listener_id);

	if (adapter->bonding->io_id)
		g_source_remove(adapter->bonding->io_id);
	g_io_channel_close(adapter->bonding->io);
	bonding_request_free(adapter->bonding);
	adapter->bonding = NULL;
}

void hcid_dbus_inquiry_start(bdaddr_t *local)
{
	struct adapter *adapter;
	const gchar *path;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	adapter->discov_active = 1;
	/*
	 * Cancel pending remote name request and clean the device list
	 * when inquiry is supported in periodic inquiry idle state.
	 */
	if (adapter->pdiscov_active)
		pending_remote_name_cancel(adapter);

	/* Disable name resolution for non D-Bus clients */
	if (!adapter->discov_requestor)
		adapter->discov_type &= ~RESOLVE_NAME;

	path = adapter_get_path(adapter);

	dbus_connection_emit_property_changed(connection, path,
			ADAPTER_INTERFACE, "PeriodicDiscovery",
			DBUS_TYPE_BOOLEAN, &adapter->discov_active);

	g_dbus_emit_signal(connection, path,
			ADAPTER_INTERFACE, "DiscoveryStarted",
			DBUS_TYPE_INVALID);
}

int found_device_req_name(struct adapter *adapter)
{
	struct hci_request rq;
	evt_cmd_status rp;
	remote_name_req_cp cp;
	struct remote_dev_info match;
	GSList *l;
	int dd, req_sent = 0;
	uint16_t dev_id = adapter_get_dev_id(adapter);

	/* get the next remote address */
	if (!adapter->found_devices)
		return -ENODATA;

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUIRED;

	l = g_slist_find_custom(adapter->found_devices, &match,
					(GCompareFunc) found_device_cmp);
	if (!l)
		return -ENODATA;

	dd = hci_open_dev(dev_id);
	if (dd < 0)
		return -errno;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_REMOTE_NAME_REQ;
	rq.cparam = &cp;
	rq.clen   = REMOTE_NAME_REQ_CP_SIZE;
	rq.rparam = &rp;
	rq.rlen   = EVT_CMD_STATUS_SIZE;
	rq.event  = EVT_CMD_STATUS;

	/* send at least one request or return failed if the list is empty */
	do {
		struct remote_dev_info *dev = l->data;

		 /* flag to indicate the current remote name requested */
		dev->name_status = NAME_REQUESTED;

		memset(&rp, 0, sizeof(rp));
		memset(&cp, 0, sizeof(cp));
		bacpy(&cp.bdaddr, &dev->bdaddr);
		cp.pscan_rep_mode = 0x02;

		if (hci_send_req(dd, &rq, 500) < 0)
			error("Unable to send the HCI remote name request: %s (%d)",
						strerror(errno), errno);

		if (!rp.status) {
			req_sent = 1;
			break;
		}

		error("Remote name request failed with status 0x%02x",
			rp.status);

		/* if failed, request the next element */
		/* remove the element from the list */
		adapter->found_devices = g_slist_remove(adapter->found_devices, dev);
		g_free(dev);

		/* get the next element */
		l = g_slist_find_custom(adapter->found_devices, &match,
					(GCompareFunc) found_device_cmp);

	} while (l);

	hci_close_dev(dd);

	if (!req_sent)
		return -ENODATA;

	return 0;
}

static void send_out_of_range(const char *path, GSList *l)
{
	while (l) {
		const char *peer_addr = l->data;

		g_dbus_emit_signal(connection, path,
				ADAPTER_INTERFACE, "DeviceDisappeared",
				DBUS_TYPE_STRING, &peer_addr,
				DBUS_TYPE_INVALID);

		l = l->next;
	}
}

void hcid_dbus_inquiry_complete(bdaddr_t *local)
{
	struct adapter *adapter;
	struct remote_dev_info *dev;
	bdaddr_t tmp;
	const gchar *path;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("Unable to find matching adapter");
		return;
	}

	path = adapter_get_path(adapter);

	/* Out of range verification */
	if (adapter->pdiscov_active && !adapter->discov_active) {
		GSList *l;

		send_out_of_range(path, adapter->oor_devices);

		g_slist_foreach(adapter->oor_devices, (GFunc) free, NULL);
		g_slist_free(adapter->oor_devices);
		adapter->oor_devices = NULL;

		l = adapter->found_devices;
		while (l) {
			dev = l->data;
			baswap(&tmp, &dev->bdaddr);
			adapter->oor_devices = g_slist_append(adapter->oor_devices,
								batostr(&tmp));
			l = l->next;
		}
	}

	adapter->pinq_idle = 1;

	/*
	 * Enable resolution again: standard inquiry can be
	 * received in the periodic inquiry idle state.
	 */
	if (adapter->pdiscov_requestor && adapter->pdiscov_resolve_names)
		adapter->discov_type |= RESOLVE_NAME;

	/*
	 * The following scenarios can happen:
	 * 1. standard inquiry: always send discovery completed signal
	 * 2. standard inquiry + name resolving: send discovery completed
	 *    after name resolving
	 * 3. periodic inquiry: skip discovery completed signal
	 * 4. periodic inquiry + standard inquiry: always send discovery
	 *    completed signal
	 *
	 * Keep in mind that non D-Bus requests can arrive.
	 */

	if (!found_device_req_name(adapter))
		return;		/* skip - there is name to resolve */

	if (adapter->discov_active) {
		g_dbus_emit_signal(connection, path,
				ADAPTER_INTERFACE, "DiscoveryCompleted",
				DBUS_TYPE_INVALID);

		adapter->discov_active = 0;
	}

	/* free discovered devices list */
	g_slist_foreach(adapter->found_devices, (GFunc) g_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	if (adapter->discov_requestor) {
		g_dbus_remove_watch(connection, adapter->discov_listener);
		adapter->discov_listener = 0;
		g_free(adapter->discov_requestor);
		adapter->discov_requestor = NULL;

		/* If there is a pending reply for discovery cancel */
		if (adapter->discovery_cancel) {
			DBusMessage *reply;
			reply = dbus_message_new_method_return(adapter->discovery_cancel);
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
			dbus_message_unref(adapter->discovery_cancel);
			adapter->discovery_cancel = NULL;
		}

		/* reset the discover type for standard inquiry only */
		adapter->discov_type &= ~STD_INQUIRY;
	}
}

void hcid_dbus_periodic_inquiry_start(bdaddr_t *local, uint8_t status)
{
	struct adapter *adapter;
	const gchar *path;

	/* Don't send the signal if the cmd failed */
	if (status)
		return;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	adapter->pdiscov_active = 1;

	/* Disable name resolution for non D-Bus clients */
	if (!adapter->pdiscov_requestor)
		adapter->discov_type &= ~RESOLVE_NAME;

	path = adapter_get_path(adapter);

	dbus_connection_emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "PeriodicDiscovery",
				DBUS_TYPE_BOOLEAN, &adapter->pdiscov_active);
}

void hcid_dbus_periodic_inquiry_exit(bdaddr_t *local, uint8_t status)
{
	struct adapter *adapter;
	const gchar *path;

	/* Don't send the signal if the cmd failed */
	if (status)
		return;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	/* reset the discover type to be able to handle D-Bus and non D-Bus
	 * requests */
	adapter->pdiscov_active = 0;
	adapter->discov_type &= ~(PERIODIC_INQUIRY | RESOLVE_NAME);

	/* free discovered devices list */
	g_slist_foreach(adapter->found_devices, (GFunc) g_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	/* free out of range devices list */
	g_slist_foreach(adapter->oor_devices, (GFunc) free, NULL);
	g_slist_free(adapter->oor_devices);
	adapter->oor_devices = NULL;

	if (adapter->pdiscov_requestor) {
		g_dbus_remove_watch(connection, adapter->pdiscov_listener);
		adapter->pdiscov_listener = 0;
		g_free(adapter->pdiscov_requestor);
		adapter->pdiscov_requestor = NULL;
	}

	path = adapter_get_path(adapter);

	/* workaround: inquiry completed is not sent when exiting from
	  * periodic inquiry */
	if (adapter->discov_active) {
		g_dbus_emit_signal(connection, path,
				ADAPTER_INTERFACE, "DiscoveryCompleted",
				DBUS_TYPE_INVALID);

		adapter->discov_active = 0;
	}

	/* Send discovery completed signal if there isn't name to resolve */
	dbus_connection_emit_property_changed(connection, path,
				ADAPTER_INTERFACE, "PeriodicDiscovery",
				DBUS_TYPE_BOOLEAN, &adapter->discov_active);
}

static char *extract_eir_name(uint8_t *data, uint8_t *type)
{
	if (!data || !type)
		return NULL;

	if (data[0] == 0)
		return NULL;

	*type = data[1];

	switch (*type) {
	case 0x08:
	case 0x09:
		return strndup((char *) (data + 2), data[0] - 1);
	}

	return NULL;
}

static void append_dict_valist(DBusMessageIter *iter,
					const char *first_key,
					va_list var_args)
{
	DBusMessageIter dict;
	const char *key;
	int type;
	void *val;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING, &dict);

	key = first_key;
	while (key) {
		type = va_arg(var_args, int);
		val = va_arg(var_args, void *);
		dbus_message_iter_append_dict_entry(&dict, key, type, val);
		key = va_arg(var_args, char *);
	}

	dbus_message_iter_close_container(iter, &dict);
}

static void emit_device_found(const char *path, const char *address,
				const char *first_key, ...)
{
	DBusMessage *signal;
	DBusMessageIter iter;
	va_list var_args;

	signal = dbus_message_new_signal(path, ADAPTER_INTERFACE,
					"DeviceFound");
	if (!signal) {
		error("Unable to allocate new %s.DeviceFound signal",
				ADAPTER_INTERFACE);
		return;
	}
	dbus_message_iter_init_append(signal, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &address);

	va_start(var_args, first_key);
	append_dict_valist(&iter, first_key, var_args);
	va_end(var_args);

	dbus_connection_send(connection, signal, NULL);

	dbus_message_unref(signal);
}

void hcid_dbus_inquiry_result(bdaddr_t *local, bdaddr_t *peer, uint32_t class,
				int8_t rssi, uint8_t *data)
{
	char filename[PATH_MAX + 1];
	struct adapter *adapter;
	GSList *l;
	char local_addr[18], peer_addr[18], *name, *tmp_name;
	const char *paddr = peer_addr;
	struct remote_dev_info match;
	dbus_int16_t tmp_rssi = rssi;
	uint8_t name_type = 0x00;
	name_status_t name_status;
	const gchar *path;

	ba2str(local, local_addr);
	ba2str(peer, peer_addr);

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	write_remote_class(local, peer, class);

	if (data)
		write_remote_eir(local, peer, data);

	/*
	 * workaround to identify situation when the daemon started and
	 * a standard inquiry or periodic inquiry was already running
	 */
	if (!adapter->discov_active && !adapter->pdiscov_active)
		adapter->pdiscov_active = 1;

	/* reset the idle flag when the inquiry complete event arrives */
	if (adapter->pdiscov_active) {
		adapter->pinq_idle = 0;

		/* Out of range list update */
		l = g_slist_find_custom(adapter->oor_devices, peer_addr,
				(GCompareFunc) strcmp);
		if (l) {
			char *dev = l->data;
			adapter->oor_devices = g_slist_remove(adapter->oor_devices,
								dev);
			g_free(dev);
		}
	}

	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, peer);
	match.name_status = NAME_SENT;
	/* if found: don't send the name again */
	l = g_slist_find_custom(adapter->found_devices, &match,
			(GCompareFunc) found_device_cmp);
	if (l)
		return;

	/* the inquiry result can be triggered by NON D-Bus client */
	if (adapter->discov_type & RESOLVE_NAME)
		name_status = NAME_REQUIRED;
	else
		name_status = NAME_NOT_REQUIRED;

	create_name(filename, PATH_MAX, STORAGEDIR, local_addr, "names");
	name = textfile_get(filename, peer_addr);

	tmp_name = extract_eir_name(data, &name_type);
	if (tmp_name) {
		if (name_type == 0x09) {
			write_device_name(local, peer, tmp_name);
			name_status = NAME_NOT_REQUIRED;

			if (name)
				g_free(name);

			name = tmp_name;
		} else {
			if (name)
				free(tmp_name);
			else
				name = tmp_name;
		}
	}

	path = adapter_get_path(adapter);

	if (name) {
		if (name_type != 0x08)
			name_status = NAME_SENT;

		emit_device_found(path, paddr,
				"Address", DBUS_TYPE_STRING, &paddr,
				"Class", DBUS_TYPE_UINT32, &class,
				"RSSI", DBUS_TYPE_INT16, &tmp_rssi,
				"Name", DBUS_TYPE_STRING, &name,
				NULL);

		g_free(name);
	} else {
		emit_device_found(path, paddr,
				"Address", DBUS_TYPE_STRING, &paddr,
				"Class", DBUS_TYPE_UINT32, &class,
				"RSSI", DBUS_TYPE_INT16, &tmp_rssi,
				NULL);
	}

	/* add in the list to track name sent/pending */
	found_device_add(&adapter->found_devices, peer, rssi, name_status);
}

void hcid_dbus_remote_class(bdaddr_t *local, bdaddr_t *peer, uint32_t class)
{
	char peer_addr[18];
	const char *paddr = peer_addr;
	uint32_t old_class = 0;
	struct adapter *adapter;
	GSList *l;
	struct btd_device *device;
	const gchar *dev_path;

	read_remote_class(local, peer, &old_class);

	if (old_class == class)
		return;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	ba2str(peer, peer_addr);

	l = g_slist_find_custom(adapter->devices, paddr,
				(GCompareFunc) device_address_cmp);
	if (!l)
		return;

	device = l->data;

	dev_path = device_get_path(device);

	dbus_connection_emit_property_changed(connection, dev_path,
				DEVICE_INTERFACE, "Class",
				DBUS_TYPE_UINT32, &class);
}

void hcid_dbus_remote_name(bdaddr_t *local, bdaddr_t *peer, uint8_t status,
				char *name)
{
	struct adapter *adapter;
	char peer_addr[18];
	const char *paddr = peer_addr;
	const gchar *dev_path;
	const gchar *path;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	ba2str(peer, peer_addr);

	if (!status) {
		struct btd_device *device;

		device = adapter_find_device(adapter, paddr);
		if (device) {

			dev_path = device_get_path(device);

			dbus_connection_emit_property_changed(connection,
						dev_path, DEVICE_INTERFACE,
						"Name", DBUS_TYPE_STRING, &name);
		}
	}

	/* remove from remote name request list */
	found_device_remove(&adapter->found_devices, peer);

	/* check if there is more devices to request names */
	if (!found_device_req_name(adapter))
		return; /* skip if a new request has been sent */

	/* free discovered devices list */
	g_slist_foreach(adapter->found_devices, (GFunc) g_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	/* The discovery completed signal must be sent only for discover
	 * devices request WITH name resolving */
	if (adapter->discov_requestor) {
		g_dbus_remove_watch(connection, adapter->discov_listener);
		adapter->discov_listener = 0;
		g_free(adapter->discov_requestor);
		adapter->discov_requestor = NULL;

		/* If there is a pending reply for discovery cancel */
		if (adapter->discovery_cancel) {
			DBusMessage *reply;
			reply = dbus_message_new_method_return(adapter->discovery_cancel);
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
			dbus_message_unref(adapter->discovery_cancel);
			adapter->discovery_cancel = NULL;
		}

		/* Disable name resolution for non D-Bus clients */
		if (!adapter->pdiscov_requestor)
			adapter->discov_type &= ~RESOLVE_NAME;
	}

	path = adapter_get_path(adapter);

	if (adapter->discov_active) {
		g_dbus_emit_signal(connection, path,
				ADAPTER_INTERFACE, "DiscoveryCompleted",
				DBUS_TYPE_INVALID);

		adapter->discov_active = 0;
	}
}

void hcid_dbus_conn_complete(bdaddr_t *local, uint8_t status, uint16_t handle,
				bdaddr_t *peer)
{
	char peer_addr[18];
	const char *paddr = peer_addr;
	struct adapter *adapter;
	const gchar *dev_path;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	ba2str(peer, peer_addr);

	if (status) {
		struct pending_auth_info *auth;

		auth = adapter_find_auth_request(adapter, peer);
		if (auth && auth->agent)
			agent_cancel(auth->agent);

		adapter_remove_auth_request(adapter, peer);

		if (adapter->bonding)
			adapter->bonding->hci_status = status;
	} else {
		struct btd_device *device;
		gboolean connected = TRUE;

		device = adapter_find_device(adapter, paddr);
		if (device) {

		  dev_path = device_get_path(device);

		  dbus_connection_emit_property_changed(connection,
					dev_path, DEVICE_INTERFACE,
					"Connected", DBUS_TYPE_BOOLEAN,
					&connected);
		}

		/* add in the active connetions list */
		active_conn_append(&adapter->active_conn, peer, handle);
	}
}

void hcid_dbus_disconn_complete(bdaddr_t *local, uint8_t status,
				uint16_t handle, uint8_t reason)
{
	DBusMessage *reply;
	char peer_addr[18];
	const char *paddr = peer_addr;
	struct adapter *adapter;
	struct btd_device *device;
	struct active_conn_info *dev;
	GSList *l;
	gboolean connected = FALSE;
	struct pending_auth_info *auth;
	const gchar *destination;
	const gchar *dev_path;
	uint16_t dev_id;

	if (status) {
		error("Disconnection failed: 0x%02x", status);
		return;
	}

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	l = g_slist_find_custom(adapter->active_conn, &handle,
				active_conn_find_by_handle);

	if (!l)
		return;

	dev = l->data;

	ba2str(&dev->bdaddr, peer_addr);

	dev_id = adapter_get_dev_id(adapter);

	/* clean pending HCI cmds */
	hci_req_queue_remove(dev_id, &dev->bdaddr);

	/* Cancel D-Bus/non D-Bus requests */
	auth = adapter_find_auth_request(adapter, &dev->bdaddr);
	if (auth && auth->agent)
		agent_cancel(auth->agent);

	adapter_remove_auth_request(adapter, &dev->bdaddr);

	/* Check if there is a pending CreateBonding request */
	if (adapter->bonding && (bacmp(&adapter->bonding->bdaddr, &dev->bdaddr) == 0)) {
		if (adapter->bonding->cancel) {
			/* reply authentication canceled */
			reply = new_authentication_return(adapter->bonding->msg,
							HCI_OE_USER_ENDED_CONNECTION);
			g_dbus_send_message(connection, reply);
		} else {
			reply = new_authentication_return(adapter->bonding->msg,
							HCI_AUTHENTICATION_FAILURE);
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
		}

		g_dbus_remove_watch(adapter->bonding->conn,
					adapter->bonding->listener_id);

		if (adapter->bonding->io_id)
			g_source_remove(adapter->bonding->io_id);
		g_io_channel_close(adapter->bonding->io);
		bonding_request_free(adapter->bonding);
		adapter->bonding = NULL;
	}

	adapter->active_conn = g_slist_remove(adapter->active_conn, dev);
	g_free(dev);

	device = adapter_find_device(adapter, paddr);
	if (device) {
		destination = device_get_address(device);
		dev_path = device_get_path(device);

		dbus_connection_emit_property_changed(connection,
					dev_path, DEVICE_INTERFACE,
					"Connected", DBUS_TYPE_BOOLEAN,
					&connected);
		if (device_is_temporary(device)) {
			debug("Removing temporary device %s", destination);
			adapter_remove_device(connection, adapter, device);
		}
	}
}

int set_limited_discoverable(int dd, const uint8_t *cls, gboolean limited)
{
	uint32_t dev_class;
	int err;
	int num = (limited ? 2 : 1);
	uint8_t lap[] = { 0x33, 0x8b, 0x9e, 0x00, 0x8b, 0x9e };
	/*
	 * 1: giac
	 * 2: giac + liac
	 */
	if (hci_write_current_iac_lap(dd, num, lap, 1000) < 0) {
		err = errno;
		error("Can't write current IAC LAP: %s(%d)",
				strerror(err), err);
		return -err;
	}

	if (limited) {
		if (cls[1] & 0x20)
			return 0; /* Already limited */

		dev_class = (cls[2] << 16) | ((cls[1] | 0x20) << 8) | cls[0];
	} else {
		if (!(cls[1] & 0x20))
			return 0; /* Already clear */

		dev_class = (cls[2] << 16) | ((cls[1] & 0xdf) << 8) | cls[0];
	}

	if (hci_write_class_of_dev(dd, dev_class, 1000) < 0) {
		err = errno;
		error("Can't write class of device: %s (%d)",
							strerror(err), err);
		return -err;
	}

	return 0;
}

int set_service_classes(int dd, const uint8_t *cls, uint8_t value)
{
	uint32_t dev_class;
	int err;

	if (cls[2] == value)
		return 0; /* Already set */

	dev_class = (value << 16) | (cls[1] << 8) | cls[0];

	if (hci_write_class_of_dev(dd, dev_class, 1000) < 0) {
		err = errno;
		error("Can't write class of device: %s (%d)",
							strerror(err), err);
		return -err;
	}

	return 0;
}

/* Section reserved to device HCI callbacks */

void hcid_dbus_setname_complete(bdaddr_t *local)
{
	int id, dd = -1;
	read_local_name_rp rp;
	struct hci_request rq;
	const char *pname = (char *) rp.name;
	char local_addr[18], name[249];

	ba2str(local, local_addr);

	id = hci_devid(local_addr);
	if (id < 0) {
		error("No matching device id for %s", local_addr);
		return;
	}

	dd = hci_open_dev(id);
	if (dd < 0) {
		error("HCI device open failed: hci%d", id);
		memset(&rp, 0, sizeof(rp));
	} else {
		memset(&rq, 0, sizeof(rq));
		rq.ogf    = OGF_HOST_CTL;
		rq.ocf    = OCF_READ_LOCAL_NAME;
		rq.rparam = &rp;
		rq.rlen   = READ_LOCAL_NAME_RP_SIZE;
		rq.event  = EVT_CMD_COMPLETE;

		if (hci_send_req(dd, &rq, 1000) < 0) {
			error("Sending getting name command failed: %s (%d)",
						strerror(errno), errno);
			rp.name[0] = '\0';
		} else if (rp.status) {
			error("Getting name failed with status 0x%02x",
					rp.status);
			rp.name[0] = '\0';
		}
		hci_close_dev(dd);
	}

	strncpy(name, pname, sizeof(name) - 1);
	name[248] = '\0';
	pname = name;
}

void hcid_dbus_setscan_enable_complete(bdaddr_t *local)
{
	struct adapter *adapter;
	read_scan_enable_rp rp;
	struct hci_request rq;
	int dd = -1;
	uint16_t dev_id;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	dev_id = adapter_get_dev_id(adapter);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("HCI device open failed: hci%d", dev_id);
		return;
	}

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_HOST_CTL;
	rq.ocf    = OCF_READ_SCAN_ENABLE;
	rq.rparam = &rp;
	rq.rlen   = READ_SCAN_ENABLE_RP_SIZE;
	rq.event  = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, 1000) < 0) {
		error("Sending read scan enable command failed: %s (%d)",
				strerror(errno), errno);
		goto failed;
	}

	if (rp.status) {
		error("Getting scan enable failed with status 0x%02x",
				rp.status);
		goto failed;
	}

	adapter_remove_discov_timeout(adapter);

	if (adapter_get_scan_mode(adapter) != rp.enable)
		adapter_mode_changed(adapter, rp.enable);

failed:
	if (dd >= 0)
		hci_close_dev(dd);
}

void hcid_dbus_write_class_complete(bdaddr_t *local)
{
	struct adapter *adapter;
	int dd;
	uint8_t cls[3];
	uint16_t dev_id;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	dev_id = adapter_get_dev_id(adapter);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("HCI device open failed: hci%d", dev_id);
		return;
	}

	if (hci_read_class_of_dev(dd, cls, 1000) < 0) {
		error("Can't read class of device on hci%d: %s (%d)",
			dev_id, strerror(errno), errno);
		hci_close_dev(dd);
		return;
	}

	write_local_class(local, cls);
	adapter_set_class(adapter, cls);

	hci_close_dev(dd);
}

void hcid_dbus_write_simple_pairing_mode_complete(bdaddr_t *local)
{
	struct adapter *adapter;
	int dd;
	uint8_t mode;
	uint16_t dev_id;
	const gchar *path;

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return;
	}

	dev_id = adapter_get_dev_id(adapter);
	path = adapter_get_path(adapter);

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		error("HCI adapter open failed: %s", path);
		return;
	}

	if (hci_read_simple_pairing_mode(dd, &mode, 1000) < 0) {
		error("Can't read class of adapter on %s: %s(%d)",
					path, strerror(errno), errno);
		hci_close_dev(dd);
		return;
	}

	adapter_update_ssp_mode(adapter, dd, mode);

	hci_close_dev(dd);
}

int hcid_dbus_get_io_cap(bdaddr_t *local, bdaddr_t *remote,
						uint8_t *cap, uint8_t *auth)
{
	struct adapter *adapter;
	struct btd_device *device;
	struct agent *agent;
	char addr[18];

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	if (get_auth_requirements(local, remote, auth) < 0)
		return -1;

	ba2str(remote, addr);

	device = adapter_find_device(adapter, addr);
	if (device) {
		agent = device_get_agent(device);
		if (agent)
			*auth = 0x03;
	}
	if (!agent)
		agent = adapter->agent;

	if (!agent) {
		if (!(*auth & 0x01)) {
			/* No input, no output */
			*cap = 0x03;
			return 0;
		}
		error("No agent available for IO capability");
		return -1;
	}

	*cap = agent_get_io_capability(agent);

	return 0;
}

int hcid_dbus_set_io_cap(bdaddr_t *local, bdaddr_t *remote,
                                                uint8_t cap, uint8_t auth)
{
	struct adapter *adapter;
	struct btd_device *device;
	char addr[18];

	adapter = manager_find_adapter(local);
	if (!adapter) {
		error("No matching adapter found");
		return -1;
	}

	ba2str(remote, addr);

	device = adapter_get_device(connection, adapter, addr);
	if (device) {
		device_set_cap(device, cap);
		device_set_auth(device, auth);
	}

	return 0;
}

static int inquiry_cancel(int dd, int to)
{
	struct hci_request rq;
	uint8_t status;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_INQUIRY_CANCEL;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);
	rq.event = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = bt_error(status);
		return -1;
	}

	return 0;
}

static int remote_name_cancel(int dd, bdaddr_t *dba, int to)
{
	remote_name_req_cancel_cp cp;
	struct hci_request rq;
	uint8_t status;

	memset(&rq, 0, sizeof(rq));
	memset(&cp, 0, sizeof(cp));

	bacpy(&cp.bdaddr, dba);

	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_REMOTE_NAME_REQ_CANCEL;
	rq.cparam = &cp;
	rq.clen   = REMOTE_NAME_REQ_CANCEL_CP_SIZE;
	rq.rparam = &status;
	rq.rlen = sizeof(status);
	rq.event = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = bt_error(status);
		return -1;
	}

	return 0;
}

int cancel_discovery(struct adapter *adapter)
{
	struct remote_dev_info *dev, match;
	GSList *l;
	int dd, err = 0;
	uint16_t dev_id = adapter_get_dev_id(adapter);

	if (!adapter->discov_active)
		goto cleanup;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = -ENODEV;
		goto cleanup;
	}

	/*
	 * If there is a pending read remote name request means
	 * that the inquiry complete event was already received
	 */
	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUESTED;

	l = g_slist_find_custom(adapter->found_devices, &match,
				(GCompareFunc) found_device_cmp);
	if (l) {
		dev = l->data;
		if (remote_name_cancel(dd, &dev->bdaddr, 1000) < 0) {
			error("Read remote name cancel failed: %s, (%d)",
					strerror(errno), errno);
			err = -errno;
		}
	} else {
		if (inquiry_cancel(dd, 1000) < 0) {
			error("Inquiry cancel failed:%s (%d)",
					strerror(errno), errno);
			err = -errno;
		}
	}

	hci_close_dev(dd);

cleanup:
	/*
	 * Reset discov_requestor and discover_state in the remote name
	 * request event handler or in the inquiry complete handler.
	 */
	g_slist_foreach(adapter->found_devices, (GFunc) g_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	/* Disable name resolution for non D-Bus clients */
	if (!adapter->pdiscov_requestor)
		adapter->discov_type &= ~RESOLVE_NAME;

	return err;
}

static int periodic_inquiry_exit(int dd, int to)
{
	struct hci_request rq;
	uint8_t status;

	memset(&rq, 0, sizeof(rq));
	rq.ogf    = OGF_LINK_CTL;
	rq.ocf    = OCF_EXIT_PERIODIC_INQUIRY;
	rq.rparam = &status;
	rq.rlen   = sizeof(status);
	rq.event = EVT_CMD_COMPLETE;

	if (hci_send_req(dd, &rq, to) < 0)
		return -1;

	if (status) {
		errno = status;
		return -1;
	}

	return 0;
}

int cancel_periodic_discovery(struct adapter *adapter)
{
	struct remote_dev_info *dev, match;
	GSList *l;
	int dd, err = 0;
	uint16_t dev_id = adapter_get_dev_id(adapter);

	if (!adapter->pdiscov_active)
		goto cleanup;

	dd = hci_open_dev(dev_id);
	if (dd < 0) {
		err = -ENODEV;
		goto cleanup;
	}
	/* find the pending remote name request */
	memset(&match, 0, sizeof(struct remote_dev_info));
	bacpy(&match.bdaddr, BDADDR_ANY);
	match.name_status = NAME_REQUESTED;

	l = g_slist_find_custom(adapter->found_devices, &match,
			(GCompareFunc) found_device_cmp);
	if (l) {
		dev = l->data;
		if (remote_name_cancel(dd, &dev->bdaddr, 1000) < 0) {
			error("Read remote name cancel failed: %s, (%d)",
					strerror(errno), errno);
			err = -errno;
		}
	}

	/* ovewrite err if necessary: stop periodic inquiry has higher
	 * priority */
	if (periodic_inquiry_exit(dd, 1000) < 0) {
		error("Periodic Inquiry exit failed:%s (%d)",
				strerror(errno), errno);
		err = -errno;
	}

	hci_close_dev(dd);

cleanup:
	/*
	 * Reset pdiscov_requestor and pdiscov_active is done when the
	 * cmd complete event for exit periodic inquiry mode cmd arrives.
	 */
	g_slist_foreach(adapter->found_devices, (GFunc) g_free, NULL);
	g_slist_free(adapter->found_devices);
	adapter->found_devices = NULL;

	return err;
}

/* Most of the functions in this module require easy access to a connection so
 * we keep it global here and provide these access functions the other (few)
 * modules that require access to it */

void set_dbus_connection(DBusConnection *conn)
{
	connection = conn;
}

DBusConnection *get_dbus_connection(void)
{
	return connection;
}