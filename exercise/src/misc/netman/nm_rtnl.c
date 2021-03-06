#include "iwlib.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ether.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

#include "securesoho.h"
#include "securesoho_wireless.h"

#include "dbus_service.h"
#include "nm_rtnl.h"

#define MYLOG_CATEGORY_NAME "net_daemon"
#include "mylog.h"


/*
 * Static information about wireless interface.
 * We cache this info for performance reason.
 */
typedef struct wireless_iface
{
	/* Linked list */
	struct wireless_iface *	next;

	/* Interface identification */
	int		ifindex;		/* Interface index == black magic */

	/* Interface data */
	char			ifname[IFNAMSIZ + 1];	/* Interface name */
	struct iw_range	range;			/* Wireless static data */
	int			has_range;
} wireless_iface;


/* Cache of wireless interfaces */
struct wireless_iface *	interface_cache = NULL;
/*
 * Dump a buffer as a serie of hex
 * Maybe should go in iwlib...
 * Maybe we should have better formatting like iw_print_key...
 */
static char *
iw_hexdump(char *		buf,
		size_t		buflen,
		const unsigned char *data,
		size_t		datalen)
{
	size_t	i;
	char *	pos = buf;

	for(i = 0; i < datalen; i++)
		pos += snprintf(pos, buf + buflen - pos, "%02X", data[i]);
	return buf;
}
/*
 * Get name of interface based on interface index...
 */
static int
index2name(int		skfd,
		int		ifindex,
		char *	name)
{
	struct ifreq	irq;
	int		ret = 0;

	memset(name, 0, IFNAMSIZ + 1);

	/* Get interface name */
	irq.ifr_ifindex = ifindex;
	if(ioctl(skfd, SIOCGIFNAME, &irq) < 0)
		ret = -1;
	else
		strncpy(name, irq.ifr_name, IFNAMSIZ);

	return(ret);
}
/*
 * Get interface data from cache or live interface
 */
static struct wireless_iface *
iw_get_interface_data(int	ifindex)
{
	struct wireless_iface *	curr;
	int				skfd = -1;	/* ioctl socket */

	/* Search for it in the database */
	curr = interface_cache;
	while(curr != NULL)
	{
		/* Match ? */
		if(curr->ifindex == ifindex)
		{
			mylog_trace("Cache : found %d-%s", curr->ifindex, curr->ifname);
			/* Return */
			return(curr);
		}
		/* Next entry */
		curr = curr->next;
	}

	/* Create a channel to the NET kernel. Doesn't happen too often, so
	 * socket creation overhead is minimal... */
	if((skfd = iw_sockets_open()) < 0)
	{
		mylog_error("iw_sockets_open error");
		return(NULL);
	}

	/* Create new entry, zero, init */
	curr = calloc(1, sizeof(struct wireless_iface));
	if(!curr)
	{
		mylog_error("Malloc failed");
		return(NULL);
	}
	curr->ifindex = ifindex;

	/* Extract static data */
	if(index2name(skfd, ifindex, curr->ifname) < 0)
	{
		mylog_error("index2name failed");
		free(curr);
		return(NULL);
	}
	curr->has_range = (iw_get_range_info(skfd, curr->ifname, &curr->range) >= 0);
	mylog_trace("Cache : create %d-%s", curr->ifindex, curr->ifname);

	/* Done */
	iw_sockets_close(skfd);

	/* Link it */
	curr->next = interface_cache;
	interface_cache = curr;

	return(curr);
}

/**
 * Get the interface name of the message
 * 
 */
static void 
process_link_status(struct ifinfomsg *msg, int bytes)
{
	struct rtattr *attr;
	const char *ifname = NULL;
	int operstate = 0; //IF_OPER_UNKNOWN

	for (attr = IFLA_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case IFLA_IFNAME:
				ifname = RTA_DATA(attr);
				mylog_trace("The message interface is %s", ifname);
				break;
			case IFLA_OPERSTATE:
				operstate = *((unsigned char *) RTA_DATA(attr));
				mylog_trace("The message operstae is %d", operstate);
				break;
		}
	}
