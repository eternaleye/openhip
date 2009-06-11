/*
 * Host Identity Protocol
 * Copyright (C) 2002-06 the Boeing Company
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 *  hip_netlink.c
 *
 *  Authors:	Jeff Ahrenholz, <jeffrey.m.ahrenholz@boeing.com>
 *              Tom Henderson <thomas.r.henderson@boeing.com>
 *
 * Functions that use the Netlink socket interface.
 *
 */
#ifndef __MACOSX__
/*
 * XXX this needs cleaned up and split into platform specific parts
 * equivalent Mac file is in ../mac/hip_mac.c 
 */

#ifndef __WIN32__
#define USE_LINUX_NETLINK
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __WIN32__
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <win32/types.h>
#else
#ifndef __MACOSX__
#include <asm/types.h>
#endif
#include <unistd.h>
#include <sys/time.h>
#include <netinet/in.h>		/* INADDR_NONE                  */
#include <netinet/ip.h>		/* INADDR_NONE                  */
#include <sys/uio.h>		/* iovec			*/
#include <pthread.h>		/* pthreads support		*/
#endif
#include <ctype.h>
#include <openssl/sha.h>
#include <openssl/dsa.h>
#include <openssl/asn1.h>	
#include <openssl/rand.h>
#include <sys/types.h>
#include <errno.h>
#include <fcntl.h>		/* open()			*/
#include <libxml/tree.h> /* all XML stuff		*/

#ifdef USE_LINUX_NETLINK
#include <netinet/ip6.h>
#include <linux/netlink.h>	/* get_my_addresses() support	*/
#include <linux/rtnetlink.h>	/* get_my_addresses() support	*/
#include <linux/if.h>		/* set_link_params() support	*/
#include <sys/ioctl.h>		/* set_link_params() support	*/
#else
#include <windows.h>		/* Windows registry access */
#define REG_INTERFACES_KEY "SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces"
#include <win32/rtnetlink.h>
#endif

#include <hip/hip_proto.h>
#include <hip/hip_types.h>
#include <hip/hip_globals.h>
#include <hip/hip_funcs.h>
#include <hip/hip_stun.h>

/* instead of including the entire netlink library here, 
 * we just implement partial functionality
 */

/* Local definitions */
int nl_sequence_number = 0;

/* Local functions */
int read_netlink_response();
void handle_local_address_change(int add,struct sockaddr *newaddr,int if_index);
void readdress_association(hip_assoc *hip_a, struct sockaddr *newaddr,
    int if_index);
void association_add_address(hip_assoc *hip_a, struct sockaddr *newaddr,
    int if_index);
void association_del_address(hip_assoc *hip_a, struct sockaddr *newaddr,
    int if_index);
void make_address_active(sockaddr_list *item);
int set_preferred_address_in_list(struct sockaddr *addr);

extern int send_udp_esp_tunnel_activation (__u32 spi_out);

/*
 * function hip_netlink_open()
 *
 * Opens and binds a Netlink socket, setting s_net.
 *
 * Returns 0 on success, -1 otherwise.
 */
int hip_netlink_open()
{
#ifdef USE_LINUX_NETLINK 
	struct sockaddr_nl local;
	
	if (s_net)
		close(s_net);
	if ((s_net = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) < 0)
		return(-1);

	memset(&local, 0, sizeof(local));
	local.nl_family = AF_NETLINK;
	/* subscribe to link, IPv4/IPv6 address notifications */
	local.nl_groups = (RTMGRP_LINK| RTMGRP_IPV4_IFADDR| RTMGRP_IPV6_IFADDR);
	
	if (bind(s_net, (struct sockaddr *)&local, sizeof(local)) < 0)
		return(-1);
	
	nl_sequence_number = time(NULL);
#else
	s_net = netlsp[1];
#endif
	return(0);
}

/* 
 * function get_my_addresses()
 *
 * Use the Netlink interface to retrieve a list of addresses for this
 * host's interfaces, and stores them into global my_addr_head list.
 */
