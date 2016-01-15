#include "config.h"

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#include <sys/socket.h>
#ifdef HAVE_WINDOWS_H
#include <winsock.h>
#define __ANSI_CPP__
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#endif
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#include "evpath.h"
#include "cm_transport.h"

#if defined (__INTEL_COMPILER)
/*  Allow unordered operations */
#  pragma warning (disable: 981)
//  allow int conversions
#  pragma warning (disable: 2259)
#endif

static int ipv4_is_loopback(int addr)
{
  return (htonl(addr) & htonl(0xff000000)) == htonl(0x7f000000);
}

static int
get_self_ip_addr(CMTransport_trace trace_func, void* trace_data)
{
    struct hostent *host = NULL;
    char hostname_buf[256];
    char **p;
#ifdef HAVE_GETIFADDRS
  struct ifaddrs *if_addrs = NULL;
  struct ifaddrs *if_addr = NULL;
  void *tmp = NULL;
  char buf[INET6_ADDRSTRLEN];
#endif
#ifdef SIOCGIFCONF
    char *ifreqs;
    struct ifreq *ifr;
    struct sockaddr_in *sai;
    struct ifconf ifaces;
    int ifrn;
    int ss;
    int ipv4_count = 0;
    int ipv6_count = 0;
#endif
    int rv = 0;
#ifdef HAVE_GETIFADDRS
    if (getifaddrs(&if_addrs) == 0) {    
        char *interface;
	// Print possible addresses
	for (if_addr = if_addrs; if_addr != NULL; if_addr = if_addr->ifa_next) {
	    int family;
	    if (!if_addr->ifa_addr) continue;
	    family = if_addr->ifa_addr->sa_family;
	    if ((family != AF_INET) && (family != AF_INET6)) continue;
	    if (if_addr->ifa_addr->sa_family == AF_INET) {
	        tmp = &((struct sockaddr_in *)if_addr->ifa_addr)->sin_addr;
		ipv4_count++;
	    } else {
	        tmp = &((struct sockaddr_in6 *)if_addr->ifa_addr)->sin6_addr;
		ipv6_count++;
	    }
	    trace_func(trace_data, "CM<IP_CONFIG> IP possibility -> %s : %s",
		       if_addr->ifa_name,
		       inet_ntop(family, tmp, buf, sizeof(buf)));
	}
	if ((interface = getenv("CM_INTERFACE")) != NULL) {
	    for (if_addr = if_addrs; if_addr != NULL; if_addr = if_addr->ifa_next) {
	        int family;
	        if (!if_addr->ifa_addr) continue;
		family = if_addr->ifa_addr->sa_family;
		if (family != AF_INET) continue;  /* currently not looking for ipv6 */
		if (strcmp(if_addr->ifa_name, interface) != 0) continue;
		tmp = &((struct sockaddr_in *)if_addr->ifa_addr)->sin_addr;
		trace_func(trace_data, "CM<IP_CONFIG> Interface specified, returning ->%s : %s",
			   if_addr->ifa_name,
			   inet_ntop(family, tmp, buf, sizeof(buf)));
		free(if_addrs);
		return (ntohl(*(uint32_t*)tmp));
	    }
	    printf("Warning!  CM_INTERFACE specified as \"%s\", but no active interface by that name found\n", interface);
	}
	    
	gethostname(hostname_buf, sizeof(hostname_buf));
	if (index(hostname_buf, '.') != NULL) {
	    /* don't even check for host if not fully qualified */
	    host = gethostbyname(hostname_buf);
	}
	if (host != NULL) {
	    for (p = host->h_addr_list; *p != 0; p++) {
		struct in_addr *in = *(struct in_addr **) p;
		if (!ipv4_is_loopback(ntohl(in->s_addr))) {
		    trace_func(trace_data, "CM<IP_CONFIG> Prefer IP associated with hostname net -> %d.%d.%d.%d",
			       *((unsigned char *) &in->s_addr),
			       *(((unsigned char *) &in->s_addr) + 1),
			       *(((unsigned char *) &in->s_addr) + 2),
			       *(((unsigned char *) &in->s_addr) + 3));
		    free(if_addrs);
		    return (ntohl(in->s_addr));
		}
	    }
	}
	/* choose the first thing that's not a loopback interface */
	for (if_addr = if_addrs; if_addr != NULL; if_addr = if_addr->ifa_next) {
	    int family;
	    uint32_t ret_ip;
	    if (!if_addr->ifa_addr) continue;
	    family = if_addr->ifa_addr->sa_family;
	    if (family != AF_INET) continue;  /* currently not looking for ipv6 */
	    if ((if_addr->ifa_flags & IFF_LOOPBACK) != 0)  continue;
	    tmp = &((struct sockaddr_in *)if_addr->ifa_addr)->sin_addr;
	    trace_func(trace_data, "CM<IP_CONFIG> get_self_ip_addr returning first avail -> %s : %s",
			       if_addr->ifa_name,
			       inet_ntop(family, tmp, buf, sizeof(buf)));
	    ret_ip = (ntohl(*(uint32_t*)tmp));
	    free(if_addrs);
	    return ret_ip;
	}
    }
#endif	
    gethostname(hostname_buf, sizeof(hostname_buf));
    if (index(hostname_buf, '.') != NULL) {
	/* don't even check for host if not fully qualified */
	host = gethostbyname(hostname_buf);
    }
    if (host != NULL) {
	for (p = host->h_addr_list; *p != 0; p++) {
	    struct in_addr *in = *(struct in_addr **) p;
	    if (!ipv4_is_loopback(ntohl(in->s_addr))) {
		trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr %lx, net %d.%d.%d.%d",
			   ntohl(in->s_addr),
			   *((unsigned char *) &in->s_addr),
			   *(((unsigned char *) &in->s_addr) + 1),
			   *(((unsigned char *) &in->s_addr) + 2),
				   *(((unsigned char *) &in->s_addr) + 3));
		return (ntohl(in->s_addr));
	    }
	}
    }
    /*
     *  Since we couldn't find an IP address in some logical way, we'll open
     *  a DGRAM socket and ask it first for the list of its interfaces, and
     *  then checking for an interface we can use, and then finally asking that
     *  interface what its address is.
     */