/* 
 * RFC 2863 operational status 
 * enum {  
 *	   IF_OPER_UNKNOWN,
 *	   IF_OPER_NOTPRESENT,
 *	   IF_OPER_DOWN,
 *	   IF_OPER_LOWERLAYERDOWN,
 *	   IF_OPER_TESTING,
 *	   IF_OPER_DORMANT,
 *	   IF_OPER_UP,
 *  }; 
 */
	if (ifname && operstate == 6){
		nm_dbus_signal_device_status(ifname,0x80|72); //k for osde_network_link_up event
	} else if (ifname && operstate == 2){
		nm_dbus_signal_device_status(ifname, 0x80|73); //m for osde_network_link_down event
	}
}

static void process_wireless_custom_token(char *data, int len)
{
	printf("\033[1;45m ===============  %s(), %d   data = %s... ================== \033[0m	\n", __FUNCTION__, __LINE__, data);
	int state;

	if(!data){
		mylog_error("The data is null");
		return ;
	}

	if(strstr(data, "PBC Session Overlap")){
		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_IN_PROGRESS;
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, "WPS status success")){
		SecureSOHO_Wireless sw;
		struct wireless_driver_ops *wlan = securesoho_get_wireless_card();

		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_CONFIGURED;
		sw.wps_mode = WIRELESS_WPS_NONE;
		wlan->wlan_get_profile(&sw);
		securesoho_wireless_set(&sw, 0);
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, "WPS 2 mins time out!")){
		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_EAP_FAILED;
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, "This WPS Registrar supports PBC")){
		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_UNKNOWN;
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, "This WPS Registrar supports PIN")){
		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_UNKNOWN;
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, "WPS status fail")){
		mylog_trace("wsc: %s", data);
		state = STATUS_WSC_EAP_FAILED;
		mylog_trace("wps status: %d", state);
		nm_dbus_signal_wps_status(state);
	}else if (strstr(data, " connects with our wireless client")){
		SecureSOHO_Wireless sw;
		char buffer[18];
		char *p;

		mylog_trace("associated: %s", data);
		if ((p = strstr(data, "STA(")) != NULL){
			p += strlen("STA(");
			memcpy(buffer, p, sizeof(buffer) - 1);
			buffer[sizeof(buffer) - 1] = '\0';
			securesoho_wireless_get(&sw);
			memcpy(sw.bssid, buffer, sizeof(sw.bssid));
			mylog_trace("Got ap's mac address: %s", buffer);
			securesoho_wireless_set(&sw, 0);
		}else{
			mylog_trace("Can not get the ap's mac address: %s", data);
		}
	}else {
		mylog_trace("Unknown wireless custom data: %s", data);
	}
}

/*------------------------------------------------------------------*/
/*
 * Process the wireless command
 */