int get_my_addresses()
{
	/* these are used for passing messages */
	struct sockaddr_storage ss_addr;
	struct sockaddr *addr = (struct sockaddr*) &ss_addr;
	struct sockaddr_nl nladdr;
	char buf[8192];
	struct nlmsghdr *h;
	int status;
	char done;
	sockaddr_list *l, *la;
	hi_node *hi;

	/* netlink packet */
	struct {
		struct nlmsghdr	n;
		struct rtgenmsg g;
	} req;

#ifdef USE_LINUX_NETLINK 
	struct iovec iov = { buf, sizeof(buf) };
	/* message response */
	struct msghdr msg = {
		(void*)&nladdr, sizeof(nladdr),
		&iov, 1,
		NULL, 0,
		0
	};
#endif

#if 0
	/* XXX enable this if we're called outside of init
	 * but will elminate address coming from my_host_identities!
	 */
	/* free global address list if it exists */
	sockaddr_list *temp;
	while (my_addr_head) {
		temp = my_addr_head;
		my_addr_head = my_addr_head->next;
		free(my_addr_head);
	}
#endif

	/* setup request */
	memset(&req, 0, sizeof(req));
	req.n.nlmsg_len = sizeof(req);
	req.n.nlmsg_type = RTM_GETADDR;
	req.n.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.n.nlmsg_pid = 0;
	req.n.nlmsg_seq = ++nl_sequence_number;
	req.g.rtgen_family = 0;//AF_PACKET;//AF_UNSPEC;

	/* send request */
	memset(&nladdr, 0, sizeof(nladdr));
#ifndef USE_LINUX_NETLINK
	if (send(s_net, (void*)&req, sizeof(req), 0) < 0) {
		log_(WARN, "Netlink: send() error: %s\n", strerror(errno));
		return(-1);
	}
#else
	nladdr.nl_family = AF_NETLINK;
	if (sendto(s_net, (void*)&req, sizeof(req), 0, 
		(struct sockaddr*)&nladdr,sizeof(nladdr)) < 0) {
		log_(WARN, "Netlink: sentdo() error: %s\n", strerror(errno));
		return(-1);
	}
#endif

	log_(NORM, "Local addresses: ");
	
	/* receiving loop 1
	 * call recvmsg() repeatedly until we get a message
	 * with the NLMSG_DONE flag set
	 */
	done = FALSE;
	while(!done) {
		/* get response */
#ifndef USE_LINUX_NETLINK
		/* note that this will block forever if no response */
#ifdef __WIN32__
		if ((status = recv(s_net, buf, sizeof(buf), 0))< 0) {
#else
		if ((status = read(s_net, buf, sizeof(buf)))< 0) {
#endif /* __WIN32__ */
#else
		if ((status = recvmsg(s_net, &msg, 0)) < 0) {
#endif /* USE_LINUX_NETLINK */
			log_(WARN, "Netlink: recvmsg() error!\nerror: %s\n",
			    strerror(errno));
			return(-1);
		}

		/* parse response - loop 2
		 * walk list of NL messages returned by this recvmsg()
		 */
		h = (struct nlmsghdr*) buf;
		while (NLMSG_OK(h, (__u32)status)) {
			int len;
			struct ifaddrmsg *ifa;
			struct rtattr *rta, *tb[IFA_MAX+1];

			memset(tb, 0, sizeof(tb));
			/* exit this loop on end or error
			 */
			if (h->nlmsg_type == NLMSG_DONE) {
				done = TRUE;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				log_(WARN, "Error in Netlink response.\n");
				break;
			}
			ifa = NLMSG_DATA(h);
			rta = IFA_RTA(ifa);
			len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));

			if ((ifa->ifa_family != AF_INET) &&
			    (ifa->ifa_family != AF_INET6))
				continue;

			/* parse list of attributes into table
			 * (same as parse_rtattr()) */
			while (RTA_OK(rta, len)) {
				if (rta->rta_type <= IFA_MAX)
					tb[rta->rta_type] = rta;
				rta = RTA_NEXT(rta,len);
			}
			/* fix tb entry for inet6 */
			if (!tb[IFA_LOCAL]) 
				tb[IFA_LOCAL] = tb[IFA_ADDRESS];
			if (!tb[IFA_ADDRESS])
				tb[IFA_ADDRESS] = tb[IFA_LOCAL];

			/* save the addresses we care about */
			if (tb[IFA_LOCAL]) {
				addr->sa_family = ifa->ifa_family;
				memcpy(SA2IP(addr), RTA_DATA(tb[IFA_LOCAL]),
				    RTA_PAYLOAD(tb[IFA_LOCAL]));
				la = add_address_to_list(&my_addr_head, addr,
				    ifa->ifa_index);
				la->status = ACTIVE;
				log_(NORM, "(%d)%s ", ifa->ifa_index,
				    logaddr(addr));
			}
			h = NLMSG_NEXT(h, status);
		} /* end while(NLMSG_OK) - loop 2 */
	} /* end while(!done) - loop 1 */ 
	
	/* in Windows, we have no mechanism yet for reading IPv6 addrs
	 * for now, get local IPv6 addresses from my_host_identities file 
	 */
	for (hi = my_hi_head; hi; hi=hi->next) {
		for (l = &hi->addrs; l; l=l->next) {
			if (l->addr.ss_family == AF_INET6) {
				la = add_address_to_list(&my_addr_head, 
						(struct sockaddr*)&l->addr, 0);
				la->status = ACTIVE;
				log_(NORM, "(0)%s ",
				    logaddr((struct sockaddr*)&l->addr));
			}
		}
	}
	log_(NORM, "\n");

	return(0);
}

/*
 * function select_preferred_address()
 *
 * Choose one of this machine's IP addresses as preferred.
 * - any user preference should take priority, i.e. which interface to use
 * - first select an active address having a default gateway
 *
 */
