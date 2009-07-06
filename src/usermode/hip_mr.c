/*
 * Host Identity Protocol
 * Copyright (C) 2005-08 the Boeing Company
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
 *  hip_mr.c
 *
 *  Authors: Jeff Ahrenholz <jeffrey.m.ahrenholz@boeing.com>
 *           Orlie Brewer <orlie.t.brewer@boeing.com>
 * 
 * Mobile router SPINAT implemenation
 *
 */

#include <unistd.h>
#include <pthread.h>            /* phread_exit() */
#include <netinet/ip.h>         /* struct ip */
#include <netinet/ip6.h>        /* struct ip6_hdr */
#include <netinet/icmp6.h>      /* struct icmp6_hdr */
#include <netinet/tcp.h>        /* struct tcphdr */
#include <netinet/udp.h>        /* struct udphdr */
#include <arpa/inet.h>
#include <stdio.h>              /* printf() */
#include <string.h>             /* strerror() */
#include <errno.h>              /* errno */
#include <hip/hip_service.h>
#include <hip/hip_types.h>
#include <hip/hip_funcs.h>
#include <hip/hip_globals.h>
#include <openssl/rand.h>	/* RAND_bytes() */
#include <linux/netfilter.h>    /* NF_DROP */
#include <libipq.h>		/* ipq_create_handle() */

#define INET6_ADDRSTRLEN 46
#define BUFSIZE 2048
#define MR_TIMEOUT_US 500000 /* microsecond timeout for mobile_router select()*/

struct ip_esp_hdr {
	__u32 spi;
	__u32 seq_no;
	__u8 enc_data[0];
};

extern hip_mr_client hip_mr_client_table[MAX_MR_CLIENTS];
extern int max_hip_mr_clients;
extern int new_external_address;
extern struct sockaddr_storage external_address;
extern pthread_mutex_t hip_mr_client_mutex;

static char *external_interface;
static struct sockaddr_storage out_addr;

int hip_send_proxy_update(struct sockaddr *newaddr, struct sockaddr *dstaddr,
			hip_hit *mn_hit, hip_hit *peer_hit,
			hip_proxy_ticket *ticket, __u32 spi);
int build_tlv_proxy_hmac(hip_proxy_ticket *ticket, __u8 *data, int location,
			int type);

/* Check if the given addresses are equal to the ones in the packet header */
int addresses_are_equal(struct sockaddr *src, struct sockaddr *dst,
                        int family, unsigned char *payload)
{
	int ret = FALSE;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;
	struct in_addr ip4_src, ip4_dst;
	struct in6_addr ip6_src, ip6_dst;

	if (src->sa_family != family || dst->sa_family != family)
		return ret;

	if (family == AF_INET) {
		ip4h = (struct ip *) payload;
		memcpy(&ip4_src, SA2IP(src), SAIPLEN(src));
		memcpy(&ip4_dst, SA2IP(dst), SAIPLEN(dst));
		if (ip4_src.s_addr == ip4h->ip_src.s_addr &&
			ip4_dst.s_addr == ip4h->ip_dst.s_addr)
			ret = TRUE;
	} else {
		ip6h = (struct ip6_hdr *) payload;
		memcpy(&ip6_src, SA2IP(src), SAIPLEN(src));
		memcpy(&ip6_dst, SA2IP(dst), SAIPLEN(dst));
		if (IN6_ARE_ADDR_EQUAL(&ip6_src, &ip6h->ip6_src) &&
			IN6_ARE_ADDR_EQUAL(&ip6_dst, &ip6h->ip6_dst))
			ret = TRUE;
	}

	return ret;
}

/*
 *
 * function get_next_spinat()
 *
 * in:          none
 *
 * out:         returns next SPI value to use for SPINAT
 *
 * Obtains new random SPI for SPINAT, checks that it is not being used.
 * TODO: Should also check that it is not being used for mobile router SAs.
 */

__u32 get_next_spinat(void)
{
	int i;
	__u32 new_spi;
	hip_spi_nat *spi_nats;

retry_getspi:
	/* randomly select a new SPI */
	new_spi = 0;
	while (new_spi <= SPI_RESERVED) {
		RAND_bytes((__u8*)&new_spi, 4);
	}

	for (i = 0; i < max_hip_mr_clients; i++) {
		for (spi_nats = hip_mr_client_table[i].spi_nats; spi_nats;
		     spi_nats = spi_nats->next) {
			if (new_spi == spi_nats->public_spi)
				goto retry_getspi;
		}
	}
	return new_spi;
}

/* Look for the given hit in the mobile router client table */

hip_mr_client *check_hits(hip_hit the_hit)
{
	int i;
	for (i = 0; i < max_hip_mr_clients; i++) {
		if (hits_equal(the_hit, hip_mr_client_table[i].mn_hit)) {
/*
			struct sockaddr *src = (struct sockaddr*)&hip_mr_client_table[i].mn_addr;
			printf("This is for Mobile Router client %s\n", logaddr(src));
*/
			return &hip_mr_client_table[i];
		}
	}
	return NULL;
}

/* Rewrite addresses in packet header */

void adjust_addrs(struct sockaddr_storage *s, struct sockaddr_storage *d,
		unsigned char *payload)
{
	struct sockaddr *src, *dst;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;
	char ipstr[INET6_ADDRSTRLEN];

	src = (struct sockaddr *)s;
	dst = (struct sockaddr *)d;
inet_ntop(src->sa_family, SA2IP(src), ipstr, sizeof(ipstr));
printf("adjust_addrs to (%s, ", ipstr);
inet_ntop(dst->sa_family, SA2IP(dst), ipstr, sizeof(ipstr));
printf("%s)\n", ipstr);

	if (src->sa_family != dst->sa_family)
		return;

	if (src->sa_family == PF_INET) {
		ip4h = (struct ip *) payload;
		memcpy(&ip4h->ip_src, SA2IP(src), SAIPLEN(src));
		memcpy(&ip4h->ip_dst, SA2IP(dst), SAIPLEN(dst));
		// XXX jeffa: need to recompute IPv4 checksum here
	} else {
		ip6h = (struct ip6_hdr *) payload;
		memcpy(&ip6h->ip6_src, SA2IP(src), SAIPLEN(src));
		memcpy(&ip6h->ip6_dst, SA2IP(dst), SAIPLEN(dst));
	}

}