static int
process_wireless_token(struct iw_event *	event,		/* Extracted token */
		struct iw_range *	iw_range,	/* Range info */
		int			has_range)
{
	char		buffer[128];	/* Temporary buffer */
	char		buffer2[30];	/* Temporary buffer */
	char *	prefix = (IW_IS_GET(event->cmd) ? "New" : "Set");
	struct wireless_driver_ops *wlan = securesoho_get_wireless_card();

	printf("============================================== \n");

	printf("\033[1;45m ===============  %s(), %d... event->cmd = 0x%x ================== \033[0m  \n", __FUNCTION__, __LINE__, event->cmd);

	/* Now, let's decode the event */
	switch(event->cmd)
	{
		/* ----- set events ----- */
		/* Events that result from a "SET XXX" operation by the user */
		case SIOCSIWNWID:
			if(event->u.nwid.disabled)
				mylog_trace("Set NWID:off/any");
			else
				mylog_trace("Set NWID:%X", event->u.nwid.value);
			break;
		case SIOCSIWFREQ:
		case SIOCGIWFREQ:
			{
				double		freq;			/* Frequency/channel */
				int		channel = -1;		/* Converted to channel */
				freq = iw_freq2float(&(event->u.freq));
				if(has_range)
				{
					if(freq < KILO)
						/* Convert channel to frequency if possible */
						channel = iw_channel_to_freq((int) freq, &freq, iw_range);
					else
						/* Convert frequency to channel if possible */
						channel = iw_freq_to_channel(freq, iw_range);
				}
				iw_print_freq(buffer, sizeof(buffer),
						freq, channel, event->u.freq.flags);
				mylog_trace("%s %s", prefix, buffer);
			}
			break;
		case SIOCSIWMODE:
			mylog_trace("Set Mode:%s",
					iw_operation_mode[event->u.mode]);
			break;
		case SIOCSIWESSID:
		case SIOCGIWESSID:
			{
				char essid[IW_ESSID_MAX_SIZE+1];
				memset(essid, '\0', sizeof(essid));
				if((event->u.essid.pointer) && (event->u.essid.length))
					memcpy(essid, event->u.essid.pointer, event->u.essid.length);
				if(event->u.essid.flags)
				{
					/* Does it have an ESSID index ? */
					if((event->u.essid.flags & IW_ENCODE_INDEX) > 1)
						mylog_trace("%s ESSID:\"%s\" [%d]", prefix, essid,
								(event->u.essid.flags & IW_ENCODE_INDEX));
					else
						mylog_trace("%s ESSID:\"%s\"", prefix, essid);
				}
				else
					mylog_trace("%s ESSID:off/any", prefix);
			}
			break;
		case SIOCSIWENCODE:
			{
				unsigned char	key[IW_ENCODING_TOKEN_MAX];
				if(event->u.data.pointer)
					memcpy(key, event->u.data.pointer, event->u.data.length);
				else
					event->u.data.flags |= IW_ENCODE_NOKEY;
				mylog_trace("Set Encryption key:");
				if(event->u.data.flags & IW_ENCODE_DISABLED)
					mylog_trace("off");
				else
				{
					/* Display the key */
					iw_print_key(buffer, sizeof(buffer), key, event->u.data.length,
							event->u.data.flags);
					mylog_trace("%s", buffer);

					/* Other info... */
					if((event->u.data.flags & IW_ENCODE_INDEX) > 1)
						mylog_trace(" [%d]", event->u.data.flags & IW_ENCODE_INDEX);
					if(event->u.data.flags & IW_ENCODE_RESTRICTED)
						mylog_trace("	Security mode:restricted");
					if(event->u.data.flags & IW_ENCODE_OPEN)
						mylog_trace("	Security mode:open");
				}
			}
			break;
			/* ----- driver events ----- */
			/* Events generated by the driver when something important happens */
		case SIOCGIWAP:
			mylog_trace("New Access Point/Cell address:%s",
					iw_sawap_ntop(&event->u.ap_addr, buffer));
			if (memcmp(event->u.ap_addr.sa_data, "\x00\x00\x00\x00\x00\x00", ETH_ALEN) == 0
			    ||
			    memcmp(event->u.ap_addr.sa_data, "\x44\x44\x44\x44\x44\x44", ETH_ALEN) == 0) {
				// Link down
				wlan->wlan_linkdown();
			} else {
				// Link up
				wlan->wlan_linkup();
			}
			break;
		case SIOCGIWSCAN:
			mylog_trace("Scan request completed");
			if(wlan->wlan_scan_completed) 
				wlan->wlan_scan_completed();
			break;
		case IWEVTXDROP:
			mylog_trace("Tx packet dropped:%s",
					iw_saether_ntop(&event->u.addr, buffer));
			break;
		case IWEVCUSTOM:
			{
				char custom[IW_CUSTOM_MAX+1];
				memset(custom, '\0', sizeof(custom));
				if((event->u.data.pointer) && (event->u.data.length))
					memcpy(custom, event->u.data.pointer, event->u.data.length);
				mylog_trace("Custom driver event:%s", custom);
				process_wireless_custom_token(custom, event->u.data.length);
			}
			break;
		case IWEVREGISTERED:
			mylog_trace("Registered node:%s",
					iw_saether_ntop(&event->u.addr, buffer));
			wlan->wlan_linkup();
			break;
		case IWEVEXPIRED:
			mylog_trace("Expired node:%s",
					iw_saether_ntop(&event->u.addr, buffer));
			wlan->wlan_linkdown();
			break;
		case SIOCGIWTHRSPY:
			{
				struct iw_thrspy	threshold;
				if((event->u.data.pointer) && (event->u.data.length))
				{
					memcpy(&threshold, event->u.data.pointer,
							sizeof(struct iw_thrspy));
					mylog_trace("Spy threshold crossed on address:%s",
							iw_saether_ntop(&threshold.addr, buffer));
					iw_print_stats(buffer, sizeof(buffer),
							&threshold.qual, iw_range, has_range);
					mylog_trace("				 Link %s", buffer);
				}
				else
					mylog_trace("Invalid Spy Threshold event");
			}
			break;
			/* ----- driver WPA events ----- */
			/* Events generated by the driver, used for WPA operation */
		case IWEVMICHAELMICFAILURE:
			if(event->u.data.length >= sizeof(struct iw_michaelmicfailure))
			{
				struct iw_michaelmicfailure mf;
				memcpy(&mf, event->u.data.pointer, sizeof(mf));
				mylog_trace("Michael MIC failure flags:0x%X src_addr:%s tsc:%s",
						mf.flags,
						iw_saether_ntop(&mf.src_addr, buffer2),
						iw_hexdump(buffer, sizeof(buffer),
							mf.tsc, IW_ENCODE_SEQ_MAX_SIZE));
			}
			break;
		case IWEVASSOCREQIE:
			mylog_trace("Association Request IEs:%s",
					iw_hexdump(buffer, sizeof(buffer),
						event->u.data.pointer, event->u.data.length));
			break;
		case IWEVASSOCRESPIE:
			mylog_trace("Association Response IEs:%s",
					iw_hexdump(buffer, sizeof(buffer),
						event->u.data.pointer, event->u.data.length));
			break;
		case IWEVPMKIDCAND:
			if(event->u.data.length >= sizeof(struct iw_pmkid_cand))
			{
				struct iw_pmkid_cand cand;
				memcpy(&cand, event->u.data.pointer, sizeof(cand));
				mylog_trace("PMKID candidate flags:0x%X index:%d bssid:%s",
						cand.flags, cand.index,
						iw_saether_ntop(&cand.bssid, buffer));
			}
			break;
		case SIOCGIWRATE:
			iw_print_bitrate(buffer, sizeof(buffer), event->u.bitrate.value);
			mylog_trace("New Bit Rate:%s", buffer);
			break;
		case SIOCGIWNAME:
			mylog_trace("Protocol:%-1.16s", event->u.name);
			break;
		case IWEVQUAL:
			{
				event->u.qual.updated = 0x0;	/* Not that reliable, disable */
				iw_print_stats(buffer, sizeof(buffer),
						&event->u.qual, iw_range, has_range);
				mylog_trace("Link %s", buffer);
				break;
			}
		default:
			mylog_trace("(Unknown Wireless event 0x%04X)", event->cmd);
	}

	return(0);
}