int select_preferred_address()
{
	int preferred_selected, preferred_iface_index;
	sockaddr_list *l;
	__u32 ip;
#ifdef SMA_CRAWLER
       int ifindex;
#endif
#ifndef USE_LINUX_NETLINK
	/* This is the Windows version that reads from the Windows registry */
	int i, mark_preferred;
	struct sockaddr_storage ss_addr;
	struct sockaddr *addr = (struct sockaddr*) &ss_addr;
	DWORD value, type;
	HKEY key, key_if;
	char devid[255], path[512];
	long len;
	CHAR szAddr[64];

	if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, REG_INTERFACES_KEY, 0,
			 KEY_READ, &key)) {
		log_(WARN, "Unable to read interfaces from registry\n");
		return(-1);
	}

	preferred_selected = FALSE;

	for(i=0; ; i++) { /* for each device */
		len = sizeof(devid);
		if (RegEnumKeyEx(key, i, devid, &len, 0, 0, 0, NULL)) {
			/* RegCloseKey(key); key is closed later */
			break;
		}
		sprintf(path, "%s\\%s", REG_INTERFACES_KEY, devid);
		if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, path, 0, 
				 KEY_READ, &key_if))
			continue;

		value = 0;
		len = sizeof(value);
		if (RegQueryValueEx(key_if, "EnableDHCP", 0, &type, 
				    (LPBYTE)&value, &len)) {
			RegCloseKey(key_if);
			continue;
		}
		
		if (value == 1)
			sprintf(path, "DhcpIpAddress");
		else
			sprintf(path, "IpAddress");

		len = sizeof(szAddr);
		if (RegQueryValueEx(key_if, path, 0, &type, 
			    (LPBYTE)&szAddr, &len)) {
			RegCloseKey(key_if);
			continue;
		}
		if (strncmp(szAddr, "0.0.0.0", 7)==0) {
			RegCloseKey(key_if);
			continue;
		}
		if (strncmp(szAddr, "169.254", 7)==0) {
			RegCloseKey(key_if);
			continue;
		}
		memset(addr, 0, sizeof(struct sockaddr_storage));
		addr->sa_family = AF_INET; 
		str_to_addr(szAddr, addr);
		/* check for preferred address from conf file */
		if (HCNF.preferred.ss_family &&
		    (addr->sa_family == HCNF.preferred.ss_family) &&
		    (memcmp(SA2IP(addr), SA2IP(&HCNF.preferred), 
			    SAIPLEN(addr))==0)) {
			preferred_selected=set_preferred_address_in_list(addr);
			if (preferred_selected)
				break;
		}
		/* this eliminates addresses from disconnected interfaces */
		if (!is_my_address(addr))
			continue;
		/* formerly was used to populate my_addr_head: */
		/* add_address_to_list(&my_addr_head, addr, i+1); */
		if (value==1)
			sprintf(path, "DhcpDefaultGateway");
		else
			sprintf(path, "DefaultGateway");
		len = sizeof(szAddr);
		mark_preferred = 0;
		/* check for preferred from conf file */
		if (HCNF.preferred.ss_family && 
		    (addr->sa_family == HCNF.preferred.ss_family) &&
		    (memcmp(SA2IP(addr), SA2IP(&HCNF.preferred), 
			    SAIPLEN(addr))==0)) {
			mark_preferred = 1;

		/* set preferred=TRUE if default gateway is present */
		} else if (!RegQueryValueEx(key_if, path, 0, &type,
			   (LPBYTE)&szAddr, &len) && (strlen(szAddr) > 0)) {
			mark_preferred = 1;
		}
		RegCloseKey(key_if);
		if (!mark_preferred)
			continue;
		preferred_selected = set_preferred_address_in_list(addr);
		if (preferred_selected)
			break;
	} /* for (i...) */
	RegCloseKey(key);
#else /* USE_LINUX_NETLINK */
	/* Linux version */
	/* XXX TODO: dump routing table and choose addr w/default route. */
#ifdef SMA_CRAWLER
       log_(NORM,"crawler master interface = %s\n",HCNF.master_interface);
       ifindex = if_nametoindex(HCNF.master_interface);
#endif
	preferred_selected = FALSE;
#endif /* USE_LINUX_NELINK */
	preferred_iface_index = -1;
	if (HCNF.preferred_iface)
		preferred_iface_index = devname_to_index(HCNF.preferred_iface,
							 NULL);

	/* first check for preferred from conf file */
	if ((HCNF.preferred.ss_family) || (preferred_iface_index != -1)) {
		for (l = my_addr_head; l; l=l->next) {
		    /* preferred address takes priority */
		    if ((l->addr.ss_family==HCNF.preferred.ss_family) &&
			(memcmp(SA2IP(&l->addr), SA2IP(&HCNF.preferred), 
				SAIPLEN(&l->addr))==0)) {
			l->preferred = TRUE;
			log_(NORM, "%s selected as the", logaddr(SA(&l->addr)));
			log_(NORM, " preferred address (conf).\n");
			preferred_selected = TRUE;
			break;
		    /* preferred interface next priority */
		    } else if ((preferred_iface_index > 0) && 
				(preferred_iface_index == l->if_index)) {
			if (l->addr.ss_family != AF_INET)
				continue;
			ip = ((struct sockaddr_in*)&l->addr)->sin_addr.s_addr;
			if ((ntohl(ip)==INADDR_LOOPBACK) || (IS_LSI32(ip)))
				continue;
			l->preferred = TRUE;
			log_(NORM, "%s selected as the", logaddr(SA(&l->addr)));
			log_(NORM, " preferred address (conf iface).\n");
			preferred_selected = TRUE;
			break;
		    }
		}
	}
	/* when a preferred address has not been found yet, choose
	 * the first IPv4 address that is not a loopback address
	 */
	if (!preferred_selected) {
		for (l = my_addr_head; l; l=l->next) {
			/* apply few criteria and pick first address */
			if (l->addr.ss_family != AF_INET)
				continue;
#ifdef SMA_CRAWLER
                       if(l->if_index != ifindex) // not on master interface
                               continue;
#endif
			ip = ((struct sockaddr_in*)&l->addr)->sin_addr.s_addr;
			if ((ntohl(ip)==INADDR_LOOPBACK) || (IS_LSI32(ip)) ||
			    ((ip & 0xFFFF) == 0xFEA9) ) /* autoconf addr */
				continue;
			l->preferred = TRUE;
			log_(NORM, "%s selected as the ",logaddr(SA(&l->addr)));
			log_(NORM, "preferred address (first in list).\n");
			break;
		}
	}

	/* If mobile router, set the outbound interface index
	 */
	if (OPT.mr) {
		external_if_index = -1;
		if (HCNF.outbound_iface)
			external_if_index = devname_to_index(HCNF.outbound_iface, NULL);
		if (external_if_index == -1) {
			if (preferred_iface_index != -1) {
				external_if_index = preferred_iface_index;
				log_(NORM, "Selected the preferred interface"
					" as outbound interface\n");
			} else {
				log_(ERR, "HIP started as mobile router but "
					"unable to set outbound interface index\n");
			}
		} else {
			log_(NORM, "Selected %s as outbound interface\n",
				HCNF.outbound_iface);
		}
	}
	return(0);
}

