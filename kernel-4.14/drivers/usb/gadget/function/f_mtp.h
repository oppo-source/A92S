/*
 * Copyright (C) 2014 Google, Inc.
 * Author: Badhri Jagan Sridharan <badhri@android.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/ioctl.h>
#include <linux/types.h>

extern struct usb_function_instance *alloc_inst_mtp_ptp(bool mtp_config);
extern struct usb_function *function_alloc_mtp_ptp(
			struct usb_function_instance *fi, bool mtp_config);

struct mtp_file_range {
    /* file descriptor for file to transfer */
	int         fd;
    /* offset in file for start of transfer */
	loff_t      offset;
    /* number of bytes to transfer */
	int64_t     length;
    /* MTP command ID for data header,
     * used only for MTP_SEND_FILE_WITH_HEADER
     */
	uint16_t    command;
    /* MTP transaction ID for data header,
     * used only for MTP_SEND_FILE_WITH_HEADER
     */
	uint32_t    transaction_id;
};

struct mtp_event {
    /* size of the event */
	size_t      length;
    /* event data to send */
	void        *data;
};

/* Sends the specified file range to the host */
#define MTP_SEND_FILE              _IOW('M', 0, struct mtp_file_range)
/* Receives data from the host and writes it to a file.
 * The file is created if it does not exist.
 */
#define MTP_RECEIVE_FILE           _IOW('M', 1, struct mtp_file_range)
/* Sends an event to the host via the interrupt endpoint */
#define MTP_SEND_EVENT             _IOW('M', 3, struct mtp_event)
/* Sends the specified file range to the host,
 * with a 12 byte MTP data packet header at the beginning.
 */
#define MTP_SEND_FILE_WITH_HEADER  _IOW('M', 4, struct mtp_file_range)

#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif

#ifdef __KERNEL__
#ifdef CONFIG_COMPAT
struct __compat_mtp_event {
	compat_size_t   length;
	compat_caddr_t  data;
};
#ifdef VENDOR_EDIT
//Tong.Han@BSP.Group.Tp, 2016/11/01, transplant the code from mt6755(L_MR1) for MTP compt 32 bit & 64 bitT
struct __compat_mtp_file_range {
	compat_int_t	fd;
	compat_loff_t	offset;
	int64_t		length;
	uint16_t	command;
	uint32_t	transaction_id;
};

#define COMPAT_MTP_SEND_FILE              _IOW('M', 0, \
						struct __compat_mtp_file_range)
#define COMPAT_MTP_RECEIVE_FILE           _IOW('M', 1, \
						struct __compat_mtp_file_range)
#define COMPAT_MTP_SEND_EVENT             _IOW('M', 3, \
						struct __compat_mtp_event)
#define COMPAT_MTP_SEND_FILE_WITH_HEADER  _IOW('M', 4, \
						struct __compat_mtp_file_range)
#endif	/*VENDOR_EDIT*/
#endif
#endif

#define COMPAT_MTP_SEND_EVENT   _IOW('M', 3, struct __compat_mtp_event)