/*
 *
 * mr_process_I1()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *
 * out: Perform SPINAT on packet
 *
 * Process the I1 from the mobile node, create SPINAT state.
 */

void mr_process_I1(hip_mr_client *hip_mr_c, int family,
		hiphdr *hiph, unsigned char *payload)
{
	struct sockaddr *dst;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;

	hip_spi_nat *spi_nats = hip_mr_c->spi_nats;

	printf("mr_process_I1 %s\n", family==AF_INET ? "IPv4" : "IPv6");
	while (spi_nats) {
		if (hits_equal(hiph->hit_rcvr, spi_nats->peer_hit)) {
			break;
		}
		spi_nats = spi_nats->next;
	}

	if (!spi_nats) {
		printf("allocating new spi_nat structure\n");
		spi_nats = malloc(sizeof(hip_spi_nat));
		if (!spi_nats)
			return;
		memset(spi_nats, 0, sizeof(hip_spi_nat));
		spi_nats->next = hip_mr_c->spi_nats;
		hip_mr_c->spi_nats = spi_nats;
        	memcpy(spi_nats->peer_hit, hiph->hit_rcvr, sizeof(hip_hit));
	}

	spi_nats->private_spi = 0;
	spi_nats->public_spi = 0;
	dst = (struct sockaddr *)&spi_nats->peer_addr;
	dst->sa_family = family;
	if (family == PF_INET) {
		ip4h = (struct ip *) payload;
		memcpy(SA2IP(dst), &ip4h->ip_dst, SAIPLEN(dst));
	} else {
		ip6h = (struct ip6_hdr *) payload;
		memcpy(SA2IP(dst), &ip6h->ip6_dst, SAIPLEN(dst));
	}

	adjust_addrs(&out_addr, &spi_nats->peer_addr, payload);

	return;
}

/*
 *
 * mr_process_R1()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *
 * out: Perform SPINAT on packet
 *
 * Process the R1 from the peer node, grab the LOCATOR info of the peer.
 */

void mr_process_R1(hip_mr_client *hip_mr_c, int family,
		hiphdr *hiph, unsigned char *payload)
{
	int location = 0;
	__u8 *data = (__u8 *)hiph;
	int data_len;
	int type, length;
	tlv_head *tlv;
	tlv_locator *loc;
	locator *loc1;
	__u8 *p_addr = NULL;

	hip_spi_nat *spi_nats = hip_mr_c->spi_nats;

	while (spi_nats) {
		if (hits_equal(hiph->hit_sndr, spi_nats->peer_hit)) {
			break;
		}
		spi_nats = spi_nats->next;
	}

	if (!spi_nats)
		return;

	data_len = (hiph->hdr_len+1) * 8;
	location += sizeof(hiphdr);

	while (location < data_len) {
		tlv = (tlv_head *) &data[location];
		type = ntohs(tlv->type);
		length = ntohs(tlv->length);
		p_addr = NULL;
		if (type == PARAM_LOCATOR) {
			loc = (tlv_locator *)tlv;
			loc1 = &loc->locator1[0];
			if ((loc1->locator_type == LOCATOR_TYPE_IPV6) &&
					(loc1->locator_length == 4)) {
				p_addr = &loc1->locator[0];
			} else if ((loc1->locator_type == LOCATOR_TYPE_SPI_IPV6) &&
					(loc1->locator_length == 5)) {
				p_addr = &loc1->locator[4];
			} else {
				log_(WARN, "Invalid locator type %d / length %d.\n",
					loc1->locator_type, loc1->locator_length);
			}
    }
		if (p_addr) {
			/*
			* Read in address from LOCATOR
			*/
			struct sockaddr *addr = NULL;

			if (IN6_IS_ADDR_V4MAPPED((struct in6_addr*)p_addr)) {
				addr = (struct sockaddr*)&spi_nats->peer_ipv4_addr;
				addr->sa_family = AF_INET;
				memcpy(SA2IP(addr), p_addr + 12, SAIPLEN(addr));
				if (IN_MULTICAST(SA2IP(addr)))
					memset(addr, 0, sizeof(struct sockaddr_storage));
				if (((struct sockaddr_in*)addr)->sin_addr.s_addr == INADDR_BROADCAST)
					memset(addr, 0, sizeof(struct sockaddr_storage));
			} else {
				addr = (struct sockaddr*)&spi_nats->peer_ipv6_addr;
				addr->sa_family = AF_INET6;
				memcpy(SA2IP(addr), p_addr, SAIPLEN(addr));
				unsigned char *p = SA2IP(addr);
				if (IN6_IS_ADDR_MULTICAST((struct in6_addr*)p))
					memset(addr, 0, sizeof(struct sockaddr_storage));
				/* IPv6 doesn't have broadcast addresses */
			}

		}

		location += tlv_length_to_parameter_length(length);
	}

	adjust_addrs(&spi_nats->peer_addr, &hip_mr_c->mn_addr, payload);
}

/*
 *
 * mr_process_I2()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *
 * out: Perform SPINAT on packet
 *
 * Process the I2 from the mobile node, get external SPI.
 */


__u32 mr_process_I2(hip_mr_client *hip_mr_c, int family,
		hiphdr *hiph, unsigned char *payload)
{
	int location = 0;
	__u8 *data = (__u8 *)hiph;
	int data_len;
	int type, length;
	tlv_head *tlv;
	tlv_esp_info *esp_info;

	hip_spi_nat *spi_nats = hip_mr_c->spi_nats;

	while (spi_nats) {
		if (hits_equal(hiph->hit_rcvr, spi_nats->peer_hit)) {
			break;
		}
		spi_nats = spi_nats->next;
	}

	if (!spi_nats)
		return 0;

	data_len = (hiph->hdr_len+1) * 8;
	location += sizeof(hiphdr);

	while (location < data_len) {
		tlv = (tlv_head *) &data[location];
		type = ntohs(tlv->type);
		length = ntohs(tlv->length);
		if (type == PARAM_ESP_INFO) {
			esp_info = (tlv_esp_info *)tlv;
			spi_nats->private_spi = ntohl(esp_info->new_spi);
			spi_nats->public_spi = get_next_spinat();
			log_(NORM, "Mobile node SPI 0x%x\n", spi_nats->private_spi);
			log_(NORM, "External SPI 0x%x added\n", spi_nats->public_spi);
			break;
		}
		location += tlv_length_to_parameter_length(length);
	}

	adjust_addrs(&out_addr, &spi_nats->peer_addr, payload);
	return spi_nats->public_spi;
}