int set_preferred_address_in_list(struct sockaddr *addr) 
{
	sockaddr_list *l;
	__u32 ip;

	for (l = my_addr_head; l; l=l->next) {
		if (addr->sa_family != l->addr.ss_family)
			continue;
	        ip = ((struct sockaddr_in*) &l->addr)->sin_addr.s_addr;
		if ((addr->sa_family == AF_INET) &&
		    ( (htonl(ip)==INADDR_LOOPBACK) || (IS_LSI32(ip))))
			continue;
		if (memcmp(SA2IP(&l->addr), SA2IP(addr), SAIPLEN(addr))==0) {
			l->preferred = TRUE;
			log_(NORM, "%s selected as the preferred address.\n",
				logaddr(addr));
			return(TRUE);
		}
	} /* for (l...) */

	return(FALSE);
}


/*
 * function add_address_to_iface()
 *
 * in:		addr = address
 * 		plen = prefix length, the length of the address mask
 *		dev = name of the device
 * Add an address to an interface.
 */
int add_address_to_iface(struct sockaddr *addr, int plen, int if_index)
{
	int err=0;
#ifdef USE_LINUX_NETLINK
	struct sockaddr_nl nladdr;
	struct sockaddr_storage ss_addr;
	struct sockaddr *brd_addr = SA(&ss_addr);
	int len;
	__u32 brd_ip, mask;
	struct rtattr *rta;

	/* netlink packet */
	struct {
		struct nlmsghdr	n;
		struct ifaddrmsg a;
		char buf[512];
	} req;

	/* setup request */
	len = sizeof(req);
	memset(&req, 0, len);
	req.n.nlmsg_len = len;
	req.n.nlmsg_type = RTM_NEWADDR;
	req.n.nlmsg_flags = NLM_F_REQUEST;//NLM_F_ROOT|NLM_F_MATCH
	req.n.nlmsg_pid = 0;
	req.n.nlmsg_seq = ++nl_sequence_number;
	req.a.ifa_family = addr->sa_family;
	req.a.ifa_prefixlen = plen;
	req.a.ifa_flags = 0;
	req.a.ifa_scope = (addr->sa_family == AF_INET) ? RT_SCOPE_HOST : 
							 RT_SCOPE_UNIVERSE;
	req.a.ifa_index = if_index;

	rta = IFA_RTA(&req.a);
	rta->rta_len =  RTA_LENGTH(SAIPLEN(addr));
	rta->rta_type = IFA_ADDRESS;
	memcpy(RTA_DATA(rta), SA2IP(addr), SAIPLEN(addr));

	rta = RTA_NEXT(rta, len);
	rta->rta_len =  RTA_LENGTH(SAIPLEN(addr));
	rta->rta_type = IFA_LOCAL;
	memcpy(RTA_DATA(rta), SA2IP(addr), SAIPLEN(addr));

	log_(NORM, "Adding address %s to interface %d.\n",
		logaddr(addr), if_index);
	/* add broadcast address only for IPv4 */
	if (addr->sa_family == AF_INET) {
		/* create the broadcast address */
		brd_addr->sa_family = AF_INET;
		brd_ip =ntohl(((struct sockaddr_in*)addr)->sin_addr.s_addr);
		mask = 0xFFFFFFFFL;
		mask = mask >> plen;
		brd_ip |= mask;
		((struct sockaddr_in*)brd_addr)->sin_addr.s_addr =htonl(brd_ip);

		/* add it to the message */
		rta = RTA_NEXT(rta, len);
		rta->rta_len =  RTA_LENGTH(SAIPLEN(addr));
		rta->rta_type = IFA_BROADCAST;
		memcpy(RTA_DATA(rta), SA2IP(brd_addr), SAIPLEN(brd_addr));
	}

	/* send request */
	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	if (sendto(s_net, (void*)&req, sizeof(req), 0, 
		(struct sockaddr*)&nladdr,sizeof(nladdr)) < 0) {
		log_(WARN, "Netlink: sentdo() error: %s\n", strerror(errno));
		return(-1);
	}

	err = read_netlink_response();
#endif
	return(err);
}

/*
 * function set_link_params()
 *
 * Uses ioctl(), not rtnetlink, just like ip command.
 * equivalent of:
 * 	"/sbin/ip link set hip0 mtu 1400"
 * 	"/sbin/ip link set hip0 up"
 * (see iproute2 source file ip/iplink.c)
 */
int set_link_params(char *dev, int mtu)
{
	int err=0;
#ifdef USE_LINUX_NETLINK
	int fd;
	struct ifreq ifr;
	__u32 flags, mask;

	if ((fd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
		log_(WARN, "set_link_up(): socket error: %s\n",
			strerror(errno));
		return(-1);
	}

	/* set link MTU */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);
	ifr.ifr_mtu = mtu;
	
	err = ioctl(fd, SIOCSIFMTU, &ifr);
	if (err) {
		log_(WARN, "set_link_params(): SIOCSIFMTU error: %s\n",
			strerror(errno));
		/* non-fatal error */
	}
	
	/* set link to UP */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, dev, IFNAMSIZ);

	err = ioctl(fd, SIOCGIFFLAGS, &ifr); /* get flags */
	if (err) {
		log_(WARN, "set_link_up(): SIOCGIFFLAGS error: %s\n",
			strerror(errno));
		close(fd);
		return(-1);
	}

	flags = mask = IFF_UP;
	if ((ifr.ifr_flags^flags)&mask) { /* modify flags */
		ifr.ifr_flags &= ~mask;
		ifr.ifr_flags |= mask&flags;
		err = ioctl(fd, SIOCSIFFLAGS, &ifr);
		if (err)
			log_(WARN, "set_link_up(): SIOCSIFFLAGS error: %s\n",
				strerror(errno));
	}
	
	close(fd);
