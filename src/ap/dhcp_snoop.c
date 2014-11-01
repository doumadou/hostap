/*
 * DHCP snooping for Proxy ARP
 * Copyright (c) 2014, Qualcomm Atheros, Inc.
 *
 * This software may be distributed under the terms of the BSD license.
 * See README for more details.
 */

#include "utils/includes.h"
#include <linux/ip.h>
#include <linux/udp.h>

#include "utils/common.h"
#include "l2_packet/l2_packet.h"
#include "hostapd.h"
#include "sta_info.h"
#include "ap_drv_ops.h"
#include "x_snoop.h"
#include "dhcp_snoop.h"

struct bootp_pkt {
	struct iphdr iph;
	struct udphdr udph;
	u8 op;
	u8 htype;
	u8 hlen;
	u8 hops;
	be32 xid;
	be16 secs;
	be16 flags;
	be32 client_ip;
	be32 your_ip;
	be32 server_ip;
	be32 relay_ip;
	u8 hw_addr[16];
	u8 serv_name[64];
	u8 boot_file[128];
	u8 exten[312];
};

#define DHCPACK	5
static const u8 ic_bootp_cookie[] = { 99, 130, 83, 99 };


static void handle_dhcp(void *ctx, const u8 *src_addr, const u8 *buf,
			size_t len)
{
	struct hostapd_data *hapd = ctx;
	const struct bootp_pkt *b;
	struct sta_info *sta;
	int exten_len;
	const u8 *end, *pos;
	int res, msgtype = 0, prefixlen = 32;
	u32 subnet_mask = 0;

	exten_len = len - ETH_HLEN - (sizeof(*b) - sizeof(b->exten));
	if (exten_len < 4)
		return;

	b = (const struct bootp_pkt *) &buf[ETH_HLEN];
	if (os_memcmp(b->exten, ic_bootp_cookie, ARRAY_SIZE(ic_bootp_cookie)))
		return;

	/* Parse DHCP options */
	end = (const u8 *) b + ntohs(b->iph.tot_len);
	pos = &b->exten[4];
	while (pos < end && *pos != 0xff) {
		const u8 *opt = pos++;

		if (*opt == 0) /* padding */
			continue;

		pos += *pos + 1;
		if (pos >= end)
			break;

		switch (*opt) {
		case 1:  /* subnet mask */
			if (opt[1] == 4)
				subnet_mask = WPA_GET_BE32(&opt[2]);
			if (subnet_mask == 0)
				return;
			while (!(subnet_mask & 0x1)) {
				subnet_mask >>= 1;
				prefixlen--;
			}
			break;
		case 53: /* message type */
			if (opt[1])
				msgtype = opt[2];
			break;
		default:
			break;
		}
	}

	if (msgtype == DHCPACK) {
		if (b->your_ip == 0)
			return;

		/* DHCPACK for DHCPREQUEST */
		sta = ap_get_sta(hapd, b->hw_addr);
		if (!sta)
			return;

		wpa_printf(MSG_DEBUG, "dhcp_snoop: Found DHCPACK for " MACSTR
			   " @ IPv4 address %X/%d",
			   MAC2STR(sta->addr), ntohl(b->your_ip), prefixlen);

		if (sta->ipaddr == b->your_ip)
			return;

		if (sta->ipaddr != 0) {
			wpa_printf(MSG_DEBUG,
				   "dhcp_snoop: Removing IPv4 address %X from the ip neigh table",
				   sta->ipaddr);
			hostapd_drv_br_delete_ip_neigh(hapd, sta->ipaddr);
		}

		res = hostapd_drv_br_add_ip_neigh(hapd, b->your_ip, prefixlen,
						  sta->addr);
		if (res) {
			wpa_printf(MSG_DEBUG,
				   "dhcp_snoop: Adding ip neigh table failed: %d",
				   res);
			return;
		}
		sta->ipaddr = b->your_ip;
	}
}


int dhcp_snoop_init(struct hostapd_data *hapd)
{
	hapd->sock_dhcp = x_snoop_get_l2_packet(hapd, handle_dhcp,
						L2_PACKET_FILTER_DHCP);
	if (hapd->sock_dhcp == NULL) {
		wpa_printf(MSG_DEBUG,
			   "dhcp_snoop: Failed to initialize L2 packet processing for DHCP packet: %s",
			   strerror(errno));
		return -1;
	}

	return 0;
}


void dhcp_snoop_deinit(struct hostapd_data *hapd)
{
	l2_packet_deinit(hapd->sock_dhcp);
}