/*
 *
 * mr_process_R2()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *
 * out: Perform SPINAT on packet
 *
 * Process the R2 from the peer node, grab the SPI of the peer.
 */

void mr_process_R2(hip_mr_client *hip_mr_c, int family,
		hiphdr *hiph, unsigned char *payload)
{
	int location = 0;
	__u8 *data = (__u8 *)hiph;
	int data_len;
	int type, length;
	tlv_head *tlv;
	tlv_esp_info *esp_info;

	hip_spi_nat *spi_nats = hip_mr_c->spi_nats;

	while (spi_nats) {
		if (hits_equal(hiph->hit_sndr, spi_nats->peer_hit)) {
			break;
		}
		spi_nats = spi_nats->next;
	}

	if (!spi_nats)
		return;

	data_len = (hiph->hdr_len+1) * 8;
	location += sizeof(hiphdr);

	while (location < data_len) {
		tlv = (tlv_head *) &data[location];
		type = ntohs(tlv->type);
		length = ntohs(tlv->length);
		if (type == PARAM_ESP_INFO) {
			esp_info = (tlv_esp_info *)tlv;
			spi_nats->peer_spi = ntohl(esp_info->new_spi);
			log_(NORM, "Peer SPI 0x%x added\n", spi_nats->peer_spi);
			break;
		}
		location += tlv_length_to_parameter_length(length);
	}

	adjust_addrs(&spi_nats->peer_addr, &hip_mr_c->mn_addr, payload);
}

/*
 *
 * mr_process_CLOSE()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *     packet_type is either CLOSE or CLOSE_ACK.
 *
 * out: Perform SPINAT on packet
 *
 * Process the CLOSE or CLOSE_ACK.
 */

void mr_process_CLOSE(hip_mr_client *hip_mr_c, int family,
		hiphdr *hiph, unsigned char *payload, int packet_type)
{
	int in_bound;
	hip_hit *peer_hit;

	hip_spi_nat *spi_nats = hip_mr_c->spi_nats;

	if (hits_equal(hiph->hit_sndr, hip_mr_c->mn_hit)) {
		in_bound = 0;
		peer_hit = &(hiph->hit_rcvr);
	} else {
		in_bound = 1;
		peer_hit = &(hiph->hit_sndr);
	}

	while (spi_nats) {
		if (hits_equal(*peer_hit, spi_nats->peer_hit)) {
			break;
		}
		spi_nats = spi_nats->next;
	}

	if (!spi_nats)
		return;

	if (in_bound) {
		adjust_addrs(&spi_nats->peer_addr, &hip_mr_c->mn_addr, payload);
	} else { 
		adjust_addrs(&out_addr, &spi_nats->peer_addr, payload);
	}

	/* TODO: Remove state for SA */

	if (packet_type == CLOSE_ACK) {
		;
	}

}

/*
 *
 * add_tlv_spi_nat()
 *
 * in: hip_mr_c pointer to the mobile node client structure
 *     family address family of packet, either AF_INET or AF_INET6
 *     hiph pointer to the HIP header in the packet
 *     payload pointer to a copy of the actual packet
 *
 * out: create new packet adding PARAM_ESP_INFO_NOSIG TLV
 *
 * Add a the external SPI of the mobile node to the I2.
 */

unsigned char *add_tlv_spi_nat(int family, unsigned char *payload,
			size_t data_len, size_t *new_len, __u32 new_spi)
{
	hiphdr *hiph;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;
	tlv_esp_info *esp_info;
	size_t len = data_len + sizeof(tlv_esp_info);
	int hiphdr_len;
	unsigned char *buff = malloc(len);

	if (!buff) {
		return buff;
	}

  /* Copy original packet */
	memcpy(buff, payload, data_len);

	/* ESP INFO */
	esp_info = (tlv_esp_info*) &buff[data_len];
	esp_info->type = htons(PARAM_ESP_INFO_NOSIG);
	esp_info->length = htons(sizeof(tlv_esp_info) - 4);
	esp_info->reserved = 0;
	esp_info->keymat_index = 0;
	esp_info->old_spi = 0;
	esp_info->new_spi = htonl(new_spi);

	/* finish with new length */

	if (family == PF_INET) {
		ip4h = (struct ip *) buff;
		hiph = (hiphdr *)(buff + sizeof(struct ip));
	} else {
		ip6h = (struct ip6_hdr *) buff;
		hiph = (hiphdr *)(buff + sizeof(struct ip6_hdr));
	}
	hiphdr_len = (hiph->hdr_len+1) * 8;
	hiphdr_len += sizeof(tlv_esp_info);
	if (family == PF_INET)
		ip4h->ip_len = htons((unsigned short)hiphdr_len + sizeof(struct ip));
	else
		ip6h->ip6_plen = htons((unsigned short)hiphdr_len);
	hiph->hdr_len = (hiphdr_len/8) - 1;

	log_(NORM, "Adding SPI_NAT of 0x%x\n", new_spi);
	*new_len = len;
	return buff;
}