#endif
	return(err);
}

/*
 * function devname_to_index()
 *
 * Convert device name to numeric index, and also return the MAC address.
 * Similar to librtnetlink ll_init_map() and ll_name_to_index(), but
 * no map is retained, no caching is performed (meant to be called only once).
 */
int devname_to_index(char *dev, __u64 *mac)
{
#ifdef USE_LINUX_NETLINK
	struct {
		struct nlmsghdr n;
		struct rtgenmsg g;
	} req;
	struct sockaddr_nl nladdr;
	char buf[8192];
	struct nlmsghdr *h;
	int status;
	char done;

	struct iovec iov = { buf, sizeof(buf) };
	/* message response */
	struct msghdr msg = {
		(void*)&nladdr, sizeof(nladdr),
		&iov, 1,
		NULL, 0,
		0
	};
	/* send a link dump message */
	req.n.nlmsg_len = sizeof(req);
	req.n.nlmsg_type = RTM_GETLINK;
	req.n.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.n.nlmsg_pid = 0;
	req.n.nlmsg_seq = ++nl_sequence_number;
	req.g.rtgen_family = AF_UNSPEC;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;
	if (sendto(s_net, (void*)&req, sizeof(req), 0, 
		(struct sockaddr*)&nladdr,sizeof(nladdr)) < 0) {
		log_(WARN, "Netlink: sentdo() error: %s\n", strerror(errno));
		return(-1);
	}
	
	/* receiving loop 1
	 * call recvmsg() repeatedly until we get a message
	 * with the NLMSG_DONE flag set
	 */
	done = FALSE;
	while(!done) {
		if ((status = recvmsg(s_net, &msg, 0)) < 0) {
			log_(WARN, "Netlink: recvmsg() error!\nerror: %s\n",
			    strerror(errno));
			return(-1);
		}

		/* parse response - loop 2
		 * walk list of NL messages returned by this recvmsg()
		 */
		h = (struct nlmsghdr*) buf;
		while (NLMSG_OK(h, status)) {
			int len;
			struct ifinfomsg *ifi;
			struct rtattr *rta, *tb[IFLA_MAX+1];

			memset(tb, 0, sizeof(tb));
			/* exit this loop on end or error
			 */
			if (h->nlmsg_type == NLMSG_DONE) {
				done = TRUE;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				log_(WARN, "Error in Netlink response.\n");
				break;
			}
			ifi = NLMSG_DATA(h);
			rta = IFLA_RTA(ifi);
			len = h->nlmsg_len - NLMSG_LENGTH(sizeof(*ifi));

			/* parse list of attributes into table
			 * (same as parse_rtattr()) */
			while (RTA_OK(rta, len)) {
				if (rta->rta_type <= IFLA_MAX)
					tb[rta->rta_type] = rta;
				rta = RTA_NEXT(rta,len);
			}
			/* Retrieve interface name and MAC address
			 * for the specified dev. */
			if (RTA_DATA(tb[IFLA_IFNAME]) && tb[IFLA_ADDRESS] &&
			    (strcmp(dev, RTA_DATA(tb[IFLA_IFNAME])) == 0)) {
				len = RTA_PAYLOAD(tb[IFLA_ADDRESS]);
				if (len > 8) len = 8;
				if (mac)
					memcpy(mac, RTA_DATA(tb[IFLA_ADDRESS]),
						len);
				return(ifi->ifi_index);
			}
			h = NLMSG_NEXT(h, status);
		} /* end while(NLMSG_OK) - loop 2 */
	} /* end while(!done) - loop 1 */ 

	/* no match on dev name */
#endif
	return(-1);	
}

/*
 * function read_netlink_response()
 *
 * Called to parse the netlink response without checking for anything
 * but errors.
 */
int read_netlink_response()
{
	int err=0;
#ifdef USE_LINUX_NETLINK
	struct sockaddr_nl nladdr;
	struct nlmsghdr *h;
	int status;
	char done;
	char buf[8192];
	struct iovec iov = { buf, sizeof(buf) };
	/* message response */
	struct msghdr msg = {
		(void*)&nladdr, sizeof(nladdr),
		&iov, 1,
		NULL, 0,
		0
	};
	/* receiving loop 1
	 * call recvmsg() repeatedly until we get a message
	 * with the NLMSG_DONE flag set
	 */
	done = FALSE;
	while (!done) {
		if ((status = recvmsg(s_net, &msg, 0)) < 0) {
			log_(WARN, "Netlink: recvmsg() error!\nerror: %s\n",
			    strerror(errno));
			return(-1);
		}

		/* parse response - loop 2
		 * walk list of NL messages returned by this recvmsg()
		 */
		h = (struct nlmsghdr*) buf;
		while (NLMSG_OK(h, status)) {
			/* exit this loop on end or error
			 */
			if ((h->nlmsg_flags & NLM_F_MULTI) &&
			    h->nlmsg_type == NLMSG_DONE) {
				done = TRUE;
				break;
			}
			if (!(h->nlmsg_flags & NLM_F_MULTI) &&
			    h->nlmsg_type == RTM_NEWADDR) { /* add types here */
				done = TRUE;
				break;
			}
			if (h->nlmsg_type == NLMSG_ERROR) {
				log_(WARN, "Error in Netlink response.\n");
				done = TRUE;
				err = -1;
				break;
			}
			/* response is otherwise ignored
			 */
			h = NLMSG_NEXT(h, status);
		} /* end while(NLMSG_OK) - loop 2 */
	} /* end while(!done) - loop 1 */ 

#endif
	return(err);
}

