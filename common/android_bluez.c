/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2009  Marcel Holtmann <marcel@holtmann.org>
 *  Copyright (C) 2009 The Android Open Source Project
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

#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>

#include <private/android_filesystem_config.h>
#include <sys/prctl.h>
#include <linux/capability.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/hci.h>

/* Set UID to bluetooth w/ CAP_NET_RAW, CAP_NET_ADMIN and CAP_NET_BIND_SERVICE
 * (Android's init.rc does not yet support applying linux capabilities) */
void android_set_aid_and_cap() {
	prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
	setuid(AID_BLUETOOTH);

	struct __user_cap_header_struct header;
	struct __user_cap_data_struct cap;
	header.version = _LINUX_CAPABILITY_VERSION;
	header.pid = 0;
	cap.effective = cap.permitted = 1 << CAP_NET_RAW |
					1 << CAP_NET_ADMIN |
					1 << CAP_NET_BIND_SERVICE;
	cap.inheritable = 0;
	capset(&header, &cap);
}

#ifdef BOARD_HAVE_BLUETOOTH_BCM
static int vendor_high_priority(int fd, uint16_t handle) {
    unsigned char hci_sleep_cmd[] = {
        0x01,               // HCI command packet
        0x57, 0xfc,         // HCI_Write_High_Priority_Connection
        0x02,               // Length
        0x00, 0x00          // Handle
    };

    hci_sleep_cmd[4] = (uint8_t)handle;
    hci_sleep_cmd[5] = (uint8_t)(handle >> 8);

    int ret = write(fd, hci_sleep_cmd, sizeof(hci_sleep_cmd));
    if (ret < 0) {
        error("write(): %s (%d)]", strerror(errno), errno);
        return -1;
    } else if (ret != sizeof(hci_sleep_cmd)) {
        error("write(): unexpected length %d", ret);
        return -1;
    }
    return 0;
}

static int get_hci_sock() {
    int sock = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    struct sockaddr_hci addr;
    int opt;

    if(sock < 0) {
        error("Can't create raw HCI socket!");
        return -1;
    }

    opt = 1;
    if (setsockopt(sock, SOL_HCI, HCI_DATA_DIR, &opt, sizeof(opt)) < 0) {
        error("Error setting data direction\n");
        return -1;
    }

    /* Bind socket to the HCI device */
    addr.hci_family = AF_BLUETOOTH;
    addr.hci_dev = 0;  // hci0
    if(bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        error("Can't attach to device hci0. %s(%d)\n",
             strerror(errno),
             errno);
        return -1;
    }
    return sock;
}

static int get_acl_handle(int fd, bdaddr_t *bdaddr) {
    int i;
    int ret = -1;
    struct hci_conn_list_req *conn_list;
    struct hci_conn_info *conn_info;
    int max_conn = 10;

    conn_list = malloc(max_conn * (
            sizeof(struct hci_conn_list_req) + sizeof(struct hci_conn_info)));
    if (!conn_list) {
        error("Out of memory in %s\n", __FUNCTION__);
        return -1;
    }

    conn_list->dev_id = 0;  /* hardcoded to HCI device 0 */
    conn_list->conn_num = max_conn;

    if (ioctl(fd, HCIGETCONNLIST, (void *)conn_list)) {
        error("Failed to get connection list\n");
        goto out;
    }

    for (i=0; i < conn_list->conn_num; i++) {
        conn_info = &conn_list->conn_info[i];
        if (conn_info->type == ACL_LINK &&
                !memcmp((void *)&conn_info->bdaddr, (void *)bdaddr,
                sizeof(bdaddr_t))) {
            ret = conn_info->handle;
            goto out;
        }
    }
    ret = 0;

out:
    free(conn_list);
    return ret;
}

/* Request that the ACL link to a given Bluetooth connection be high priority,
 * for improved coexistance support
 */
int android_set_high_priority(bdaddr_t *ba) {
    int ret;
    int fd = get_hci_sock();
    int acl_handle;

    if (fd < 0)
        return fd;

    acl_handle = get_acl_handle(fd, ba);
    if (acl_handle < 0) {
        ret = acl_handle;
        goto out;
    }

    ret = vendor_high_priority(fd, acl_handle);

out:
    close(fd);

    return ret;
}

#else

int android_set_high_priority(bdaddr_t *ba) {
    return 0;
}

#endif