void generate_hip_updates(struct sockaddr *out)
{
	int i;
	struct sockaddr *dst = NULL;

	if (!out->sa_family) {
		log_(WARN, "No external address for UPDATE\n");
		return;
	}

	for (i = 0; i < max_hip_mr_clients; i++) {
		if (RESPONSE_SENT == hip_mr_client_table[i].state) {
			hip_spi_nat *spi_nats;

			struct sockaddr *src = (struct sockaddr*)&hip_mr_client_table[i].mn_addr;
			log_(NORM, "Doing UPDATE for Mobile Router client %s\n", logaddr(src));

			for (spi_nats = hip_mr_client_table[i].spi_nats;
				spi_nats; spi_nats = spi_nats->next) {

				dst = (struct sockaddr*)&spi_nats->peer_addr;
				if (out->sa_family == AF_INET &&
						AF_INET ==
							((struct sockaddr *)&spi_nats->peer_ipv4_addr)->sa_family)
					dst = (struct sockaddr*)&spi_nats->peer_ipv4_addr;
				if (out->sa_family == AF_INET6 &&
						AF_INET6 ==
							((struct sockaddr *)&spi_nats->peer_ipv6_addr)->sa_family)
					dst = (struct sockaddr*)&spi_nats->peer_ipv6_addr;

				if (dst->sa_family == out->sa_family) {
					hip_send_proxy_update(out, dst,
						&hip_mr_client_table[i].mn_hit,
						&spi_nats->peer_hit,
						&spi_nats->ticket,
						spi_nats->public_spi);
				} else {
					log_(WARN,
					"Unable to find %s external address "
						"for destination %s\n",
						(out->sa_family == AF_INET) ?
						"IPv4" : "IPv6", logaddr(dst));
				}
			}
		}
	}
}

/* Periodic check to see if we have a new external address */
void check_address_change(void)
{
	struct sockaddr *out = (struct sockaddr *)&out_addr;

	if (!new_external_address)
		return;

	pthread_mutex_lock(&hip_mr_client_mutex);
	memcpy(&out_addr, &external_address, sizeof(out_addr));
	new_external_address = FALSE;
	if (!external_interface) {
		external_interface = malloc(strlen(HCNF.outbound_iface) + 1);
		if (!external_interface)
			log_(WARN, "Warning: external_interface malloc error!\n");
		else
			strcpy(external_interface, HCNF.outbound_iface);
	}
	generate_hip_updates(out);
	pthread_mutex_unlock(&hip_mr_client_mutex);
}

/* Perform SPINAT and Mobiler Router service for HIP packets */

unsigned char *check_hip_packet(int family, unsigned char *payload,
			size_t data_len, size_t *new_len)
{
	struct sockaddr *src, *dst;
	struct sockaddr_storage src_addr, dst_addr;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;
	hiphdr *hiph;
	hip_mr_client *hip_mr_c;
	int length;
	unsigned char *buff = payload;
	char ipstr[INET6_ADDRSTRLEN];

	*new_len = data_len;
	if (family == PF_INET) {
		ip4h = (struct ip *) payload;
		hiph = (hiphdr *) (payload + sizeof(struct ip));
	} else {
		ip6h = (struct ip6_hdr *) payload;
		hiph = (hiphdr *) (payload + sizeof(struct ip6_hdr));
	}
	length = (hiph->hdr_len+1) * 8;

/*
	printf("HIP packet from HIT");
	print_hex(hiph->hit_sndr, HIT_SIZE);
	printf(" to HIT");
	print_hex(hiph->hit_rcvr, HIT_SIZE);
	printf("\n");
*/

// XXX jeffa: move locking here
//            lookup here
//     process only based on switch statement
	switch(hiph->packet_type) {
		case HIP_I1:
			pthread_mutex_lock(&hip_mr_client_mutex);
			hip_mr_c = check_hits(hiph->hit_sndr);
			printf("HIP I1 packet of length %d. %s a client\n",
				length, hip_mr_c ? "is" : "NOT" );
			if (hip_mr_c) {
				mr_process_I1(hip_mr_c, family, hiph, payload);
			}
			pthread_mutex_unlock(&hip_mr_client_mutex);
			break;
		case HIP_R1:
			pthread_mutex_lock(&hip_mr_client_mutex);
			hip_mr_c = check_hits(hiph->hit_rcvr);
			printf("HIP R1 packet of length %d. %s a client\n",
				length, hip_mr_c ? "is" : "NOT" );
			if (hip_mr_c) {
				mr_process_R1(hip_mr_c, family, hiph, payload);
			}
			pthread_mutex_unlock(&hip_mr_client_mutex);
			break;
		case HIP_I2:
			pthread_mutex_lock(&hip_mr_client_mutex);
			hip_mr_c = check_hits(hiph->hit_sndr);
			printf("HIP I2 packet of length %d. %s a client\n",
				length, hip_mr_c ? "is" : "NOT" );
			if (hip_mr_c) {
				__u32 new_spi;
				new_spi = mr_process_I2(hip_mr_c, family, hiph, payload);
				if (new_spi) {
					buff = add_tlv_spi_nat(family, payload,
						data_len, new_len, new_spi);
				}
			}
			pthread_mutex_unlock(&hip_mr_client_mutex);
			break;
		case HIP_R2:
			printf("HIP R2 packet of length %d\n", length);
			pthread_mutex_lock(&hip_mr_client_mutex);
			hip_mr_c = check_hits(hiph->hit_rcvr);
			if (hip_mr_c) {
				mr_process_R2(hip_mr_c, family, hiph, payload);
			}
			pthread_mutex_unlock(&hip_mr_client_mutex);
			break;
		case CLOSE:
		case CLOSE_ACK:
			printf("%s packet of length %d\n", (hiph->packet_type == CLOSE) ?
				"HIP CLOSE" : "HIP CLOSE ACK", length);
			pthread_mutex_lock(&hip_mr_client_mutex);
			if ((hip_mr_c = check_hits(hiph->hit_rcvr))  ||
			    (hip_mr_c = check_hits(hiph->hit_sndr))) {
				mr_process_CLOSE(hip_mr_c, family, hiph, payload,
					hiph->packet_type);
			}
			pthread_mutex_unlock(&hip_mr_client_mutex);
			break;
	}

	/* finish with new checksum */

	if (buff != payload) {
		if (family == PF_INET) {
			ip4h = (struct ip *)buff;
			hiph = (hiphdr *)(buff + sizeof(struct ip));
		} else {
			ip6h = (struct ip6_hdr *)buff;
			hiph = (hiphdr *)(buff + sizeof(struct ip6_hdr));
		}
	}
	src = (struct sockaddr *)&src_addr;
	dst = (struct sockaddr *)&dst_addr;
	src->sa_family = family;
	dst->sa_family = family;
	if (family == PF_INET) {
		memcpy(SA2IP(src), &(ip4h->ip_src), SAIPLEN(src));
		memcpy(SA2IP(dst), &(ip4h->ip_dst), SAIPLEN(dst));
	} else {
		memcpy(SA2IP(src), &(ip6h->ip6_src), SAIPLEN(src));
		memcpy(SA2IP(dst), &(ip6h->ip6_dst), SAIPLEN(dst));
	}
	memset(ipstr, 0, sizeof(ipstr));
	inet_ntop(family, SA2IP(src), ipstr, sizeof(ipstr));
	printf("addresses are now %s, ", ipstr);
	memset(ipstr, 0, sizeof(ipstr));
	inet_ntop(family, SA2IP(dst), ipstr, sizeof(ipstr));
	printf("%s\n", ipstr);
	hiph->checksum = 0;
	hiph->checksum = checksum_packet((__u8 *)hiph, src, dst);

	// printf("\n");
	return buff;
}