/*
 * function add_address_to_list()
 *
 * Make a sockaddr and add it to a list.
 */
sockaddr_list *add_address_to_list(sockaddr_list **list, struct sockaddr *addr,
    int ifi)
{
	sockaddr_list *item, *new_item, *last_item;

	/* make a new sockaddr_list element */
	new_item = (sockaddr_list*) malloc(sizeof(sockaddr_list));
	if (!new_item)
		return NULL;
	memset(new_item, 0, sizeof(sockaddr_list));
	memcpy(&new_item->addr, addr, SALEN(addr));
	new_item->if_index = ifi;
	new_item->status = UNVERIFIED;
	new_item->next = NULL;
	
	/* append element to list */
	if (*list) {
		for(item = *list; item; item = item->next) {
			/* check if new_item already exists */
			if ((item->if_index == new_item->if_index) &&
			    (item->addr.ss_family == new_item->addr.ss_family)
			    && (!memcmp(SA2IP(&item->addr),
				 SA2IP(&new_item->addr), SAIPLEN(addr)))) {
				free(new_item);
				return(item);
			}
			last_item = item;
		}
		last_item->next = new_item;
	} else {
		*list = new_item;
	}
	return(new_item);
}

/*
 * function delete_address_from_list()
 *
 * Remove a given address from the list, or remove all addresses associated
 * with if_index (when len==0).
 */
void delete_address_from_list(sockaddr_list **list, struct sockaddr *addr,
    int ifi)
{
	sockaddr_list *item, *prev;
	int remove;

	if (!*list) /* no list */
		return;

	remove = FALSE;
	prev = NULL;
	item = *list;
	while (item) {
		/* remove from list if if_index matches */
		if (!addr) {
			if (item->if_index == ifi)
				remove = TRUE;
		/* remove from list if address matches */
		} else {
			if ((item->addr.ss_family == addr->sa_family) &&
			    (memcmp(SA2IP(&item->addr), SA2IP(addr),
				    SAIPLEN(addr))==0)) {
				/* address match */
				remove = TRUE;
			}
		}
		if (!remove) { /* nothing to delete, advance in list... */
			prev = item;
			item = item->next;
			continue;
		}
		remove = FALSE;
		if (prev) {
			prev->next = item->next;
			free(item);
			item = prev->next;
		} else { /* delete first item in list */
			*list = item->next;
			free(item);
			item = *list;
		}
	}
}


void delete_address_entry_from_list(sockaddr_list **list, sockaddr_list *entry)
{
	sockaddr_list *item, *prev;

	if (!*list) /* no list */
		return;
	
	prev = NULL;
	item = *list;
	while (item) {
		/* pointer match */
		if (item == entry) {
			if (prev) {
				prev->next = item->next;
				free(item);
				item = prev->next;
			} else {
				/* for hi_node->addrs, we cannot delete
				 * the first item in the list! */
				return;
			}
			break;
		} else {
			prev = item;
			item = item->next;
		}
	}
}

/*
 * function is_my_address()
 *
 * Returns the interface index if supplied address is found in my_addr_head, 
 * FALSE (0) otherwise. (The interface index is never zero.)
 */
int is_my_address(struct sockaddr *addr)
{
	sockaddr_list *l;

	for (l = my_addr_head; l; l=l->next) {
		if (addr->sa_family != l->addr.ss_family)
			continue;
		if (memcmp(SA2IP(&l->addr), SA2IP(addr), SAIPLEN(addr))==0) {
			/* address match */
			return(l->if_index);
		}
	}
	return FALSE;
}

void print_addr_list(sockaddr_list *list)
{
	sockaddr_list *l;
	log_(NORM, "Address list: [");
	for (l = my_addr_head; l; l=l->next) {
		log_(NORM, "(%d)%s, ", l->if_index,
		    logaddr((struct sockaddr*)&l->addr));
	}
	log_(NORM, "]\n");
}

/*
 * function hip_handle_netlink()
 *
 * Handles received netlink messages. Returns 1 if address change requires
 * selection/publishing new preferred address, 0 otherwise.
 */