#ifdef SIOCGIFCONF
    ss = socket(AF_INET, SOCK_DGRAM, 0);
    ifaces.ifc_len = 64 * sizeof(struct ifreq);
    ifaces.ifc_buf = ifreqs = malloc(ifaces.ifc_len);
    /*
     *  if we can't SIOCGIFCONF we're kind of dead anyway, bail.
     */
    if (ioctl(ss, SIOCGIFCONF, &ifaces) >= 0) {
	ifr = ifaces.ifc_req;
	ifrn = ifaces.ifc_len / sizeof(struct ifreq);
	for (; ifrn--; ifr++) {
	    /*
	     * Basically we'll take the first interface satisfying 
	     * the following: 
	     *   up, running, not loopback, address family is INET.
	     */
	    ioctl(ss, SIOCGIFFLAGS, ifr);
	    sai = (struct sockaddr_in *) &(ifr->ifr_addr);
	    if (ifr->ifr_flags & IFF_LOOPBACK) {
		trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr %lx, rejected, loopback",
			   ntohl(sai->sin_addr.s_addr));
		continue;
	    }
	    if (!(ifr->ifr_flags & IFF_UP)) {
		trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr %lx, rejected, not UP",
			   ntohl(sai->sin_addr.s_addr));
		continue;
	    }
	    if (!(ifr->ifr_flags & IFF_RUNNING)) {
		trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr %lx, rejected, not RUNNING",
			   ntohl(sai->sin_addr.s_addr));
		continue;
	    }
	    /*
	     * sure would be nice to test for AF_INET here but it doesn't
	     * cooperate and I've done enough for now ...
	     * if (sai->sin_addr.s.addr != AF_INET) continue;
	    */
	    if (sai->sin_addr.s_addr == INADDR_ANY)
		continue;
	    if (sai->sin_addr.s_addr == INADDR_LOOPBACK)
		continue;
	    rv = ntohl(sai->sin_addr.s_addr);
	    trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr DHCP %lx, net %d.%d.%d.%d",
		       ntohl(sai->sin_addr.s_addr),
		       *((unsigned char *) &sai->sin_addr.s_addr),
		       *(((unsigned char *) &sai->sin_addr.s_addr) + 1),
		       *(((unsigned char *) &sai->sin_addr.s_addr) + 2),
		       *(((unsigned char *) &sai->sin_addr.s_addr) + 3));
	    break;
	}
    }
    close(ss);
    free(ifreqs);
#endif
    /*
     *  Absolute last resort.  If we can't figure out anything else, look
     *  for the CM_LAST_RESORT_IP_ADDR environment variable.
     */
    if (rv == 0) {
	char *c = getenv("CM_LAST_RESORT_IP_ADDR");
	trace_func(trace_data, "CM<IP_CONFIG> - Get self IP addr at last resort");
	if (c != NULL) {
	    trace_func(trace_data, "CM<IP_CONFIG> - Translating last resort %s", c);
	    rv = inet_addr(c);
	}
    }
    /*
     *	hopefully by now we've set rv to something useful.  If not,
     *  GET A BETTER NETWORK CONFIGURATION.
     */
    return rv;
}