static void process_wireless(int index, struct ifinfomsg *msg, int bytes)
{
	printf("\033[1;45m ===============  %s(), %d... ================== \033[0m  \n", __FUNCTION__, __LINE__);
	struct rtattr *attr;

	if(!msg || bytes <= 0){
		mylog_error("The data is null");
		return;
	}

	for (attr = IFLA_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		struct iw_event iwe;
		char *data;
		int len, ret;
		//struct wireless_driver_ops *wlan = securesoho_get_wireless_card();
		struct stream_descr	stream;
		struct wireless_iface *	wireless_data;


		if (attr->rta_type != IFLA_WIRELESS)
			continue;

		/* Get data from cache */
		wireless_data = iw_get_interface_data(index);
		if(wireless_data == NULL)
			break;

		data = (char *)attr + RTA_ALIGN(sizeof(struct rtattr));
		len = attr->rta_len - RTA_ALIGN(sizeof(struct rtattr));

		iw_init_event_stream(&stream, data, len);

		do
		{
			/* Extract an event and print it */
			ret = iw_extract_event_stream(&stream, &iwe,
					wireless_data->range.we_version_compiled);
			if(ret != 0)
			{
				printf("\033[1;45m ===============  %s(), %d...ret = %d ================== \033[0m  \n", __FUNCTION__, __LINE__, ret);

				if(ret > 0)
					process_wireless_token(&iwe,
							&wireless_data->range, wireless_data->has_range);
				else
					mylog_trace("(Invalid event)");
			}
		}
		while(ret > 0);
	}


}

static void process_newlink(unsigned short type, int index, unsigned flags,
		unsigned change, struct ifinfomsg *msg, int bytes)
{

	printf("\033[1;45m ===============  %s(), %d... type = %d ================== \033[0m  \n", __FUNCTION__, __LINE__, type);

	switch (type) {
		case ARPHRD_ETHER:
		case ARPHRD_LOOPBACK:
		case ARPHRD_NONE:
			mylog_trace("%s:%d", __func__, __LINE__);
			break;
	}

	process_link_status(msg, bytes);
	process_wireless(index, msg, bytes);
}