int hip_handle_netlink(char *data, int length)
{
	struct nlmsghdr *msg;
	struct ifinfomsg *ifinfo; /* link layer specific message */
	struct ifaddrmsg *ifa; /* interface address message */
	struct rtattr *rta, *tb[IFA_MAX+1];
	int len, is_add, retval = 0;
	struct sockaddr_storage ss_addr;
	struct sockaddr *addr;
	sockaddr_list *l;
	
	NatType nattype;

	addr = (struct sockaddr*) &ss_addr;

	for (msg = (struct nlmsghdr*)data; NLMSG_OK(msg, (__u32)length);
	     msg = NLMSG_NEXT(msg, length)) {
		switch(msg->nlmsg_type) {
		case RTM_NEWLINK:
			/* wait for RTM_NEWADDR to add addresses */
			break;
		case RTM_DELLINK:
			ifinfo = (struct ifinfomsg*)NLMSG_DATA(msg);
			delete_address_from_list(&my_addr_head, NULL,
			    ifinfo->ifi_index);
			break;
		/* Add or delete address from my_addr_head */
		case RTM_NEWADDR:
		case RTM_DELADDR:
			ifa = (struct ifaddrmsg*)NLMSG_DATA(msg);
			rta = IFA_RTA(ifa);
			len = msg->nlmsg_len - NLMSG_LENGTH(sizeof(*ifa));
			if ((ifa->ifa_family != AF_INET) &&
			    (ifa->ifa_family != AF_INET6))
				continue;

			memset(tb, 0, sizeof(tb));
			memset(addr, 0, sizeof(struct sockaddr_storage));
			is_add = (msg->nlmsg_type==RTM_NEWADDR);
			
			/* parse list of attributes into table
			 * (same as parse_rtattr()) */
			while (RTA_OK(rta, len)) {
				if (rta->rta_type <= IFA_MAX)
					tb[rta->rta_type] = rta;
				rta = RTA_NEXT(rta,len);
		}
			/* fix tb entry for inet6 */
			if (!tb[IFA_LOCAL]) 
				tb[IFA_LOCAL] = tb[IFA_ADDRESS];
			if (!tb[IFA_ADDRESS])
				tb[IFA_ADDRESS] = tb[IFA_LOCAL];

			if (!tb[IFA_LOCAL])
				continue;
			addr->sa_family = ifa->ifa_family;
			memcpy(	SA2IP(addr), RTA_DATA(tb[IFA_LOCAL]),
				RTA_PAYLOAD(tb[IFA_LOCAL]) );
			log_(NORM, "Address %s: (%d)%s \n", (is_add) ? "added" :
			    "deleted", ifa->ifa_index, logaddr(addr));

#ifndef __MACOSX__
			/* NAT detection */
			if (OPT.stun && is_add) {
				log_(NORMT, "STUN: NAT detection with server ");
				printIPv4Addr (&STUN_server_addr);
				log_(NORM, "\n");
				nattype = stunNatType( &STUN_server_addr,FALSE, NULL, NULL, 0, NULL) ;
				if (nattype == StunTypeOpen || nattype == StunTypeFirewall) {
					is_behind_nat = FALSE ;
					log_(NORM, "STUN: No NAT detected.\n");
				} else {
					is_behind_nat = TRUE ;
					log_(NORM, "STUN: NAT detected, UDP encapsulation activated.\n");
				}
			}
#endif

			/* update our global address list */
			if (is_add) {
				l = add_address_to_list(&my_addr_head, addr,
							ifa->ifa_index);
				l->status = ACTIVE;
				/* Need to select_preferred_address() and
				 * publish_my_hits() here, but the address
				 * was just added and we may get no route to
				 * host errors, so handle later */
				retval = 1;
			} else {
				delete_address_from_list(&my_addr_head, addr,
				    ifa->ifa_index);
			}

			/* update each SA, handle HIP readdressing */
			handle_local_address_change(is_add, addr,
						    ifa->ifa_index);
			
			break;
		default:
			break;
		}
	}
	return retval;
}

/*
 * handle_local_address_change()
 * 
 * Handle adding/deleting addresses to/from HIP associations, performing
 * readdress when needed. (readdress occurs after a preferred has been deleted)
 */
void handle_local_address_change(int add, struct sockaddr *newaddr,int if_index)
{
	int i;
	hip_assoc *hip_a;


	if (!VALID_FAM(newaddr))
		return;

	/* add/delete on all */
	for (i=0; i < max_hip_assoc; i++) {
		hip_a = &hip_assoc_table[i];
		/* perform basic check of association */
		if ((hip_a->state==0) || !hip_a->hi || !hip_a->peer_hi)
			continue;
		/* add or delete from list in hip_a; both functions will check
		 * if the address already exists */
		if (add) {
			association_add_address(hip_a, newaddr, if_index);
		} else {
			association_del_address(hip_a, newaddr, if_index);
		}
	}

	if (add && if_index == external_if_index && max_hip_mr_clients > 0) {
		pthread_mutex_lock(&hip_mr_client_mutex);
		new_external_address = TRUE;
		pthread_mutex_unlock(&hip_mr_client_mutex);
	}
}

/*
 * readdress_association()
 *
 * Perform readdressing tasks due to local address changes.
 */
void readdress_association(hip_assoc *hip_a, struct sockaddr *newaddr, 
    int if_index)
{
	int err=0;
	struct sockaddr *oldaddr = HIPA_SRC(hip_a);

	log_(NORMT, "Readdressing association with %s (%s) from ",
		hip_a->peer_hi->name, logaddr(HIPA_DST(hip_a)));
	log_(NORM, "%s to ", logaddr(oldaddr));
	log_(NORM, "%s.\n", logaddr(newaddr));
	if (hip_a->state != ESTABLISHED) {
		log_(NORMT, "NOT readdressing association since state=%d\n",
			hip_a->state);
		return;
	}
	log_hipa_fromto(QOUT, "Update initiated (readdress)", 
			hip_a, FALSE, TRUE);

	rebuild_sa(hip_a, newaddr, 0, FALSE, FALSE, is_behind_nat);
	rebuild_sa(hip_a, newaddr, 0, TRUE, FALSE, is_behind_nat);
	err = sadb_readdress(oldaddr, newaddr, hip_a, hip_a->spi_in);
	
	/* replace the old preferred address */
	memcpy(&hip_a->hi->addrs.addr, newaddr, SALEN(newaddr));
	hip_a->hi->addrs.if_index = if_index;
	hip_a->hi->addrs.lifetime = 0; /* XXX need to copy from somewhere? */
	hip_a->hi->addrs.preferred = TRUE;
	make_address_active(&hip_a->hi->addrs);

	/* must send ESP_INFO with new UPDATE message */
	if (!hip_a->rekey) {
		if (build_rekey(hip_a) < 0)
			log_(WARN, "readdress_association() had problem buildi"
				"ng a new rekey structure for ESP_INFO\n");
	}

	if (OPT.stun) {
		hip_a->next_use_udp = is_behind_nat;
	}
	if (is_behind_nat) { /* && hip_a->peer_dst_port==0)  */
		hip_a->peer_dst_port = HIP_UDP_PORT;
		hip_a->peer_esp_dst_port = HIP_ESP_UDP_PORT;
	}

	/* inform peer of new preferred address */
	if (hip_send_update(hip_a, newaddr, NULL, is_behind_nat) < 0)
		log_(WARN, "Problem sending UPDATE(REA) for %s!\n",
		    logaddr(newaddr));
#ifdef __UMH__
	if (hip_a->use_udp) { /* (HIP_ESP_OVER_UDP) */
	/* not necessary. it is just meant to update the port for 
	  incoming packets sent before rekeying is completely finished */
		err = send_udp_esp_tunnel_activation (hip_a->spi_out);
		if (err<0) {
			printf("Activation of UDP-ESP channel failed.\n");
		} else {
			printf("Activation of UDP-ESP channel for spi:0x%x done.\n", hip_a->spi_out);
		}
	}
#endif /* __UMH__ */
}