/* Translate packet between IPv4 and IPv6 */

unsigned char * new_header(int family, unsigned char *payload)
{
	__u32 tc;
	int data_len;
	unsigned char *data;
	struct ip_esp_hdr *esph;
	struct ip *ip4h;
	struct ip6_hdr *ip6h;

	if (family == AF_INET) {
		ip4h = (struct ip *)payload;
		esph = (struct ip_esp_hdr *) (payload + sizeof(struct ip));
		data_len = ntohs(ip4h->ip_len) - sizeof(struct ip);
		data = malloc(sizeof(struct ip6_hdr) + data_len);
		if (data) {
			ip6h = (struct ip6_hdr *)data;
			memset(ip6h, 0, sizeof(struct ip6_hdr));
			ip6h->ip6_flow = 0; /* zero the version (4), TC (8), flow-ID (20) */
			ip6h->ip6_vfc = 0x60;
			ip6h->ip6_plen = htons(data_len);
			ip6h->ip6_nxt = ip4h->ip_p;
			ip6h->ip6_hlim = ip4h->ip_ttl;
			tc = ip4h->ip_tos << 20;
			ip6h->ip6_flow |= tc;   /* 8 bits traffic class */
			ip6h->ip6_hlim = ip4h->ip_ttl;    /* __u8 */
			memcpy(data + sizeof(struct ip6_hdr), esph, data_len);
		}
	} else {
		ip6h = (struct ip6_hdr *)payload;
		esph = (struct ip_esp_hdr *) (payload + sizeof(struct ip6_hdr));
		data_len = ntohs(ip6h->ip6_plen);
		data = malloc(sizeof(struct ip) + data_len);
		if (data) {
			ip4h = (struct ip *)data;
			memset(ip4h, 0, sizeof(struct ip));
			ip4h->ip_v = 4;
			ip4h->ip_hl = 5;
			tc = ip6h->ip6_flow & 0x1100000;
			ip4h->ip_tos = tc >> 20;
			ip4h->ip_len = htons(data_len + sizeof(struct ip));
			ip4h->ip_id  = 0;
			ip4h->ip_off = htons(0x4000);
			ip4h->ip_ttl = ip6h->ip6_hlim;
			ip4h->ip_p = ip6h->ip6_nxt;
			ip4h->ip_sum = 0;
			memcpy(data + sizeof(struct ip), esph, data_len);
		}
	}

	return data;
}

/* Perform SPINAT on ESP packets */

unsigned char *check_esp_packet(int family, int inbound, unsigned char *payload)
{
	int i;
	unsigned char *new_payload = NULL;
	struct sockaddr *out = (struct sockaddr *)&out_addr;
	struct ip_esp_hdr *esph;

	esph = (struct ip_esp_hdr *) (payload + ((family == AF_INET) ?
			sizeof(struct ip) : sizeof(struct ip6_hdr)));

/*
	printf("ESP packet with SPI 0x%x\n", ntohl(esph->spi));
*/
	pthread_mutex_lock(&hip_mr_client_mutex);
	for (i = 0; i < max_hip_mr_clients; i++) {
		struct sockaddr *addr = (struct sockaddr *)&hip_mr_client_table[i].mn_addr;
// XXX BUGFIX
		hip_spi_nat *spi_nats; // = hip_mr_client_table[i].spi_nats;
//		while (spi_nats) {
		for (spi_nats = hip_mr_client_table[i].spi_nats;
		     spi_nats; spi_nats = spi_nats->next) {
			if (inbound) {
				if (spi_nats->public_spi != ntohl(esph->spi))
					continue;
/*
				printf("Found the public SPI 0x%x\n", ntohl(esph->spi));
				printf("Changing to 0x%x\n", spi_nats->private_spi);
*/
				esph->spi = htonl(spi_nats->private_spi);
				if (family == addr->sa_family) {
					new_payload = payload;
				} else {
					new_payload = new_header(family, payload);
				}
				if (new_payload) {
					adjust_addrs(&spi_nats->peer_addr,
						     &hip_mr_client_table[i].mn_addr, new_payload);
				}
				pthread_mutex_unlock(&hip_mr_client_mutex);
				return new_payload;
			} else if (!inbound) {
				if (spi_nats->peer_spi != ntohl(esph->spi))
					continue;
				struct sockaddr *peer = (struct sockaddr *)&spi_nats->peer_addr;
				if (!addresses_are_equal(addr, peer, family, payload))
					continue;
				if (family == out->sa_family) {
					struct sockaddr_storage *dst_addr = &spi_nats->peer_addr;
					if (family == AF_INET) {
						if (AF_INET ==
							((struct sockaddr *)&spi_nats->peer_ipv4_addr)->sa_family)
							dst_addr = &spi_nats->peer_ipv4_addr;
					} else if (family == AF_INET6) {
						if (AF_INET6 ==
							((struct sockaddr *)&spi_nats->peer_ipv6_addr)->sa_family)
							dst_addr = &spi_nats->peer_ipv6_addr;
					}
					adjust_addrs(&out_addr, dst_addr, payload);
					pthread_mutex_unlock(&hip_mr_client_mutex);
					return payload;
				} else {
					struct sockaddr_storage *dst_addr = NULL;
					if (family == AF_INET) {
						if (AF_INET6 ==
							((struct sockaddr *)&spi_nats->peer_ipv6_addr)->sa_family)
							dst_addr = &spi_nats->peer_ipv6_addr;
					} else if (family == AF_INET6) {
						if (AF_INET ==
							((struct sockaddr *)&spi_nats->peer_ipv4_addr)->sa_family)
							dst_addr = &spi_nats->peer_ipv4_addr;
					}
					if (dst_addr) {
						/* Need to do IP family translation */
						new_payload = new_header(family, payload);
						if (new_payload) {
							adjust_addrs(&out_addr, dst_addr, new_payload);
						}
					}
					pthread_mutex_unlock(&hip_mr_client_mutex);
					return new_payload;
				}
			}
// BUGFIX
//		spi_nats = spi_nats->next;
		}
	}
	pthread_mutex_unlock(&hip_mr_client_mutex);
	/* Need to determine is this packet is for this host */
	/* Right now just accept */
	return payload;
}