static void process_dellink(unsigned short type, int index, unsigned flags,
		unsigned change, struct ifinfomsg *msg, int bytes)
{
	printf("\033[1;45m ===============  %s(), %d... type = %d ================== \033[0m  \n", __FUNCTION__, __LINE__, type);

	switch (type) {
		case ARPHRD_ETHER:
		case ARPHRD_LOOPBACK:
		case ARPHRD_NONE:
			mylog_trace("%s:%d", __func__, __LINE__);
			break;
	}

	process_link_status(msg, bytes);
	process_wireless(index, msg, bytes);
}

static void extract_addr(struct ifaddrmsg *msg, int bytes,
		const char **label,
		struct in_addr *local,
		struct in_addr *address,
		struct in_addr *broadcast)
{
	struct rtattr *attr;

	for (attr = IFA_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case IFA_ADDRESS:
				if (address != NULL)
					*address = *((struct in_addr *) RTA_DATA(attr));
				break;
			case IFA_LOCAL:
				if (local != NULL)
					*local = *((struct in_addr *) RTA_DATA(attr));
				break;
			case IFA_BROADCAST:
				if (broadcast != NULL)
					*broadcast = *((struct in_addr *) RTA_DATA(attr));
				break;
			case IFA_LABEL:
				if (label != NULL)
					*label = RTA_DATA(attr);
				break;
		}
	}
}

static void process_newaddr(unsigned char family, unsigned char prefixlen,
		int index, struct ifaddrmsg *msg, int bytes)
{
	struct in_addr address = { INADDR_ANY };
	const char *label = NULL;

	if (family != AF_INET)
		return;

	extract_addr(msg, bytes, &label, &address, NULL, NULL);

}

static void process_deladdr(unsigned char family, unsigned char prefixlen,
		int index, struct ifaddrmsg *msg, int bytes)
{
	struct in_addr address = { INADDR_ANY };
	const char *label = NULL;

	if (family != AF_INET)
		return;

	extract_addr(msg, bytes, &label, &address, NULL, NULL);
}

static void extract_route(struct rtmsg *msg, int bytes, int *index,
		struct in_addr *dst,
		struct in_addr *gateway)
{
	struct rtattr *attr;

	for (attr = RTM_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case RTA_DST:
				if (dst != NULL)
					*dst = *((struct in_addr *) RTA_DATA(attr));
				break;
			case RTA_GATEWAY:
				if (gateway != NULL)
					*gateway = *((struct in_addr *) RTA_DATA(attr));
				break;
			case RTA_OIF:
				if (index != NULL)
					*index = *((int *) RTA_DATA(attr));
				break;
		}
	}
}

static void process_newroute(unsigned char family, unsigned char scope,
		struct rtmsg *msg, int bytes)
{
	struct in_addr dst = { INADDR_ANY }, gateway = { INADDR_ANY };
	int index = -1;

	if (family != AF_INET)
		return;

	extract_route(msg, bytes, &index, &dst, &gateway);
}

static void process_delroute(unsigned char family, unsigned char scope,
		struct rtmsg *msg, int bytes)
{
	struct in_addr dst = { INADDR_ANY }, gateway = { INADDR_ANY };
	int index = -1;

	if (family != AF_INET)
		return;

	extract_route(msg, bytes, &index, &dst, &gateway);
}



static inline void print_ether(struct rtattr *attr, const char *name)
{
	int len = (int) RTA_PAYLOAD(attr);

	if (len == ETH_ALEN) {
		struct ether_addr eth;
		memcpy(&eth, RTA_DATA(attr), ETH_ALEN);
		mylog_trace("  attr %s (len %d) %s", name, len, ether_ntoa(&eth));
	} else
		mylog_trace("  attr %s (len %d)", name, len);
}

static inline void print_inet(struct rtattr *attr, const char *name,
		unsigned char family)
{
	int len = (int) RTA_PAYLOAD(attr);

	if (family == AF_INET && len == sizeof(struct in_addr)) {
		struct in_addr addr;
		addr = *((struct in_addr *) RTA_DATA(attr));
		mylog_trace("  attr %s (len %d) %s", name, len, inet_ntoa(addr));
	} else
		mylog_trace("  attr %s (len %d)", name, len);
}

static inline void print_string(struct rtattr *attr, const char *name)
{
	mylog_trace("  attr %s (len %d) %s", name, (int) RTA_PAYLOAD(attr),
			(char *) RTA_DATA(attr));
}