/* 
 * association_add_address()
 *
 * An address has been added to this interface, so add it
 * to the list in hip_a->hi->addrs.
 * If the preferred address was deleted, make this the new
 * preferred address and perform readdressing procedures. */
void association_add_address(hip_assoc *hip_a, struct sockaddr *newaddr,
    int if_index)
{
	sockaddr_list *list, *l;
	struct sockaddr *oldaddr;
#ifdef ALWAYS_SEND_DH
	dh_cache_entry *dh_entry;
#endif
	

	/* 
	 * If preferred address is deleted, do readdress and replace it
	 */
	if (hip_a->hi->addrs.status == DELETED) {
		oldaddr = HIPA_SRC(hip_a);
		if (!memcmp(oldaddr, newaddr, SALEN(newaddr))) {
			/* address is same, 'undelete' */
			make_address_active(&hip_a->hi->addrs);
			return;
		}
		/* perform readdress */
		readdress_association(hip_a, newaddr, if_index);
	/* 
	 * Add the new address to the end of the list (or unmark deleted status)
	 */
	} else {
		/* this function checks if the address already exists */
		list = &hip_a->hi->addrs;
		l = add_address_to_list(&list, newaddr, if_index);
		make_address_active(l);

	}
}

/* 
 * association_del_address()
 *
 * An address has been deleted from this interface, mark its status as DELETED. 
 * Try to perform readdressing if the preferred has been deleted.
 */
void association_del_address(hip_assoc *hip_a, struct sockaddr *newaddr,
    int if_index)
{
	sockaddr_list *l, *deleted, *list;

	/* Search this hip_a address list for deleted */
	list = &hip_a->hi->addrs;
	for (l = list; l; l=l->next) {
		if (newaddr->sa_family != l->addr.ss_family)
			continue;
		if (!memcmp(SA2IP(&l->addr), SA2IP(newaddr), SAIPLEN(&l->addr)))
			break;
	}
	
	if (!l) /* Deleted address not found, do nothing. */
		return;

	/* Deleted address exists in this association. */
	l->status = DELETED;
	deleted = l;
	if (deleted != list)  /* not the preferred address, exit */ 
		return;
	/* XXX inform peer via UPDATE? */

	/* 
	 * Preferred address has been deleted, 
	 * so switch to another address if possible.
	 */
	l = NULL;
	for (l = list->next; l; l=l->next) { /* look for same family */
		if (newaddr->sa_family != l->addr.ss_family)
			continue;
		else
			break;
	}
	/* Switch to the next address in the list. */
	if (l) {
		readdress_association(hip_a, SA(&l->addr), if_index);
		delete_address_entry_from_list(&list, l);
		return;
	/* No address with same address family in hip_a list. 
	 * We need to look further in the system-wide my_addr_head
	 * list, after select_preferred_address() is called...  */
	} else {
		log_(NORM, "Selecting a new preferred address since none " \
			   "available in association...\n");
		select_preferred_address();
		for (l = my_addr_head; l; l=l->next) {
			if (newaddr->sa_family != l->addr.ss_family)
				continue;
			if (l->preferred) /* find the preferred */
				break;
		}
		/* is the system-wide preferred address different? */
		if (l && memcmp(SA2IP(&deleted->addr), SA2IP(&l->addr),
			        SAIPLEN(&l->addr))) {
			/* new preferred selected */
			readdress_association(hip_a, SA(&l->addr), 
					      if_index);
			delete_address_entry_from_list(&list, l);
		} else {
			log_(NORMT, "Preferred address deleted, but could not" \
				    " find a suitable replacement.\n");
		}
	}

}

/*
 *  make_address_active()
 */
void make_address_active(sockaddr_list *item)
{
	if (!item)
		return;
	item->status = ACTIVE;
	gettimeofday(&item->creation_time, NULL);
}

/*
 * update the address of a peer in the peer_hi_head list
 */
int update_peer_list_address(const hip_hit peer_hit, struct sockaddr *old_addr, struct sockaddr *new_addr)
{
	sockaddr_list *l;
	hi_node *peer_hi = find_host_identity(peer_hi_head, peer_hit);
	if (!peer_hi)
		return(-1);
	if (!new_addr)
		return(-1);

	l = &peer_hi->addrs;
	/* remove old address, if any specified */
	if (old_addr) /* or should we just flag deleted? */
		delete_address_from_list(&l, old_addr, 0);
	/* add the new address */
	l = add_address_to_list(&l, new_addr, 0);
	return ( l ? 0 : -1 );
}

#endif /* #ifndef __MACOSX__ */