/* Thread for the mobile router functionality in HIP */

void *hip_mobile_router(void *arg)
{
	int family = PF_INET6;
	int err, type, inbound;
	int write_raw, raw_ip4_socket, raw_ip6_socket;
	unsigned int verdict;
	unsigned char buf[BUFSIZE];
	unsigned char *output_buffer;
	size_t output_length;
	struct ipq_handle *h4, *h6;
	struct ip *ip4h = NULL;
	struct ip6_hdr *ip6h = NULL;
	ipq_packet_msg_t *m;
	int highest_descriptor = 0;
	struct timeval timeout;
	fd_set read_fdset;
//	char buffer[INET6_ADDRSTRLEN];

	printf("hip_mobile_router() thread started...\n");

	/* Sockets to use when changing address family */

	raw_ip4_socket = socket(PF_INET, SOCK_RAW, IPPROTO_RAW);
	raw_ip6_socket = socket(PF_INET6, SOCK_RAW, IPPROTO_RAW);
	if (raw_ip4_socket < 0 || raw_ip6_socket < 0) {
		if (raw_ip4_socket < 0) {
			perror("hip_mobile_router() - socket(PF_INET, SOCK_RAW, IPPROTO_RAW) "
				"failed");
		}
		if (raw_ip6_socket < 0) {
			perror("hip_mobile_router() - socket(PF_INET6, SOCK_RAW, IPPROTO_RAW) "
				"failed");
		}
		fflush(stdout);
		return NULL;
	}

	/* IPQ sessions to get packets from iptables */

	h4 = ipq_create_handle(0, PF_INET);
	h6 = ipq_create_handle(0, PF_INET6);
	if (!h4 || !h6) {
		if (!h4) {
			printf("hip_mobile_router() - ipq_create_handle(0, PF_INET) "
				"failed: %s\n", ipq_errstr());
		}
		if (!h6) {
			printf("hip_mobile_router() - ipq_create_handle(0, PF_INET6) "
				"failed: %s\n", ipq_errstr());
		}
		fflush(stdout);
		return NULL;
	}

	err = ipq_set_mode(h4, IPQ_COPY_PACKET, BUFSIZE);
	if (err < 0) {
		printf("*** hip_mobile_router() - ipq_set_mode(IPV4) failed: "
			"%s\n Are the correct kernel modules loaded "
			"(modprobe ip_queue)?", ipq_errstr());
		ipq_destroy_handle(h4);
		ipq_destroy_handle(h6);
		fflush(stdout);
		return NULL;
	}

	err = ipq_set_mode(h6, IPQ_COPY_PACKET, BUFSIZE);
	if (err < 0) {
		printf("*** hip_mobile_router() - ipq_set_mode(IPV6) failed: "
			"%s\n Are the correct kernel modules loaded "
			"(modprobe ip6_queue)?", ipq_errstr());
		ipq_destroy_handle(h4);
		ipq_destroy_handle(h6);
		fflush(stdout);
		return NULL;
	}

	/* Main loop */

	while(g_state == 0) {

		verdict = NF_DROP;
		write_raw = 0;
		/* prepare file descriptor sets */
		FD_ZERO(&read_fdset);
		FD_SET(h4->fd, &read_fdset);
		FD_SET(h6->fd, &read_fdset);
		timeout.tv_sec = 0;
		timeout.tv_usec = MR_TIMEOUT_US;
		highest_descriptor = maxof(2, h4->fd, h6->fd);

		err = select(highest_descriptor + 1, &read_fdset,
			     NULL, NULL, &timeout);
		if (err < 0) { /* select() error */
			if (EINTR == errno)
				continue;
			printf("hip_mobile_router(): select() error: %s.\n",
				strerror(errno));
			continue;
		} else if (err == 0) { /* idle cycle - select() timeout  */
			check_address_change();
			continue;
		} else if (FD_ISSET(h4->fd, &read_fdset)) {
			family = AF_INET;
		} else if (FD_ISSET(h6->fd, &read_fdset)) {
			family = AF_INET6;
		} else {
			printf("hip_mobile_router(): unknown socket "
				"activity\n");
			continue;
		}
		err = ipq_read( (family==AF_INET) ? h4 : h6, buf, BUFSIZE, 0);
		if (err < 0) {
			printf("hip_mobile_router() ipq_read(%s) error: %s\n",
				(family==AF_INET) ? "IPV4" : "IPV6",
				ipq_errstr());
			continue;
		}

		type = ipq_message_type(buf);
		if (NLMSG_ERROR == type) {
			printf("hip_mobile_router(): received error message %d"
				"\n", ipq_get_msgerr(buf));
			continue;
		} else if (IPQM_PACKET != type) {
			printf("hip_mobile_router(): received unexpected type "
				"%d\n", type);
			continue;
		}

		m = ipq_get_packet(buf);
		output_buffer = m->payload;
		output_length = m->data_len;

		if (family == AF_INET) {
			ip4h = (struct ip *)m->payload;
/*
			printf("\nPacket from %s",
				inet_ntop(AF_INET, &(ip4h->ip_src),
					buffer, sizeof(buffer)));
			printf(" to %s\n",
				inet_ntop(AF_INET, &(ip4h->ip_dst),
					buffer, sizeof(buffer))); // */
		} else {
			ip6h = (struct ip6_hdr *)m->payload;
/*
			printf("\nPacket from %s",
				inet_ntop(AF_INET6, &(ip6h->ip6_src),
					buffer, sizeof(buffer)));
			printf(" to %s\n",
				inet_ntop(AF_INET6, &(ip6h->ip6_dst),
					buffer, sizeof(buffer))); // */
		}

		/* Determine if packet is from external side or not */
/*
		if (m->indev_name[0] != 0  &&  m->outdev_name[0] == 0)
			printf("  INPUT from %s\n\n", m->indev_name);
		else if (m->indev_name[0] != 0  &&  m->outdev_name[0] != 0)
			printf("  FORWARD from %s to %s\n\n", m->indev_name,
				m->outdev_name);
		else if (m->indev_name[0] == 0  &&  m->outdev_name[0] != 0)
			printf("  OUTPUT to %s\n\n", m->outdev_name);
*/
		if (external_interface &&
		    (strcmp(m->indev_name, external_interface) == 0))
			inbound = TRUE;
		else
			inbound = FALSE;

		/* Only process HIP and ESP packets */

		if ((family == PF_INET && ip4h->ip_p == H_PROTO_HIP) ||
			  (family == PF_INET6 && ip6h->ip6_nxt == H_PROTO_HIP)) {
			output_buffer = check_hip_packet(family, m->payload,
				m->data_len, &output_length);
			verdict = NF_ACCEPT;
		} else if ((family == PF_INET && ip4h->ip_p == IPPROTO_ESP) ||
		           (family == PF_INET6 && ip6h->ip6_nxt == IPPROTO_ESP)) {
			output_buffer = check_esp_packet(family, inbound, m->payload);
			if (output_buffer == m->payload)
				verdict = NF_ACCEPT;
			else {
				verdict = NF_DROP;
				/* Change of address family */
				if (output_buffer)
					write_raw = (family == PF_INET) ? PF_INET6 : PF_INET;
			}
		}

		
		printf("verdict=%s\n output_length=%d\n",
			verdict==NF_ACCEPT ? "ACCEPT" : "DROP", output_length);
		
		/* Give verdict to IPQ */

		err = ipq_set_verdict((family == PF_INET) ? h4 : h6,
			m->packet_id, verdict, output_length, output_buffer);

		if (err < 0) {
			printf("hip_mobile_router() - ipq_set_verdict(%s) "
				"failed: %s\n", family == PF_INET ? "IPV4" : "IPV6",
				ipq_errstr());
		}

		/* Change of address family, write new packet to raw socket */

		if (write_raw == PF_INET) {

			struct sockaddr_in sendto_addr;
			struct ip *ip4h = (struct ip *)output_buffer;
 
			memset(&sendto_addr, 0, sizeof(sendto_addr));
			sendto_addr.sin_family = AF_INET;
			sendto_addr.sin_addr.s_addr = ip4h->ip_dst.s_addr;
			sendto_addr.sin_port = htons(0);
			output_length = ntohs(ip4h->ip_len);

			int i = sendto(raw_ip4_socket, output_buffer, output_length, 0,
				(const struct sockaddr *)&sendto_addr, sizeof(struct sockaddr_in));
			if (i < 0)
				perror("Error ");

		} else if (write_raw == PF_INET6) {

			struct sockaddr_in6 sendto_addr;
			struct ip6_hdr *ip6h = (struct ip6_hdr *)output_buffer;
 
			memset(&sendto_addr, 0, sizeof(sendto_addr));
			sendto_addr.sin6_family = AF_INET6;
			memcpy(&sendto_addr.sin6_addr.s6_addr, &ip6h->ip6_dst,
				sizeof(struct in6_addr));
			sendto_addr.sin6_port = htons(0);
			output_length = ntohs(ip6h->ip6_plen) + sizeof(struct ip6_hdr);

			int i = sendto(raw_ip6_socket, output_buffer, output_length, 0,
				(const struct sockaddr *)&sendto_addr, sizeof(struct sockaddr_in6));
			if (i < 0)
				perror("Error ");
		}

		if (output_buffer != m->payload)
			free(output_buffer);
	}

	printf("hip_mobile_router() thread shutdown.\n");
	close(raw_ip4_socket);
	close(raw_ip6_socket);
	ipq_destroy_handle(h4);
	ipq_destroy_handle(h6);
	fflush(stdout);
	pthread_exit((void *) 0);
	return(NULL);
}