static int
is_private_IP(int IP)
{
    if ((IP & 0xffff0000) == 0xC0A80000) return 1;	/* equal 192.168.x.x */
    if ((IP & 0xffff0000) == 0xB6100000) return 1;	/* equal 182.16.x.x */
    if ((IP & 0xff000000) == 0x0A000000) return 1;	/* equal 10.x.x.x */
    return 0;
}

static void
get_qual_hostname(char *buf, int len, attr_list attrs,
		  int *network_p, CMTransport_trace trace_func, void *trace_data)
{
    struct hostent *host = NULL;

    char *network_string = getenv("CM_NETWORK");
    char *hostname_string = getenv("CERCS_HOSTNAME");
    if (hostname_string != NULL) {
	strncpy(buf, hostname_string, len);
	return;
    }
    (void)get_qual_hostname;
    gethostname(buf, len);
    buf[len - 1] = '\0';
    if (memchr(buf, '.', strlen(buf)) == NULL) {
	/* no dots, probably not fully qualified */
#ifdef HAVE_GETDOMAINNAME
	int end = strlen(buf);
	buf[end] = '.';
	if (getdomainname((&buf[end]) + 1, len - end - 1) == -1) {
	    buf[end+1]=0;
	}
	if (buf[end + 1] == 0) {
	    char *tmp_name;
	    struct hostent *host = gethostbyname(buf);
	    buf[end] = 0;
	    /* getdomainname was useless, hope that gethostbyname helps */
	    if (host) {
		tmp_name = host->h_name;
		strncpy(buf, tmp_name, len);
	    }		
	}
#else
	{
	    /* no getdomainname, hope that gethostbyname will help */
	    struct hostent *he = gethostbyname(buf);
	    char *tmp_name;
	    if (he) {
		tmp_name = (gethostbyname(buf))->h_name;
		strncpy(buf, tmp_name, len);
	    }
	}
#endif
	buf[len - 1] = '\0';
    }
    trace_func(trace_data, "CM<IP_CONFIG> - Tentative Qualified hostname %s", buf);
    if (memchr(buf, '.', strlen(buf)) == NULL) {
	/* useless hostname if it's not fully qualified */
	buf[0] = 0;
    }
    if ((buf[0] != 0) && ((host = gethostbyname(buf)) == NULL)) {
	/* useless hostname if we can't translate it */
	buf[0] = 0;
    }
    if (host != NULL) {
	char **p;
	int good_addr = 0;
	for (p = host->h_addr_list; *p != 0; p++) {
	    struct in_addr *in = *(struct in_addr **) p;
	    if (!ipv4_is_loopback(ntohl(in->s_addr))) {
		good_addr++;
		trace_func(trace_data, "CM<IP_CONFIG> - Hostname gets good addr %lx, %d.%d.%d.%d",
			   ntohl(in->s_addr),
			   *((unsigned char *) &in->s_addr),
			   *(((unsigned char *) &in->s_addr) + 1),
			   *(((unsigned char *) &in->s_addr) + 2),
			   *(((unsigned char *) &in->s_addr) + 3));
	    }
	}
	if (good_addr == 0) {
	    /* 
	     * even a fully qualifiedhostname that doesn't get us a valid
	     * IP addr is useless
	     */
	    buf[0] = 0;
	}
    }
    if (buf[0] == 0) {
	/* bloody hell, what do you have to do? */
	struct in_addr IP;
	extern int h_errno;
	IP.s_addr = htonl(get_self_ip_addr(trace_func, trace_data));
	trace_func(trace_data, "CM<IP_CONFIG> - No hostname yet, trying gethostbyaddr on IP %lx", IP);
	if (!is_private_IP(ntohl(IP.s_addr))) {
	    host = gethostbyaddr((char *) &IP, sizeof(IP), AF_INET);
	    if (host != NULL) {
		trace_func(trace_data, "     result was %s", host->h_name);
		strncpy(buf, host->h_name, len);
	    } else {
		trace_func(trace_data, "     FAILED, errno %d", h_errno);
	    }
	}
    }
    if (network_string == NULL) {
	static atom_t CM_NETWORK_POSTFIX = -1;
	if (CM_NETWORK_POSTFIX == -1) {
	    CM_NETWORK_POSTFIX = attr_atom_from_string("CM_NETWORK_POSTFIX");
	}
	if (!get_string_attr(attrs, CM_NETWORK_POSTFIX, &network_string)) {
	    trace_func(trace_data, "TCP/IP transport found no NETWORK POSTFIX attribute");
	} else {
	    trace_func(trace_data, "TCP/IP transport found NETWORK POSTFIX attribute %s", network_string);
	}
    }
    if (network_string != NULL) {
	int name_len = strlen(buf) + 2 + strlen(network_string);
	char *new_name_str = malloc(name_len);
	char *first_dot = strchr(buf, '.');

	/* stick the CM_NETWORK value in there */
	memset(new_name_str, 0, name_len);
	*first_dot = 0;
	first_dot++;
	sprintf(new_name_str, "%s%s.%s", buf, network_string, first_dot);
	if (gethostbyname(new_name_str) != NULL) {
	    /* host has no NETWORK interface */
	    strcpy(buf, new_name_str);
	    if (network_p) (*network_p)++;
	}
	free(new_name_str);
    }

    if ((buf[0] == 0) ||
	((host = gethostbyname(buf)) == NULL) ||
	(memchr(buf, '.', strlen(buf)) == NULL)) {
	/* just use the bloody IP address */
	struct in_addr IP;
	IP.s_addr = htonl(get_self_ip_addr(trace_func, trace_data));
	if (IP.s_addr != 0) {
	    struct in_addr ip;
	    char *tmp;
	    ip.s_addr = htonl(get_self_ip_addr(trace_func, trace_data));
	    tmp = inet_ntoa(ip);
	    strncpy(buf, tmp, len);
	} else {
	    static int warn_once = 0;
	    if (warn_once == 0) {
		warn_once++;
		trace_func(trace_data, "Attempts to establish your fully qualified hostname, or indeed any\nuseful network name, have failed horribly.  using localhost.\n");
	    }
	    strncpy(buf, "localhost", len);
	}
    }
    trace_func(trace_data, "CM<IP_CONFIG> - GetQualHostname returning %s", buf);
}

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif
extern void
get_IP_config(char *hostname_buf, int len, int* IP_p, int *port_range_low_p, int *port_range_high_p, 
	      int *use_hostname_p, attr_list attrs, CMTransport_trace trace_func, void *trace_data)
{
    static int first_call = 1;
    static char determined_hostname[HOST_NAME_MAX+1];
    static int determined_IP = -1;
    static int port_range_low = 26000, port_range_high = 26100;
    static int use_hostname = 0;
    if (first_call) {
	char *preferred_hostname = getenv("CM_HOSTNAME");
	char *port_range = getenv("CM_PORT_RANGE");
	first_call = 0;
	determined_hostname[0] = 0;
	
	if (preferred_hostname != NULL) {
	    struct hostent *host;
	    use_hostname = 1;
	    trace_func(trace_data, "CM<IP_CONFIG> CM_HOSTNAME set to \"%s\", running with that.", preferred_hostname);
	    host = gethostbyname(preferred_hostname);
	    strcpy(determined_hostname, preferred_hostname);
	    if (!host) {
		printf("Warning, CM_HOSTNAME is \"%s\", but gethostbyname fails for that string.\n", preferred_hostname);
	    } else {
		char **p;
		for (p = host->h_addr_list; *p != 0; p++) {
		    struct in_addr *in = *(struct in_addr **) p;
		    if (!ipv4_is_loopback(ntohl(in->s_addr))) {
			trace_func(trace_data, "CM IP_CONFIG Prefer IP associated with hostname net -> %d.%d.%d.%d",
				   *((unsigned char *) &in->s_addr),
				   *(((unsigned char *) &in->s_addr) + 1),
				   *(((unsigned char *) &in->s_addr) + 2),
				   *(((unsigned char *) &in->s_addr) + 3));
			determined_IP  = (ntohl(in->s_addr));
		    }
		}
	    }
	} else {
	    get_qual_hostname(determined_hostname, sizeof(determined_hostname), attrs, NULL, trace_func, trace_data);
	}
	if (determined_IP == -1) {
	    /* I.E. the specified hostname didn't determine what IP we should use */
	    determined_IP = get_self_ip_addr(trace_func, trace_data);
	}
	if (port_range != NULL) {
	    if (sscanf(port_range, "%d:%d", &port_range_high, &port_range_low) != 2) {
		printf("CM_PORT_RANGE spec not understood \"%s\"\n", port_range);
	    } else {
		if (port_range_high < port_range_low) {
		    int tmp = port_range_high;
		    port_range_high = port_range_low;
		    port_range_low = tmp;
		}
	    }
	}
    }

    if (hostname_buf && (len > strlen(determined_hostname))) {
	strcpy(hostname_buf, determined_hostname);
    }
    if (IP_p && (determined_IP != -1)) {
	*IP_p = determined_IP;
    }
    
    if (port_range_low_p) {
	*port_range_low_p = port_range_low;
    }
    if (port_range_high_p) {
	*port_range_high_p = port_range_high;
    }
    if (use_hostname_p) {
	*use_hostname_p = use_hostname;
    }
    {
	char buf[256];
	int net_byte_order = htonl(determined_IP);
	trace_func(trace_data, "CM<IP_CONFIG> returning hostname \"%s\", IP %s, use_hostname = %d, port range %d:%d",
		   determined_hostname, inet_ntop(AF_INET, &net_byte_order, &buf[0], 256), use_hostname, port_range_low, port_range_high);
    }
}