static inline void print_byte(struct rtattr *attr, const char *name)
{
	mylog_trace("  attr %s (len %d) 0x%02x", name, (int) RTA_PAYLOAD(attr),
			*((unsigned char *) RTA_DATA(attr)));
}

static inline void print_integer(struct rtattr *attr, const char *name)
{
	mylog_trace("  attr %s (len %d) %d", name, (int) RTA_PAYLOAD(attr),
			*((int *) RTA_DATA(attr)));
}

static inline void print_attr(struct rtattr *attr, const char *name)
{
	int len = (int) RTA_PAYLOAD(attr);

	if (name && len > 0)
		mylog_trace("  attr %s (len %d)", name, len);
	else
		mylog_trace("  attr %d (len %d)", attr->rta_type, len);
}

static void rtnl_link(struct nlmsghdr *hdr)
{
	struct ifinfomsg *msg;
	struct rtattr *attr;
	int bytes;

	msg = (struct ifinfomsg *) NLMSG_DATA(hdr);
	bytes = IFLA_PAYLOAD(hdr);

	mylog_trace("ifi_index %d ifi_flags 0x%04x", msg->ifi_index, msg->ifi_flags);
	printf("\033[1;45m ===============  %s(), %d...================== \033[0m  \n", __FUNCTION__, __LINE__);

	for (attr = IFLA_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case IFLA_ADDRESS:
				print_ether(attr, "address");
				break;
			case IFLA_BROADCAST:
				print_ether(attr, "broadcast");
				break;
			case IFLA_IFNAME:
				print_string(attr, "ifname");
				break;
			case IFLA_MTU:
				print_integer(attr, "mtu");
				break;
			case IFLA_LINK:
				print_attr(attr, "link");
				break;
			case IFLA_QDISC:
				print_attr(attr, "qdisc");
				break;
			case IFLA_STATS:
				print_attr(attr, "stats");
				break;
			case IFLA_COST:
				print_attr(attr, "cost");
				break;
			case IFLA_PRIORITY:
				print_attr(attr, "priority");
				break;
			case IFLA_MASTER:
				print_attr(attr, "master");
				break;
			case IFLA_WIRELESS:
				print_attr(attr, "wireless");
				break;
			case IFLA_PROTINFO:
				print_attr(attr, "protinfo");
				break;
			case IFLA_TXQLEN:
				print_integer(attr, "txqlen");
				break;
			case IFLA_MAP:
				print_attr(attr, "map");
				break;
			case IFLA_WEIGHT:
				print_attr(attr, "weight");
				break;
			case IFLA_OPERSTATE:
				print_byte(attr, "operstate");
				break;
			case IFLA_LINKMODE:
				print_byte(attr, "linkmode");
				break;
			default:
				print_attr(attr, NULL);
				break;
		}
	}
}

static void rtnl_newlink(struct nlmsghdr *hdr)
{
	printf("\033[1;45m ===============  %s(), %d...================== \033[0m  \n", __FUNCTION__, __LINE__);
	struct ifinfomsg *msg = (struct ifinfomsg *) NLMSG_DATA(hdr);

	rtnl_link(hdr);

	process_newlink(msg->ifi_type, msg->ifi_index, msg->ifi_flags,
			msg->ifi_change, msg, IFA_PAYLOAD(hdr));
}

static void rtnl_dellink(struct nlmsghdr *hdr)
{
	struct ifinfomsg *msg = (struct ifinfomsg *) NLMSG_DATA(hdr);

	rtnl_link(hdr);

	process_dellink(msg->ifi_type, msg->ifi_index, msg->ifi_flags,
			msg->ifi_change, msg, IFA_PAYLOAD(hdr));
}

static void rtnl_addr(struct nlmsghdr *hdr)
{
	struct ifaddrmsg *msg;
	struct rtattr *attr;
	int bytes;

	msg = (struct ifaddrmsg *) NLMSG_DATA(hdr);
	bytes = IFA_PAYLOAD(hdr);

	mylog_trace("ifa_family %d ifa_index %d", msg->ifa_family, msg->ifa_index);

	for (attr = IFA_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case IFA_ADDRESS:
				print_inet(attr, "address", msg->ifa_family);
				break;
			case IFA_LOCAL:
				print_inet(attr, "local", msg->ifa_family);
				break;
			case IFA_LABEL:
				print_string(attr, "label");
				break;
			case IFA_BROADCAST:
				print_inet(attr, "broadcast", msg->ifa_family);
				break;
			case IFA_ANYCAST:
				print_attr(attr, "anycast");
				break;
			case IFA_CACHEINFO:
				print_attr(attr, "cacheinfo");
				break;
			case IFA_MULTICAST:
				print_attr(attr, "multicast");
				break;
			default:
				print_attr(attr, NULL);
				break;
		}
	}
}