/*
 *
 * function hip_send_proxy_update()
 * 
 * in:		
 * 		newaddr = new preferred address to include in LOCATOR, or NULL
 * 		dstaddr = alternate destination address, if this is an address
 * 			check message, otherwise NULL
 *		mn_hit = hit of the mobile node
 *		peer_hit = hit of the peer node
 *		ticket = the signed ticket using the keys from the mobile node
 *		spi_in = the SPI from the SPINAT
 * 		
 * out:		Returns bytes sent when successful, -1 on error.
 *
 * Opens a socket and sends the UPDATE packet.
 *
 */
int hip_send_proxy_update(struct sockaddr *newaddr, struct sockaddr *dstaddr,
			hip_hit *mn_hit, hip_hit *peer_hit,
			hip_proxy_ticket *ticket, __u32 spi_in)
{
	struct sockaddr *src, *dst;
	hiphdr *hiph;
	__u8   buff[sizeof(hiphdr)             + 2*sizeof(tlv_locator) +
		    sizeof(tlv_auth_ticket)    +
		    sizeof(tlv_hmac)           + sizeof(tlv_hip_sig) +
		    MAX_SIG_SIZE + 2 ];
	int location=0, retransmit=FALSE;

	tlv_locator *loc;
	tlv_auth_ticket *auth_ticket;
	locator *loc1;
	__u32 loc_spi;

	memset(buff, 0, sizeof(buff));

	src = newaddr;
	dst = dstaddr;

	/* build the HIP header */

	hiph = (hiphdr*) buff;
	hiph->nxt_hdr = IPPROTO_NONE;
	hiph->hdr_len = 0;
	hiph->packet_type = UPDATE;
	hiph->version = HIP_PROTO_VER;
	hiph->res = HIP_RES_SHIM6_BITS;
	hiph->control = 0;
	hiph->checksum = 0;
	memcpy(&hiph->hit_sndr, mn_hit, sizeof(hip_hit));
	memcpy(&hiph->hit_rcvr, peer_hit, sizeof(hip_hit));
	location = sizeof(hiphdr);

	/* set control bits */

	hiph->control = htons(hiph->control);

	/*
	 * Add LOCATOR parameter when supplied with readdressing info.
	 */
	if (newaddr) {
		loc = (tlv_locator*) &buff[location];
		loc->type = htons(PARAM_LOCATOR);
		loc->length = htons(sizeof(tlv_locator) - 4);
		loc1 = &loc->locator1[0];
		loc1->traffic_type = LOCATOR_TRAFFIC_TYPE_BOTH;
		loc1->locator_type = LOCATOR_TYPE_SPI_IPV6;
		loc1->locator_length = 5; /* (32 + 128 bits) / 4 */
		loc1->reserved = LOCATOR_PREFERRED; /* set the P-bit */
		loc1->locator_lifetime = htonl(HCNF.loc_lifetime);
		memset(loc1->locator, 0, sizeof(loc1->locator));
		loc_spi = htonl(spi_in);
		memcpy(loc1->locator, &loc_spi, 4);
		if (newaddr->sa_family == AF_INET6) {
			memcpy(&loc1->locator[4], SA2IP(newaddr),
			    SAIPLEN(newaddr));
		} else {/* IPv4-in-IPv6 address format */
			memset(&loc1->locator[14], 0xFF, 2);
			memcpy(&loc1->locator[16], SA2IP(newaddr),
			    SAIPLEN(newaddr));
		}
		location += sizeof(tlv_locator);
		location = eight_byte_align(location);
		add_reg_request = FALSE;
	}

	/* AUTH_TICKET */
	auth_ticket = (tlv_auth_ticket *) &buff[location];
	auth_ticket->type = htons(PARAM_AUTH_TICKET);
	auth_ticket->length = htons(sizeof(tlv_auth_ticket) - 4);
	auth_ticket->hmac_key_index = htons(ticket->hmac_key_index);
	auth_ticket->transform_type = htons(ticket->transform_type);
	auth_ticket->action         = htons(ticket->action);
	auth_ticket->lifetime       = htons(ticket->lifetime);
	memcpy(auth_ticket->hmac, ticket->hmac, sizeof(auth_ticket->hmac));
	location += sizeof(tlv_auth_ticket);
	location = eight_byte_align(location);

	/* HMAC */
	hiph->hdr_len = (location/8) - 1; 
	location += build_tlv_proxy_hmac(ticket, buff, location, PARAM_HMAC);

#ifdef NOT
	
	/* HIP signature */
	hiph->hdr_len = (location/8) - 1; 
	location += build_tlv_signature(hip_a->hi, buff, location, FALSE);

#endif
	hiph->hdr_len = (location/8) - 1;
	hiph->checksum = 0;
	hiph->checksum = checksum_packet(buff, src, dst);

	/* send the packet */
	log_(NORMT, "sending UPDATE packet (%d bytes)...\n", location);

	/* Retransmit UPDATEs unless it contains a LOCATOR or address check */
	log_(NORM, "Sending UPDATE packet to dst : %s \n", logaddr(dst));
/*
	hip_check_bind(src, use_udp, HIP_UPDATE_BIND_CHECKS);
*/
	return(hip_send(buff, location, src, dst, NULL, retransmit, 0, 0));
}

/* HMAC the proxy update */

int build_tlv_proxy_hmac(hip_proxy_ticket *ticket, __u8 *data, int location,
			int type)
{
	hiphdr *hiph;
	tlv_hmac *hmac;
	unsigned int hmac_md_len;
	unsigned char hmac_md[EVP_MAX_MD_SIZE];
	
	/* compute HMAC over message */
	hiph = (hiphdr*) data;
	memset(hmac_md, 0, sizeof(hmac_md));
	hmac_md_len = EVP_MAX_MD_SIZE;
	
	switch (ticket->transform_type) {
	case ESP_AES_CBC_HMAC_SHA1:
	case ESP_3DES_CBC_HMAC_SHA1:
	case ESP_BLOWFISH_CBC_HMAC_SHA1:
	case ESP_NULL_HMAC_SHA1:
		HMAC(	EVP_sha1(), 
			ticket->hmac_key,
			auth_key_len(ticket->transform_type),
			data, location,
			hmac_md, &hmac_md_len  );
		break;		
	case ESP_3DES_CBC_HMAC_MD5:
	case ESP_NULL_HMAC_MD5:
		HMAC(	EVP_md5(), 
			ticket->hmac_key,
			auth_key_len(ticket->transform_type),
			data, location,
			hmac_md, &hmac_md_len  );
		break;
	default:
		return(0);
		break;
	}

	log_(NORM, "HMAC computed over %d bytes hdr length=%d\n ",
	    location, hiph->hdr_len);

	/* build tlv header */
	hmac = (tlv_hmac*)  &data[location];
	hmac->type = htons((__u16)type);
	hmac->length = htons(sizeof(tlv_hmac) - 4);
	
	/* get lower 160-bits of HMAC computation */
	memcpy( hmac->hmac, 
		&hmac_md[hmac_md_len-sizeof(hmac->hmac)],
		sizeof(hmac->hmac));

	return(eight_byte_align(sizeof(tlv_hmac)));
}