static void rtnl_newaddr(struct nlmsghdr *hdr)
{
	struct ifaddrmsg *msg = (struct ifaddrmsg *) NLMSG_DATA(hdr);

	rtnl_addr(hdr);

	process_newaddr(msg->ifa_family, msg->ifa_prefixlen, msg->ifa_index,
			msg, IFA_PAYLOAD(hdr));
}

static void rtnl_deladdr(struct nlmsghdr *hdr)
{
	struct ifaddrmsg *msg = (struct ifaddrmsg *) NLMSG_DATA(hdr);

	rtnl_addr(hdr);

	process_deladdr(msg->ifa_family, msg->ifa_prefixlen, msg->ifa_index,
			msg, IFA_PAYLOAD(hdr));
}

static void rtnl_route(struct nlmsghdr *hdr)
{
	struct rtmsg *msg;
	struct rtattr *attr;
	int bytes;

	msg = (struct rtmsg *) NLMSG_DATA(hdr);
	bytes = RTM_PAYLOAD(hdr);

	mylog_trace("rtm_family %d rtm_table %d rtm_protocol %d",
			msg->rtm_family, msg->rtm_table, msg->rtm_protocol);
	mylog_trace("rtm_scope %d rtm_type %d rtm_flags 0x%04x",
			msg->rtm_scope, msg->rtm_type, msg->rtm_flags);

	for (attr = RTM_RTA(msg); RTA_OK(attr, bytes);
			attr = RTA_NEXT(attr, bytes)) {
		switch (attr->rta_type) {
			case RTA_DST:
				print_inet(attr, "dst", msg->rtm_family);
				break;
			case RTA_SRC:
				print_inet(attr, "src", msg->rtm_family);
				break;
			case RTA_IIF:
				print_string(attr, "iif");
				break;
			case RTA_OIF:
				print_integer(attr, "oif");
				break;
			case RTA_GATEWAY:
				print_inet(attr, "gateway", msg->rtm_family);
				break;
			case RTA_PRIORITY:
				print_attr(attr, "priority");
				break;
			case RTA_PREFSRC:
				print_inet(attr, "prefsrc", msg->rtm_family);
				break;
			case RTA_METRICS:
				print_attr(attr, "metrics");
				break;
			case RTA_TABLE:
				print_integer(attr, "table");
				break;
			default:
				print_attr(attr, NULL);
				break;
		}
	}
}

static void rtnl_newroute(struct nlmsghdr *hdr)
{
	struct rtmsg *msg = (struct rtmsg *) NLMSG_DATA(hdr);

	rtnl_route(hdr);

	if (msg->rtm_table == RT_TABLE_MAIN &&
			msg->rtm_protocol == RTPROT_BOOT &&
			msg->rtm_type == RTN_UNICAST)
		process_newroute(msg->rtm_family, msg->rtm_scope,
				msg, RTM_PAYLOAD(hdr));
}

static void rtnl_delroute(struct nlmsghdr *hdr)
{
	struct rtmsg *msg = (struct rtmsg *) NLMSG_DATA(hdr);

	rtnl_route(hdr);

	if (msg->rtm_table == RT_TABLE_MAIN &&
			msg->rtm_protocol == RTPROT_BOOT &&
			msg->rtm_type == RTN_UNICAST)
		process_delroute(msg->rtm_family, msg->rtm_scope,
				msg, RTM_PAYLOAD(hdr));
}

static const char *type2string(uint16_t type)
{
	switch (type) {
		case NLMSG_NOOP:
			return "NOOP";
		case NLMSG_ERROR:
			return "ERROR";
		case NLMSG_DONE:
			return "DONE";
		case NLMSG_OVERRUN:
			return "OVERRUN";
		case RTM_GETLINK:
			return "GETLINK";
		case RTM_NEWLINK:
			return "NEWLINK";
		case RTM_DELLINK:
			return "DELLINK";
		case RTM_NEWADDR:
			return "NEWADDR";
		case RTM_DELADDR:
			return "DELADDR";
		case RTM_GETROUTE:
			return "GETROUTE";
		case RTM_NEWROUTE:
			return "NEWROUTE";
		case RTM_DELROUTE:
			return "DELROUTE";
		default:
			return "UNKNOWN";
	}
}

static void rtnl_message(void *buf, size_t len)
{
	mylog_debug("buf %p len %zd", buf, len);

	while (len > 0) {
		struct nlmsghdr *hdr = buf;
		struct nlmsgerr *err;

		if (!NLMSG_OK(hdr, len))
			break;

		mylog_debug("%s len %d type %d flags 0x%04x seq %d",
				type2string(hdr->nlmsg_type),
				hdr->nlmsg_len, hdr->nlmsg_type,
				hdr->nlmsg_flags, hdr->nlmsg_seq);
		printf("=================%s len %d type %d flags 0x%04x seq %d",
				type2string(hdr->nlmsg_type),
				hdr->nlmsg_len, hdr->nlmsg_type,
				hdr->nlmsg_flags, hdr->nlmsg_seq);

		switch (hdr->nlmsg_type) {
			case NLMSG_NOOP:
			case NLMSG_OVERRUN:
			case NLMSG_DONE:
				mylog_debug("%s:%d", __func__, __LINE__);
				return;
			case NLMSG_ERROR:
				err = NLMSG_DATA(hdr);
				mylog_debug("error %d (%s)", -err->error,
						strerror(-err->error));
				return;
			case RTM_NEWLINK:
				rtnl_newlink(hdr);
				break;
			case RTM_DELLINK:
				rtnl_dellink(hdr);
				break;
			case RTM_NEWADDR:
				rtnl_newaddr(hdr);
				break;
			case RTM_DELADDR:
				rtnl_deladdr(hdr);
				break;
			case RTM_NEWROUTE:
				rtnl_newroute(hdr);
				break;
			case RTM_DELROUTE:
				rtnl_delroute(hdr);
				break;
		}

		len -= hdr->nlmsg_len;
		buf += hdr->nlmsg_len;
	}
}

int nm_rtnl_handle_init(struct nm_rtnl_handle *rth)
{
	int sk;
	struct sockaddr_nl addr;

	mylog_debug("enter %s", __func__);

	if (!rth){
		mylog_error("Failed to get the rtnl handle");
		return -1;
	}

	sk = socket(PF_NETLINK, SOCK_DGRAM, NETLINK_ROUTE);
	if (sk < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.nl_family = AF_NETLINK;
	addr.nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV4_ROUTE;

	if (bind(sk, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		close(sk);
		return -1;
	}

	rth->sk = sk;
	return 0;
}

int nm_rtnl_handle_fini(struct nm_rtnl_handle *rth)
{
	if (!rth){
		mylog_error("The rth handle is null");
		return -1;
	}

	if (rth->sk < 0){
		mylog_error("the socket is null");
		return -1;
	}

	close(rth->sk);
	rth->sk = -1;
	return 0;
}

int nm_rtnl_handle_get_fd(struct nm_rtnl_handle *rth)
{
	if (!rth){
		mylog_error("The rth handle is null");
		return -1;
	}

	return rth->sk;
}

int nm_rtnl_handle_events(struct nm_rtnl_handle *rth)
{
	printf("\033[1;45m ===============  %s(), %d... ================== \033[0m  \n", __FUNCTION__, __LINE__);
	struct sockaddr_nl sanl;
	socklen_t sanllen = sizeof(struct sockaddr_nl);

	int len;
	char buf[8192];

	if (!rth){
		mylog_error("the rtnl handle is null");
		return -1;
	}
	if (rth->sk < 0){
		mylog_error("the socket is null");
		return -1;
	}

	len = recvfrom(rth->sk, buf, sizeof(buf), MSG_DONTWAIT, (struct sockaddr*)&sanl, &sanllen);
	if(len < 0)
	{
		if(errno != EINTR && errno != EAGAIN)
		{
			mylog_error("%s: error reading netlink: %s.",
					__func__, strerror(errno));
		}
		return -1;
	}

	if(len == 0)
	{
		mylog_error("%s: EOF on netlink??", __func__);
		return -1;
	}
	printf("\033[1;45m ===============  %s(), %d... ================== \033[0m  \n", __FUNCTION__, __LINE__);

	rtnl_message(buf, len);
	return 0;
}

